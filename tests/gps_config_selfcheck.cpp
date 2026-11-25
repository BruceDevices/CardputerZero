/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for GPS > Config (model::gps_config: ~/.bruce/gps.conf
 * parse / validate / save, env precedence) and the GPS port discovery used by
 * Config > GPS Port and the "Auto" port (platform::gps port_candidates /
 * auto_detect) - on ptys and a fake /dev tree, so no GPS or USB dongle is
 * needed. Not part of the CMake build. Run manually (Linux, non-root for the
 * permission case):
 *
 *   g++ -std=c++17 -pthread -Isrc/model -Isrc/platform tests/gps_config_selfcheck.cpp \
 *       src/model/gps_config.cpp src/platform/gps_port.cpp src/platform/gps_parser.cpp \
 *       src/platform/uart_speed.cpp -lutil -o /tmp/gps_config_check && /tmp/gps_config_check
 */

#include "gps_config.h"
#include "gps_port.h"
#include "uart_speed.h"

#include <fcntl.h>
#include <pty.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_checks = 0;
int g_failed = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failed;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

namespace gc = model::gps_config;
namespace gps = platform::gps;
using V = std::vector<std::string>;

std::string temp_dir() {
    static std::string dir;
    if (dir.empty()) {
        char tmpl[] = "/tmp/gps_config_check.XXXXXX";
        dir = ::mkdtemp(tmpl);
    }
    return dir;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void touch(const std::string& path, mode_t mode = 0644) {
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, mode);
    if (fd >= 0) {
        ::close(fd);
    }
}

constexpr const char* kNmea =
    "$GNGGA,152010.00,3540.87416,N,13946.02750,E,1,07,1.20,41.2,M,39.0,M,,*7B\r\n"
    "$GNRMC,152010.00,A,3540.87416,N,13946.02750,E,0.50,10.0,221126,,,A,V*3A\r\n";

// A pty whose master streams NMEA every 200 ms while `run` is set.
struct NmeaPty {
    int master = -1, slave = -1;
    std::string name;
    std::atomic<bool> run{false};
    std::thread writer;

    NmeaPty() {
        char buf[128] = {};
        if (::openpty(&master, &slave, buf, nullptr, nullptr) == 0) {
            name = buf;
        }
    }
    void stream(const char* text) {
        run = true;
        writer = std::thread([this, text]() {
            while (run) {
                (void)!::write(master, text, std::strlen(text));
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
    }
    ~NmeaPty() {
        run = false;
        if (writer.joinable()) {
            writer.join();
        }
        if (slave >= 0) ::close(slave);
        if (master >= 0) ::close(master);
    }
};

void test_validate_parse() {
    CHECK(gc::baud_rates() == (std::vector<int>{9600, 19200, 38400, 57600, 115200}));
    for (int b : gc::baud_rates()) CHECK(gc::validate_baud(b) == b);
    for (int b : {0, -1, 4800, 1200, 230400, 115201}) CHECK(gc::validate_baud(b) == 9600);

    CHECK(gc::valid_port("/dev/serial0") && gc::valid_port("Auto") &&
          gc::valid_port("/dev/serial/by-id/usb-u-blox_AG_-_www.u-blox.com_u-blox_GNSS_receiver-if00"));
    CHECK(!gc::valid_port("") && !gc::valid_port("auto") && !gc::valid_port("ttyUSB0") &&
          !gc::valid_port("/dev/tty USB0") && !gc::valid_port("/") && !gc::valid_port("/dev/x\n"));

    const gc::GpsConfig cap = gc::board_default(true);
    const gc::GpsConfig other = gc::board_default(false);
    CHECK(cap.port == "/dev/serial0" && cap.baud == 115200);
    CHECK(other.port == "/dev/serial0" && other.baud == 9600);

    CHECK(gc::parse("", cap) == cap);
    gc::GpsConfig c = gc::parse("# c\n  gpsBaudrate = 38400 \r\ngpsPort=/dev/ttyACM0\nfoo=bar\nnoeq\n", cap);
    CHECK(c.baud == 38400 && c.port == "/dev/ttyACM0");
    c = gc::parse("gpsPort=Auto\n", other);
    CHECK(c.port == "Auto" && c.baud == 9600);
    // Bruce validation: a present but unknown baud -> 9600, not the board default.
    CHECK(gc::parse("gpsBaudrate=4800\n", cap).baud == 9600);
    CHECK(gc::parse("gpsBaudrate=fast\n", cap).baud == 9600);
    CHECK(gc::parse("gpsBaudrate=\n", cap).baud == 9600);
    // A bad port keeps the default.
    CHECK(gc::parse("gpsPort=COM3\n", cap).port == "/dev/serial0");
    CHECK(gc::parse("gpsPort=\n", cap).port == "/dev/serial0");
    // Last one wins, like re-reading a key.
    CHECK(gc::parse("gpsBaudrate=19200\ngpsBaudrate=57600\n", cap).baud == 57600);

    CHECK(gc::serialize({115200, "Auto"}) ==
          "# Bruce CardputerZero GPS settings (GPS > Config)\ngpsBaudrate=115200\ngpsPort=Auto\n");
    CHECK(gc::parse(gc::serialize({19200, "/dev/ttyUSB3"}), other) == (gc::GpsConfig{19200, "/dev/ttyUSB3"}));
    std::printf("ok   config: Bruce baud validation, port validation, parse, board defaults\n");
}

void test_load_save_resolve() {
    const std::string home = temp_dir() + "/home";
    ::mkdir(home.c_str(), 0755);
    ::unsetenv("BRUCE_GPS_CONF");
    ::setenv("HOME", home.c_str(), 1);
    ::unsetenv("GPS_UART");
    ::unsetenv("GPS_BAUD");
    const std::string path = home + "/.bruce/gps.conf";
    CHECK(gc::config_path() == path);

    gc::set_board_default(gc::board_default(true));
    CHECK(gc::load() == gc::board_default(true)); // no file -> board default

    std::string err;
    CHECK(gc::save({57600, "/dev/ttyACM1"}, &err)); // creates ~/.bruce
    struct stat st{};
    CHECK(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0644 && st.st_uid == ::getuid());
    CHECK(::stat((home + "/.bruce").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(slurp(path).find("gpsBaudrate=57600\ngpsPort=/dev/ttyACM1\n") != std::string::npos);
    CHECK(gc::load() == (gc::GpsConfig{57600, "/dev/ttyACM1"}));
    // No temp file left behind.
    CHECK(::access((path + ".tmp." + std::to_string(::getpid())).c_str(), F_OK) != 0);
    // Invalid port is refused, file untouched.
    CHECK(!gc::save({9600, "nope"}, &err) && err.find("invalid") != std::string::npos);
    CHECK(gc::load().port == "/dev/ttyACM1");
    // Unwritable dir -> error, no crash.
    ::setenv("BRUCE_GPS_CONF", "/nonexistent/dir/gps.conf", 1);
    CHECK(!gc::save({9600, "/dev/serial0"}, &err) && !err.empty());
    ::unsetenv("BRUCE_GPS_CONF");

    // Precedence: env > file > board default.
    gc::Effective e = gc::resolve(gc::load());
    CHECK(e.port == "/dev/ttyACM1" && e.baud == 57600 && !e.port_from_env && !e.baud_from_env);
    ::setenv("GPS_UART", "/dev/ttyUSB9", 1);
    ::setenv("GPS_BAUD", "4800", 1); // the env hook is taken as is
    e = gc::resolve(gc::load());
    CHECK(e.port == "/dev/ttyUSB9" && e.baud == 4800 && e.port_from_env && e.baud_from_env);
    ::setenv("GPS_BAUD", "0", 1);
    CHECK(gc::resolve(gc::load()).baud == 57600);
    ::unsetenv("GPS_UART");
    ::unsetenv("GPS_BAUD");
    std::printf("ok   config: ~/.bruce/gps.conf load/save (0644, user-owned, atomic), env > file > board\n");
}

void test_candidates() {
    const std::string dev = temp_dir() + "/dev";
    ::mkdir(dev.c_str(), 0755);
    CHECK(gps::port_candidates_in(dev).empty());
    for (const char* n : {"ttyUSB10", "ttyUSB2", "ttyACM0", "ttyS0", "ttyAMA0", "ttyUSBx"}) {
        touch(dev + "/" + n);
    }
    ::symlink("ttyS0", (dev + "/serial0").c_str());
    ::mkdir((dev + "/serial").c_str(), 0755);
    ::mkdir((dev + "/serial/by-id").c_str(), 0755);
    ::symlink("../../ttyACM0", (dev + "/serial/by-id/usb-u-blox_GNSS-if00").c_str());
    const V got = gps::port_candidates_in(dev);
    // serial0 first, by-id, then ACM (deduplicated: its by-id link already
    // listed it), then USB in natural order; ttyS0/ttyAMA0 are not GPS nodes.
    CHECK(got == (V{dev + "/serial0", dev + "/serial/by-id/usb-u-blox_GNSS-if00", dev + "/ttyUSB2",
                    dev + "/ttyUSB10", dev + "/ttyUSBx"}));

    CHECK(gps::auto_order("/dev/serial0", {"/dev/ttyACM0", "/dev/serial0", "/dev/ttyUSB0"}) ==
          (V{"/dev/serial0", "/dev/ttyACM0", "/dev/ttyUSB0"}));
    CHECK(gps::auto_order("/dev/serial0", {"/dev/ttyACM0"}) == (V{"/dev/ttyACM0"}));

    // Test hook: an explicit list; only existing entries, no duplicates.
    const std::string env = dev + "/ttyACM0:/nonexistent/tty:" + dev + "/ttyUSB2:" + dev + "/ttyACM0";
    ::setenv("BRUCE_GPS_PORT_CANDIDATES", env.c_str(), 1);
    CHECK(gps::port_candidates() == (V{dev + "/ttyACM0", dev + "/ttyUSB2"}));
    ::unsetenv("BRUCE_GPS_PORT_CANDIDATES");
    std::printf("ok   ports: /dev/serial0, by-id, ttyACM*, ttyUSB* (natural order, dedup), env hook\n");
}

void test_auto_probe() {
    const std::vector<int> bauds = gc::baud_rates();

    // No candidates at all.
    gps::ProbeResult r = gps::auto_detect({}, 115200, bauds, 300);
    CHECK(r.kind == gps::ProbeResult::Kind::NoCandidates && r.message.find("GPS not Found!") == 0);

    // Streaming pty after a silent one: the silent one times out at every
    // baud, the streaming one is found at the preferred baud.
    {
        NmeaPty silent, live;
        CHECK(!silent.name.empty() && !live.name.empty());
        live.stream(kNmea);
        const auto t0 = std::chrono::steady_clock::now();
        r = gps::auto_detect({silent.name, live.name}, 115200, bauds, 600);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(r.kind == gps::ProbeResult::Kind::Found);
        CHECK(r.port == live.name && r.baud == 115200);
        CHECK(ms < 600 + 600 + 300); // silent window + a burst on the live one
        // The probe released both ports (a Session-style exclusive lock works again).
        const int fd = ::open(live.name.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
        CHECK(fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0);
        if (fd >= 0) ::close(fd);
    }

    // Garbage (no valid checksum) is not NMEA.
    {
        NmeaPty noisy;
        noisy.stream("$GNRMC,garbage*00\r\n\x01\x02 hello\r\n");
        r = gps::auto_detect({noisy.name}, 9600, {9600}, 500);
        CHECK(r.kind == gps::ProbeResult::Kind::NoNmea && r.message.find("no NMEA on 1 port") != std::string::npos);
    }

    // A file of NMEA (the GPS_UART test style) is found too.
    {
        const std::string file = temp_dir() + "/gps.nmea";
        std::ofstream(file) << kNmea;
        r = gps::auto_detect({"/nonexistent/ttyUSB0", file}, 9600, bauds, 300);
        CHECK(r.kind == gps::ProbeResult::Kind::Found && r.port == file);
    }

    if (::geteuid() != 0) {
        // EACCES (not in dialout) on the only port with no NMEA elsewhere ->
        // the popup names the dialout group.
        const std::string denied = temp_dir() + "/ttyACM9";
        touch(denied, 0000);
        NmeaPty silent;
        r = gps::auto_detect({denied, silent.name}, 9600, {9600, 115200}, 300);
        CHECK(r.kind == gps::ProbeResult::Kind::Permission);
        CHECK(r.message == "Cannot open " + denied + ": permission denied - add your user to the dialout group");
        CHECK(gps::probe_port(denied, 9600, 100, nullptr) == EACCES);

        // EBUSY: held exclusively (gpsd-style TIOCEXCL, or our own Session's flock).
        NmeaPty held;
        held.stream(kNmea);
        CHECK(::ioctl(held.slave, TIOCEXCL) == 0);
        r = gps::auto_detect({held.name}, 9600, {9600}, 300);
        CHECK(r.kind == gps::ProbeResult::Kind::Busy && r.message.find("busy") != std::string::npos);
        NmeaPty locked;
        locked.stream(kNmea);
        const int lfd = ::open(locked.name.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
        CHECK(lfd >= 0 && ::flock(lfd, LOCK_EX | LOCK_NB) == 0);
        CHECK(gps::probe_port(locked.name, 9600, 300, nullptr) == EBUSY);
        // ... while a free, streaming port next to them still wins.
        NmeaPty live;
        live.stream(kNmea);
        r = gps::auto_detect({held.name, locked.name, denied, live.name}, 9600, {9600}, 600);
        CHECK(r.kind == gps::ProbeResult::Kind::Found && r.port == live.name);
        ::close(lfd);
        std::printf("ok   auto: streaming pty found, silent/garbage -> no NMEA, EACCES -> dialout, EBUSY -> busy\n");
    } else {
        std::printf("skip auto: EACCES/EBUSY cases need a non-root user\n");
    }

    // Cancel: a long probe of a silent port returns within ~100 ms.
    {
        NmeaPty silent;
        std::atomic<bool> cancel{false};
        gps::ProbeResult out;
        const auto t0 = std::chrono::steady_clock::now();
        std::thread th([&]() { out = gps::auto_detect({silent.name}, 9600, bauds, 5000, &cancel); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel = true;
        th.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(out.kind == gps::ProbeResult::Kind::Cancelled);
        CHECK(ms < 400);
    }

    // Fallback bauds: the preferred one first, then the rest in Bruce's order;
    // a missing node is not retried per baud.
    {
        const auto t0 = std::chrono::steady_clock::now();
        r = gps::auto_detect({"/nonexistent/ttyUSB0"}, 115200, bauds, 1000);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(r.kind == gps::ProbeResult::Kind::NoNmea && ms < 100);
    }
    std::printf("ok   auto: cancel within 100 ms, missing nodes skipped\n");
}

// A receiver that only talks at one speed: the writer streams NMEA only while
// the pty's line speed (set by the probe's termios2 call) equals `baud`.
struct BaudGatedPty : NmeaPty {
    void stream_at(int baud) {
        run = true;
        writer = std::thread([this, baud]() {
            while (run) {
                if (platform::uart_get_speed(slave) == baud) {
                    (void)!::write(master, kNmea, std::strlen(kNmea));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
};

void test_fallback_baud_and_hangup() {
    const std::vector<int> bauds = gc::baud_rates();
    // Only at 19200 (not the pty's initial 38400, not the preferred 115200):
    // found on the fallback pass, reported with that baud.
    {
        BaudGatedPty gated;
        NmeaPty silent;
        CHECK(!gated.name.empty() && !silent.name.empty());
        gated.stream_at(19200);
        const auto r = gps::auto_detect({silent.name, gated.name}, 115200, bauds, 400);
        CHECK(r.kind == gps::ProbeResult::Kind::Found);
        CHECK(r.port == gated.name);
        CHECK(r.baud == 19200);
        // ...and nothing at the preferred baud alone.
        const auto only = gps::auto_detect({gated.name}, 115200, {115200}, 400);
        CHECK(only.kind == gps::ProbeResult::Kind::NoNmea);
    }
    // Hang-up during a probe (pty master closed = USB GPS unplugged): the
    // probe ends at once with EIO instead of spinning on POLLHUP until its
    // deadline. Repeated: after the hang-up the kernel's read() returns
    // either 0 or EIO, and only the 0 case used to spin.
    long long worst_ms = 0;
    double worst_cpu = 0;
    for (int round = 0; round < 5; ++round) {
        NmeaPty pty;
        CHECK(!pty.name.empty());
        int rc = -1;
        double cpu_ms = -1;
        const auto t0 = std::chrono::steady_clock::now();
        std::thread th([&]() {
            timespec a{}, b{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
            rc = gps::probe_port(pty.name, 9600, 1500, nullptr);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
            cpu_ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ::close(pty.master);
        pty.master = -1;
        th.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(rc == EIO);
        CHECK(ms < 600);
        CHECK(cpu_ms >= 0 && cpu_ms < 100);
        worst_ms = std::max<long long>(worst_ms, ms);
        worst_cpu = std::max(worst_cpu, cpu_ms);
    }
    std::printf("ok   auto: 19200-only receiver found on the fallback pass; hang-up -> EIO, worst of 5: %lld ms, "
                "%.1f ms CPU\n", worst_ms, worst_cpu);
}

} // namespace

int main() {
    test_validate_parse();
    test_load_save_resolve();
    test_candidates();
    test_auto_probe();
    test_fallback_baud_and_hangup();
    std::string cmd = "rm -rf '" + temp_dir() + "'";
    (void)!std::system(cmd.c_str());
    if (g_failed) {
        std::printf("gps_config_selfcheck: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    std::printf("gps_config_selfcheck: all %d checks passed\n", g_checks);
    return 0;
}
