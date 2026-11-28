/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the native GPS reader (TinyGPS++-compatible
 * parser), the serial Session (pty / error paths / idle CPU / snapshots), the
 * GPX storage worker and the Bruce GPS Tracker port (screen texts, timing, GPX
 * output). Not part of the CMake build. Run manually (Linux):
 *
 *   g++ -std=c++17 -pthread -Isrc/platform tests/gps_reader_selfcheck.cpp \
 *       src/platform/gps_parser.cpp src/platform/gps_session.cpp \
 *       src/platform/gps_tracker.cpp src/platform/gpx_writer.cpp \
 *       src/platform/gps_port.cpp src/platform/uart_speed.cpp -lutil -o /tmp/gps_reader_check && /tmp/gps_reader_check
 *
 * The packaged executable is checked by tests/gps_tracker_cli_check.sh.
 *
 * The no-fix sentences are a verbatim capture from the LoRa+GNSS Cap
 * (ATGM336H, /dev/ttyS0 @115200, indoors, antenna unplugged) on 2026-11-23.
 */

#include "gps_parser.h"
#include "gps_session.h"
#include "gps_tracker.h"

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <csignal>
#include <sched.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace platform::gps;

namespace {

int g_checks = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

// One second of the Cap's output with no fix (captured 15:20:02 UTC).
const char* const kCapNoFix[] = {
    "$GNGGA,152002.00,,,,,0,00,25.5,,,,,,*4E",
    "$GNGLL,,,,,152002.00,V,N*50",
    "$GNGSA,A,1,,,,,,,,,,,,,25.5,25.5,25.5,1*01",
    "$GNGSA,A,1,,,,,,,,,,,,,25.5,25.5,25.5,2*02",
    "$GNGSA,A,1,,,,,,,,,,,,,25.5,25.5,25.5,3*03",
    "$GNGSA,A,1,,,,,,,,,,,,,25.5,25.5,25.5,4*04",
    "$GNGSA,A,1,,,,,,,,,,,,,25.5,25.5,25.5,5*05",
    "$GPGSV,1,1,00,1*64",
    "$GLGSV,1,1,00,1*78",
    "$GAGSV,1,1,00,7*73",
    "$BDGSV,1,1,00,1*75",
    "$GQGSV,1,1,00,1*65",
    "$GNRMC,152002.00,V,,,,,,,221126,,,N,V*19",
    "$GNVTG,,,,,,,,,N*2E",
    "$GNZDA,152002.00,22,11,2026,00,00*7A",
    "$GPTXT,01,01,01,ANTENNA OPEN*25",
};

std::string burst(const char* const* lines, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += lines[i];
        s += "\r\n";
    }
    return s;
}

std::string cap_no_fix() {
    return burst(kCapNoFix, sizeof(kCapNoFix) / sizeof(kCapNoFix[0]));
}

// "$<body>*HH\r\n" with a correct checksum.
std::string nmea(const std::string& body) {
    unsigned char sum = 0;
    for (char c : body) {
        sum ^= static_cast<unsigned char>(c);
    }
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
    return "$" + body + tail;
}

// A synthetic fix at Tokyo station: 35.681236 N, 139.767125 E, 7 sats, HDOP 1.2.
std::string fix_burst(const char* hhmmss, const char* lat, const char* lng) {
    return nmea(std::string("GNGGA,") + hhmmss + ".00," + lat + ",N," + lng + ",E,1,07,1.20,41.2,M,39.0,M,,") +
           nmea(std::string("GNRMC,") + hhmmss + ".00,A," + lat + ",N," + lng + ",E,0.50,10.0,221126,,,A,V") +
           nmea("GPTXT,01,01,01,ANTENNA OK");
}

void feed(Parser& p, const std::string& s) {
    p.encode(s.data(), s.size());
}

bool near(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> texts(const ScreenFrame& f) {
    std::vector<std::string> out;
    for (const auto& l : f.lines) {
        out.push_back(l.text);
    }
    return out;
}

std::string temp_dir() {
    char tmpl[] = "/tmp/gps_reader_check_XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    CHECK(d != nullptr);
    return d;
}

// --- parser -----------------------------------------------------------------------

void test_parser_cap_no_fix() {
    Parser p;
    feed(p, cap_no_fix());
    CHECK(p.failedChecksum() == 0);
    CHECK(p.passedChecksum() == sizeof(kCapNoFix) / sizeof(kCapNoFix[0]));
    // No fix: location never committed (RMC 'V', GGA quality 0).
    CHECK(!p.location.isValid());
    CHECK(!p.location.isUpdated());
    // ... but the Cap's RTC time/date commit on every RMC, like TinyGPS++.
    CHECK(p.date.isUpdated() && p.time.isUpdated());
    CHECK(p.date.value() == 221126);
    CHECK(p.date.year() == 2026 && p.date.month() == 11 && p.date.day() == 22);
    CHECK(!p.date.isUpdated()); // reading clears isUpdated(), as in TinyGPS++
    CHECK(p.time.hour() == 15 && p.time.minute() == 20 && p.time.second() == 2);
    CHECK(p.satellites.value() == 0);
    CHECK(near(p.hdop.hdop(), 25.5, 1e-9));
    CHECK(p.antenna() == Antenna::Open);
    std::printf("ok   parser: Cap capture (no fix, ANTENNA OPEN, $BD/$GQ/$GL/$GA)\n");
}

void test_parser_checksums() {
    Parser p;
    feed(p, "$GNRMC,152002.00,V,,,,,,,221126,,,N,V*18\r\n"); // wrong checksum
    CHECK(p.failedChecksum() == 1 && !p.date.isValid());
    feed(p, "$GNRMC,152002.00,V,,,,,,,221126,,,N,V\r\n"); // none: TinyGPS++ never commits
    CHECK(p.failedChecksum() == 1 && !p.date.isValid());     // ... and does not count it
    feed(p, "garbage at the wrong baud \xff\xfe$GNRMC,15"); // runaway/partial
    feed(p, std::string(200, 'x') + "\r\n");
    CHECK(!p.date.isValid());
    feed(p, nmea("GNRMC,152002.00,V,,,,,,,221126,,,N,V"));
    CHECK(p.date.isValid());
    std::printf("ok   parser: checksum required, garbage tolerated\n");
}

void test_parser_fix() {
    Parser p;
    feed(p, fix_burst("152010", "3540.87416", "13946.02750"));
    CHECK(p.location.isValid() && p.location.isUpdated());
    CHECK(near(p.location.lat(), 35.0 + 40.87416 / 60.0, 1e-9));
    CHECK(!p.location.isUpdated()); // lat() clears it
    CHECK(near(p.location.lng(), 139.0 + 46.0275 / 60.0, 1e-9));
    CHECK(p.satellites.value() == 7);
    CHECK(near(p.hdop.hdop(), 1.2, 1e-9));
    CHECK(near(p.altitude.meters(), 41.2, 1e-9));
    CHECK(p.antenna() == Antenna::Ok);
    char coord[64];
    std::snprintf(coord, sizeof(coord), "%.6f, %.6f", p.location.lat(), p.location.lng());
    CHECK(std::string(coord) == "35.681236, 139.767125");

    // Southern/western hemispheres are negative.
    feed(p, nmea("GNRMC,010203.00,A,3352.12800,S,15112.50000,W,0.0,0.0,010127,,,A,V"));
    CHECK(near(p.location.lat(), -(33.0 + 52.128 / 60.0), 1e-9));
    CHECK(near(p.location.lng(), -(151.0 + 12.5 / 60.0), 1e-9));

    // A BeiDou-only receiver ($BD talker) commits too.
    Parser bd;
    feed(bd, nmea("BDRMC,152010.00,A,3540.87416,N,13946.02750,E,0.5,10.0,221126,,,A,V"));
    CHECK(bd.location.isUpdated());
    // An unknown talker does not.
    Parser xx;
    feed(xx, nmea("XXRMC,152010.00,A,3540.87416,N,13946.02750,E,0.5,10.0,221126,,,A,V"));
    CHECK(!xx.location.isValid());

    // A GGA without a fix still commits time/satellites/HDOP but not location.
    Parser gga;
    feed(gga, nmea("GNGGA,152011.00,,,,,0,03,9.90,,,,,,"));
    CHECK(!gga.location.isValid() && gga.time.isUpdated() && !gga.date.isUpdated());
    CHECK(gga.satellites.value() == 3 && near(gga.hdop.hdop(), 9.9, 1e-9));
    std::printf("ok   parser: fix, hemispheres, $BD talker, isUpdated() semantics\n");
}

void test_distance() {
    const double one_deg = 6371009.0 * 3.14159265358979323846 / 180.0;
    CHECK(near(Parser::distanceBetween(0, 0, 0, 1), one_deg, 1e-6));
    CHECK(near(Parser::distanceBetween(0, 0, 1, 0), one_deg, 1e-6));
    const double tokyo_osaka = Parser::distanceBetween(35.681236, 139.767125, 34.702485, 135.495951);
    CHECK(near(tokyo_osaka, Parser::distanceBetween(34.702485, 135.495951, 35.681236, 139.767125), 1e-6));
    CHECK(tokyo_osaka > 400000 && tokyo_osaka < 404000); // ~402 km great circle
    // ~0, not exactly 0: aarch64 contracts to FMA, leaving a sub-micron residue.
    CHECK(near(Parser::distanceBetween(35.681236, 139.767125, 35.681236, 139.767125), 0.0, 1e-3));
    std::printf("ok   distanceBetween: TinyGPS++ great circle (R=6371009)\n");
}

// --- layout -----------------------------------------------------------------------

void test_fit_lines() {
    const std::vector<ScreenLine> bruce = {
        {1, ""},
        {2, "File: 261122_152002_gps_tracker"},
        {2, "GPS Coordinates: 12"},
        {2, "Distance: 0.42km"},
        {1, ""},
        {1, "GPS location not updated"},
        {2, "Date: 2026-11-22"},
        {2, "Time: 15:20:02"},
        {2, "Sat:  7"},
        {2, "HDOP: 1.20"},
    };
    // 10 -> drop 2 blanks (8) -> merge Sat+HDOP (7) -> merge Date+Time (6): fits,
    // so "GPS Coordinates" / "Distance" stay on their own lines.
    auto fitted = fit_lines(bruce, 6);
    CHECK(fitted.size() == 6);
    CHECK(fitted[0].text == "File: 261122_152002_gps_tracker" && fitted[0].padx == 2);
    CHECK(fitted[1].text == "GPS Coordinates: 12");
    CHECK(fitted[2].text == "Distance: 0.42km");
    CHECK(fitted[3].text == "GPS location not updated" && fitted[3].padx == 1);
    CHECK(fitted[4].text == "Date: 2026-11-22   Time: 15:20:02");
    CHECK(fitted[5].text == "Sat:  7   HDOP: 1.20");
    // A tighter screen merges the banner pair too.
    fitted = fit_lines(bruce, 5);
    CHECK(fitted.size() == 5 && fitted[1].text == "GPS Coordinates: 12   Distance: 0.42km");

    // No banner yet: 7 lines -> blanks dropped, the Date block stays verbatim.
    const std::vector<ScreenLine> early = {
        {1, ""}, {1, ""}, {1, "GPS location not updated"}, {2, "Date: 2026-11-22"},
        {2, "Time: 15:20:02"}, {2, "Sat:  0"}, {2, "HDOP: 25.50"},
    };
    fitted = fit_lines(early, 6);
    CHECK(fitted.size() == 5 && fitted[4].text == "HDOP: 25.50");

    // Already fits: untouched (blank lines kept).
    fitted = fit_lines({{1, ""}, {1, "Initializing..."}}, 6);
    CHECK(fitted.size() == 2 && fitted[0].text.empty());
    std::printf("ok   fit_lines: Bruce text kept, pairs merged only when needed\n");
}

// --- tracker ----------------------------------------------------------------------

const char* const kGoldenTrack =
    "<?xml version=\"1.0\" encoding=\"ISO-8859-1\" standalone=\"yes\"?>\r\n"
    "<?xml-stylesheet type=\"text/xsl\" href=\"details.xsl\"?>\r\n"
    "<gpx\r\n"
    "  version=\"1.1\"\r\n"
    "  creator=\"Bruce Firmware\"\r\n"
    "  xmlns=\"http://www.topografix.com/GPX/1/1\"\r\n"
    "  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\r\n"
    "  xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd\"\r\n"
    ">\r\n"
    "  <metadata>\r\n"
    "    <name>Bruce GPS Tracker</name>\r\n"
    "    <desc>GPS Tracker using Bruce Firmware</desc>\r\n"
    "    <link href=\"https://bruce.computer\">\r\n"
    "      <text>Bruce Website</text>\r\n"
    "    </link>\r\n"
    "  </metadata>\r\n"
    "  <trk>\r\n"
    "    <name>Bruce Route</name>\r\n"
    "    <desc>GPS route captured by Bruce firmware</desc>\r\n"
    "    <trkseg>\r\n"
    "      <trkpt lat=\"35.681236\" lon=\"139.767125\">\n"
    "        <sym>Waypoint</sym>\r\n"
    "        <ele>41.200000</ele>\n"
    "        <hdop>1.200000</hdop>\n"
    "        <sat>7</sat>\n"
    "      </trkpt>\r\n"
    "      <trkpt lat=\"35.682236\" lon=\"139.767125\">\n"
    "        <sym>Waypoint</sym>\r\n"
    "        <ele>41.200000</ele>\n"
    "        <hdop>1.200000</hdop>\n"
    "        <sat>7</sat>\n"
    "      </trkpt>\r\n"
    "    </trkseg>\r\n"
    "  </trk>\r\n"
    "</gpx>\r\n";

// Wait (bounded) until the storage worker has applied every result.
void settle(Tracker& t, int timeout_ms = 3000) {
    for (int i = 0; i < timeout_ms / 5 && !t.io_idle(); ++i) {
        ::usleep(5000);
    }
    CHECK(t.io_idle());
}

// One screen tick: the locked phase, the unlocked hand-off to the storage
// worker, then (test only) wait for the worker so results are deterministic.
void step(Tracker& t, uint64_t now, size_t bytes, Parser& gps, bool disconnected = false) {
    t.tick(now, bytes, disconnected, gps);
    t.flush_io();
    settle(t);
}

Tracker::Options opts(const std::string& dir) {
    Tracker::Options o;
    o.dir = dir;
    o.current_year = 2026; // Bruce's CURRENT_YEAR, pinned for a stable test
    return o;
}

void test_tracker_waiting_and_passes() {
    const std::string home = temp_dir();
    const std::string dir = home + "/BruceGPS";
    Parser gps;
    Tracker t(opts(dir));
    uint64_t now = 1000;

    t.start(now);
    CHECK(t.state() == Tracker::State::Waiting);
    CHECK(t.frame().title == "GPS Tracker");
    CHECK((texts(t.frame()) == std::vector<std::string>{"", "", "Initializing...", "Waiting for GPS data"}));
    CHECK(!t.gps_connected());

    // begin_gps(): "Waiting GPS: Ns" once a second while nothing arrives.
    step(t, now, 0, gps);
    CHECK(t.frame().popup == PopupKind::Message && t.frame().popup_text == "Waiting GPS: 0s");
    step(t, now + 500, 0, gps);
    CHECK(t.frame().popup_text == "Waiting GPS: 0s");
    step(t, now + 1000, 0, gps);
    CHECK(t.frame().popup_text == "Waiting GPS: 1s");

    // First bytes: connected, first pass straight away (no fix yet).
    const std::string nofix = cap_no_fix();
    feed(gps, nofix);
    now += 2000;
    step(t, now, nofix.size(), gps);
    CHECK(t.state() == Tracker::State::Running && t.gps_connected());
    CHECK(t.frame().popup == PopupKind::None);
    CHECK((texts(t.frame()) == std::vector<std::string>{"", "", "GPS location not updated", "Date: 2026-11-22",
                                                        "Time: 15:20:02", "Sat:  0", "HDOP: 25.50"}));
    CHECK(t.frame().lines[3].padx == 2);
    CHECK(t.filename() == "261122_152002_gps_tracker.gpx"); // year 2026 in [2026, 2031)
    CHECK(::access(t.filepath().c_str(), F_OK) != 0);        // nothing written without a fix

    // MAX_WAIT: the next pass is 5 s later, never earlier.
    const std::string fix1 = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, fix1);
    step(t, now + 4999, fix1.size(), gps);
    CHECK(texts(t.frame())[2] == "GPS location not updated");
    now += 5000;
    step(t, now, 0, gps);
    CHECK((texts(t.frame()) == std::vector<std::string>{"", "", "GPS location updated", "Coord: 35.681236, 139.767125"}));
    CHECK(t.coord_count() == 1);

    // Next pass with a move ~111 m north: the banner shows File / count / Distance.
    const std::string fix2 = fix_burst("152015", "3540.93416", "13946.02750");
    feed(gps, fix2);
    now += 5000;
    step(t, now, fix2.size(), gps);
    const auto lines = texts(t.frame());
    CHECK(lines[1] == "File: 261122_152002_gps_tracker");
    CHECK(lines[2] == "GPS Coordinates: 1");
    CHECK(lines[3] == "Distance: 0.00km"); // the banner is drawn before set_position()
    CHECK(lines[5] == "GPS location updated");
    CHECK(t.coord_count() == 2);
    CHECK(t.distance_m() > 110 && t.distance_m() < 112);

    // A pass without new fix data: "not updated" + the Date block (date latched).
    const std::string nofix2 = cap_no_fix();
    feed(gps, nofix2);
    now += 5000;
    step(t, now, nofix2.size(), gps);
    CHECK(texts(t.frame())[3] == "Distance: 0.11km");
    CHECK(texts(t.frame())[5] == "GPS location not updated");

    // ESC: end() + the destructor's footer.
    t.stop();
    settle(t);
    CHECK(t.state() == Tracker::State::Ended && t.end_reason() == Tracker::EndReason::Stopped);
    CHECK(!t.gps_connected());
    t.stop(); // idempotent: no second footer

    // Independent golden file, written out by hand from Bruce firmware 1.17
    // gps_tracker.cpp (println -> CRLF, printf("...\n") -> LF), not produced
    // by the formatter under test.
    CHECK(read_file(dir + "/261122_152002_gps_tracker.gpx") == kGoldenTrack);
    std::printf("ok   tracker: waiting popup, passes, banner, distance, GPX file\n");
}

void test_tracker_not_found() {
    Parser gps;
    Tracker t(opts(temp_dir() + "/BruceGPS"));
    uint64_t now = 0;
    t.start(now);
    const std::string nofix = cap_no_fix();
    feed(gps, nofix);
    step(t, now, nofix.size(), gps); // connected + first pass
    CHECK(t.gps_connected());
    // Bruce: count > 5 -> six "No GPS data available" passes, then the error.
    for (int i = 0; i < 6; ++i) {
        now += 5000;
        step(t, now, 0, gps);
        CHECK(t.state() == Tracker::State::Running);
        CHECK(texts(t.frame()).back() == "No GPS data available");
    }
    now += 5000;
    step(t, now, 0, gps);
    CHECK(t.state() == Tracker::State::Ended && t.end_reason() == Tracker::EndReason::NotFound);
    CHECK(t.frame().popup == PopupKind::Error && t.frame().popup_text == "GPS not Found!");
    CHECK(!t.gps_connected());

    // Data before the 7th empty pass resets the count (count = 0).
    Tracker t2(opts(temp_dir() + "/BruceGPS"));
    t2.start(0);
    step(t2, 0, 10, gps);
    for (int i = 1; i <= 5; ++i) {
        step(t2, i * 5000, 0, gps);
    }
    step(t2, 6 * 5000, 10, gps);
    for (int i = 7; i <= 12; ++i) {
        step(t2, i * 5000, 0, gps);
    }
    CHECK(t2.state() == Tracker::State::Running);
    std::printf("ok   tracker: 6 empty passes then \"GPS not Found!\"\n");
}

void test_tracker_waiting_for_valid_data_and_errors() {
    // GGA only: time updates but no date -> "Waiting for valid GPS data".
    Parser gps;
    Tracker t(opts(temp_dir() + "/BruceGPS"));
    t.start(0);
    const std::string gga = nmea("GNGGA,152011.00,,,,,0,00,25.5,,,,,,");
    feed(gps, gga);
    step(t, 0, gga.size(), gps);
    CHECK(texts(t.frame()).back() == "Waiting for valid GPS data");
    CHECK(t.filename().empty());

    // Storage root missing -> "Storage setup error" (line + popup) and end.
    Parser fix;
    Tracker bad(opts("/nonexistent/gps/home/BruceGPS"));
    bad.start(0);
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(fix, f);
    step(bad, 0, f.size(), fix);
    CHECK(bad.state() == Tracker::State::Ended && bad.end_reason() == Tracker::EndReason::StorageError);
    CHECK(texts(bad.frame()).back() == "Storage setup error");
    CHECK(bad.frame().popup == PopupKind::Error && bad.frame().popup_text == "Storage setup error");

    // Folder not writable -> "Failed to open file for writing".
    if (::geteuid() != 0) {
        const std::string home = temp_dir();
        const std::string ro = home + "/BruceGPS";
        CHECK(::mkdir(ro.c_str(), 0555) == 0);
        Parser fix2;
        Tracker t3(opts(ro));
        t3.start(0);
        feed(fix2, f);
        step(t3, 0, f.size(), fix2);
        CHECK(t3.end_reason() == Tracker::EndReason::FileError);
        CHECK(t3.frame().popup_text == "Failed to open file for writing");
    }

    // Open failures (Linux only): banner + "Initializing..." + error popup.
    Tracker open_fail(opts(temp_dir()));
    open_fail.fail_open("GPS not Found!", true);
    CHECK((texts(open_fail.frame()) == std::vector<std::string>{"", "", "Initializing..."}));
    CHECK(open_fail.frame().popup_text == "GPS not Found!");
    CHECK(open_fail.end_reason() == Tracker::EndReason::NotFound);
    std::printf("ok   tracker: waiting for valid data, storage/file/open errors\n");
}

void test_tracker_filename() {
    CHECK(tracker_filename(2026, 1, 2, 3, 4, 5) == "260102_030405_gps_tracker.gpx");
    CHECK(build_year() >= 2026);
    std::printf("ok   tracker: file name pattern\n");
}

// --- session ------------------------------------------------------------------------

bool wait_for(Session& s, size_t& got, int ms) {
    for (int i = 0; i < ms / 10; ++i) {
        got += s.take_available();
        if (got > 0) {
            return true;
        }
        ::usleep(10000);
    }
    return false;
}

double cpu_seconds() {
    timespec ts{};
    ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void test_session_pty() {
    int master = -1, slave = -1;
    char name[128] = {};
    CHECK(::openpty(&master, &slave, name, nullptr, nullptr) == 0);
    // Keep the slave open in this process so closing the session doesn't hang
    // the pty up; raw mode so the line discipline doesn't echo or translate.
    termios tio{};
    ::tcgetattr(slave, &tio);
    ::cfmakeraw(&tio);
    ::tcsetattr(slave, TCSANOW, &tio);

    Session s;
    OpenResult res;
    CHECK(s.open(name, 115200, res));
    CHECK(res.error == OpenError::None && s.is_open());

    // Idle: the reader blocks in poll(), so ~no CPU is spent while silent.
    const double cpu0 = cpu_seconds();
    ::usleep(500000);
    const double idle_cpu = cpu_seconds() - cpu0;
    CHECK(idle_cpu < 0.02);

    const std::string data = cap_no_fix();
    CHECK(::write(master, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    size_t got = 0;
    CHECK(wait_for(s, got, 2000));
    ::usleep(200000);
    got += s.take_available();
    CHECK(got == data.size());
    s.with_gps([](Parser& p) {
        CHECK(p.date.value() == 221126 && p.antenna() == Antenna::Open);
        return 0;
    });
    CHECK(s.take_available() == 0);

    // close() returns promptly even though the reader is blocked in poll().
    s.close();
    CHECK(!s.is_open());

    // Reopen, then hang the pty up (USB unplug): the reader stops, no spin.
    CHECK(s.open(name, 9600, res));
    ::close(slave);
    ::close(master);
    for (int i = 0; i < 100 && !s.hung_up(); ++i) {
        ::usleep(10000);
    }
    CHECK(s.hung_up());
    const double cpu1 = cpu_seconds();
    ::usleep(200000);
    CHECK(cpu_seconds() - cpu1 < 0.02);
    s.close();
    std::printf("ok   session: pty read, idle CPU %.1f ms/0.5 s, close, hang-up\n", idle_cpu * 1000);
}

void test_session_errors() {
    Session s;
    OpenResult res;
    CHECK(!s.open("/nonexistent/gps0", 115200, res));
    CHECK(res.error == OpenError::NotFound && res.message == "GPS not Found!");

    if (::geteuid() != 0) {
        const std::string path = temp_dir() + "/ttyFAKE";
        const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0000);
        CHECK(fd >= 0);
        ::close(fd);
        CHECK(!s.open(path, 115200, res));
        CHECK(res.error == OpenError::Permission);
        CHECK(res.message.find("permission denied") != std::string::npos);
        CHECK(res.message.find("dialout") != std::string::npos);

        // gpsd-style exclusive hold (TIOCEXCL) -> EBUSY for everyone else.
        int master = -1, slave = -1;
        char name[128] = {};
        CHECK(::openpty(&master, &slave, name, nullptr, nullptr) == 0);
        CHECK(::ioctl(slave, TIOCEXCL) == 0);
        CHECK(!s.open(name, 115200, res));
        CHECK(res.error == OpenError::Busy);
        CHECK(res.message.find("busy") != std::string::npos);
        ::close(slave);
        ::close(master);
        std::printf("ok   session: not found / permission / busy (TIOCEXCL)\n");
    } else {
        std::printf("skip session: permission/busy checks need a non-root user\n");
    }
}

// Session + Tracker wired the way GpsAppScreen drives them.
void test_end_to_end() {
    int master = -1, slave = -1;
    char name[128] = {};
    CHECK(::openpty(&master, &slave, name, nullptr, nullptr) == 0);
    termios tio{};
    ::tcgetattr(slave, &tio);
    ::cfmakeraw(&tio);
    ::tcsetattr(slave, TCSANOW, &tio);

    const std::string dir = temp_dir() + "/BruceGPS";
    Session s;
    OpenResult res;
    CHECK(s.open(name, 115200, res));
    Tracker t(opts(dir));
    t.start(0);
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    CHECK(::write(master, f.data(), f.size()) == static_cast<ssize_t>(f.size()));
    size_t got = 0;
    CHECK(wait_for(s, got, 2000));
    ::usleep(100000);
    got += s.take_available();
    s.with_gps([&](Parser& p) {
        t.tick(0, got, s.hung_up(), p);
        return 0;
    });
    t.flush_io(); // file I/O outside the parser lock, as GpsAppScreen does
    settle(t);
    CHECK(texts(t.frame())[2] == "GPS location updated");
    s.close();
    t.stop();
    settle(t);
    CHECK(read_file(dir + "/261122_152010_gps_tracker.gpx").find("</gpx>") != std::string::npos);
    ::close(slave);
    ::close(master);
    std::printf("ok   end to end: pty -> Session -> Tracker -> GPX\n");
}


// === Regressions for the PR A review ===============================================

void open_raw_pty(int& master, int& slave, char* name) {
    CHECK(::openpty(&master, &slave, name, nullptr, nullptr) == 0);
    termios tio{};
    ::tcgetattr(slave, &tio);
    ::cfmakeraw(&tio);
    ::tcsetattr(slave, TCSANOW, &tio);
}

// HIGH 1: a burst that fills the read buffer exactly must not end the reader
// (a raw tty returns 0 from read() when its queue is empty - not a hang-up).
void test_review_burst_then_fix() {
    int master = -1, slave = -1;
    char name[128] = {};
    open_raw_pty(master, slave, name);
    Session s;
    OpenResult res;
    CHECK(s.open(name, 115200, res));

    for (size_t burst : {size_t{1024}, size_t{4096}, size_t{8192}}) {
        // Exactly `burst` bytes of valid NMEA filler.
        std::string data;
        const std::string gsv = nmea("GPGSV,1,1,00,1");
        while (data.size() + gsv.size() <= burst) {
            data += gsv;
        }
        data += std::string(burst - data.size(), '\n');
        CHECK(data.size() == burst);
        CHECK(::write(master, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
        size_t got = 0;
        for (int i = 0; i < 200 && got < burst; ++i) {
            got += s.take_available();
            ::usleep(10000);
        }
        CHECK(got == burst);
        ::usleep(150000); // the reader sees an empty queue now
        CHECK(!s.hung_up());
    }

    // ... and a later fix is still parsed.
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    CHECK(::write(master, f.data(), f.size()) == static_cast<ssize_t>(f.size()));
    size_t got = 0;
    CHECK(wait_for(s, got, 2000));
    ::usleep(150000);
    s.with_gps([](Parser& p) {
        CHECK(p.location.isUpdated());
        CHECK(near(p.location.lat(), 35.681236, 1e-6));
        return 0;
    });
    CHECK(!s.hung_up());
    s.close();
    ::close(slave);
    ::close(master);
    std::printf("ok   review H1: 1/4/8 KiB bursts then a fix on a live pty\n");
}

// --- exec'd helper processes ---------------------------------------------------------
//
// Cases that need a separate process (RLIMIT_FSIZE, exit(), signals) run in a
// freshly exec'd copy of this binary (`/proc/self/exe --child <case> ...`), not
// a bare fork: a forked child would inherit the parent's GPX writer registry
// while only the forking thread survives, which made such tests flaky.

struct ChildResult {
    int status = -1;
    std::string out, err;
};

ChildResult run_child(const std::vector<std::string>& args, rlim_t fsize_limit = RLIM_INFINITY) {
    // argv is built before fork(): the child only calls async-signal-safe
    // functions (dup2/close/signal/setrlimit/execv), never the allocator.
    std::vector<std::string> full = {"gps_reader_check", "--child"};
    full.insert(full.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : full) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    int outp[2], errp[2];
    CHECK(::pipe(outp) == 0 && ::pipe(errp) == 0);
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        ::dup2(outp[1], 1);
        ::dup2(errp[1], 2);
        ::close(outp[0]);
        ::close(errp[0]);
        ::close(outp[1]);
        ::close(errp[1]);
        ::signal(SIGXFSZ, SIG_DFL); // default disposition: the writer must cope
        if (fsize_limit != RLIM_INFINITY) {
            rlimit rl{fsize_limit, fsize_limit};
            ::setrlimit(RLIMIT_FSIZE, &rl); // survives exec
        }
        ::execv("/proc/self/exe", argv.data());
        ::_exit(127);
    }
    ::close(outp[1]);
    ::close(errp[1]);
    ChildResult r;
    pollfd fds[2] = {{outp[0], POLLIN, 0}, {errp[0], POLLIN, 0}};
    int open_fds = 2;
    char buf[512];
    while (open_fds > 0) {
        ::poll(fds, 2, -1);
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd >= 0 && fds[i].revents) {
                const ssize_t n = ::read(fds[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    (i == 0 ? r.out : r.err).append(buf, static_cast<size_t>(n));
                } else {
                    ::close(fds[i].fd);
                    fds[i].fd = -1;
                    --open_fds;
                }
            }
        }
    }
    ::waitpid(pid, &r.status, 0);
    return r;
}

// HIGH 2: GPX write failures are reported and never counted (RLIMIT_FSIZE in a
// helper process with the default SIGXFSZ disposition).
int run_with_fsize_limit(rlim_t limit, const std::string& dir, int& count, std::string& popup,
                         std::string& finalize, bool& file_exists) {
    const ChildResult r = run_child({"fsize", dir}, limit);
    // Never killed by SIGXFSZ: a clean exit even with the default disposition.
    CHECK(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0);
    std::istringstream in(r.out);
    std::string c, name;
    std::getline(in, c);
    std::getline(in, popup);
    std::getline(in, finalize);
    std::getline(in, name);
    count = std::atoi(c.c_str());
    file_exists = !name.empty() && ::access((dir + "/" + name).c_str(), F_OK) == 0;
    return r.status;
}

int child_fsize(const std::string& dir) {
    Parser gps;
    Tracker t(opts(dir));
    t.start(0);
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, f);
    step(t, 0, f.size(), gps);
    t.stop();
    settle(t);
    std::printf("%d\n%s\n%s\n%s\n", t.coord_count(), t.frame().popup_text.c_str(), t.finalize_error().c_str(),
                t.filename().c_str());
    std::fflush(stdout);
    return 0;
}

void test_review_write_failures() {
    const size_t header = std::strlen(kGoldenTrack) - std::strlen(std::strstr(kGoldenTrack, "      <trkpt"));
    const size_t point = std::strstr(kGoldenTrack, "      <trkpt lat=\"35.682236\"") -
                         std::strstr(kGoldenTrack, "      <trkpt");
    int count = -1;
    std::string popup, finalize;
    bool exists = false;

    // 64 bytes: the header cannot be written -> no point, no stub file.
    const std::string dir64 = temp_dir() + "/BruceGPS";
    run_with_fsize_limit(64, dir64, count, popup, finalize, exists);
    CHECK(count == 0 && popup == "Failed to write GPS file" && !exists);
    CHECK(::access((dir64 + "/261122_152010_gps_tracker.gpx").c_str(), F_OK) != 0); // no 64-byte stub

    // Header fits, the point does not -> not counted, error shown.
    run_with_fsize_limit(header + 20, temp_dir() + "/BruceGPS", count, popup, finalize, exists);
    CHECK(count == 0 && popup == "Failed to write GPS file");

    // Header + point fit, the footer does not -> finalization error surfaced.
    run_with_fsize_limit(header + point, temp_dir() + "/BruceGPS", count, popup, finalize, exists);
    CHECK(count == 1);
    CHECK(finalize == "Failed to finalize 261122_152010_gps_tracker.gpx");
    CHECK(popup == finalize);
    std::printf("ok   review H2: write failures reported, not counted, no SIGXFSZ death\n");
}

// HIGH 3: no root path, and the folder / file are never followed as symlinks.
void test_review_no_symlink_following() {
    const std::string home = temp_dir();
    const std::string target = temp_dir();
    CHECK(::symlink(target.c_str(), (home + "/BruceGPS").c_str()) == 0);
    Parser gps;
    Tracker t(opts(home + "/BruceGPS"));
    t.start(0);
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, f);
    step(t, 0, f.size(), gps);
    CHECK(t.end_reason() == Tracker::EndReason::StorageError);
    CHECK(t.coord_count() == 0);
    CHECK(::access((target + "/261122_152010_gps_tracker.gpx").c_str(), F_OK) != 0);

    // A planted symlink with the track's name is not opened (O_EXCL|O_NOFOLLOW):
    // the track gets a suffix and the link target stays untouched.
    const std::string home2 = temp_dir();
    const std::string dir2 = home2 + "/BruceGPS";
    CHECK(::mkdir(dir2.c_str(), 0755) == 0);
    const std::string victim = temp_dir() + "/victim";
    { std::ofstream(victim) << "keep"; }
    CHECK(::symlink(victim.c_str(), (dir2 + "/261122_152010_gps_tracker.gpx").c_str()) == 0);
    Parser gps2;
    Tracker t2(opts(dir2));
    t2.start(0);
    feed(gps2, f);
    step(t2, 0, f.size(), gps2);
    CHECK(t2.coord_count() == 1 && t2.filename() == "261122_152010_gps_tracker-1.gpx");
    t2.stop();
    settle(t2);
    CHECK(read_file(victim) == "keep");

    // Options come from $HOME only; SUDO_UID no longer redirects or chowns.
    ::setenv("SUDO_UID", "0", 1);
    ::setenv("HOME", home.c_str(), 1);
    CHECK(tracker_options_from_env().dir == home + "/BruceGPS");
    ::unsetenv("SUDO_UID");
    std::printf("ok   review H3: no symlink following, no sudo/chown path\n");
}

// MED 4: a port that hangs up is "GPS not Found!" once its bytes are used up.
void test_review_disconnect() {
    Parser gps;
    // Before the first byte: no endless "Waiting GPS".
    Tracker t(opts(temp_dir() + "/BruceGPS"));
    t.start(0);
    step(t, 0, 0, gps);
    CHECK(t.frame().popup_text == "Waiting GPS: 0s");
    step(t, 1000, 0, gps, /*disconnected=*/true);
    CHECK(t.end_reason() == Tracker::EndReason::NotFound && t.frame().popup_text == "GPS not Found!");

    // Buffered bytes are still used, then the disconnect ends it at once.
    Tracker t2(opts(temp_dir() + "/BruceGPS"));
    t2.start(0);
    const std::string nofix = cap_no_fix();
    feed(gps, nofix);
    step(t2, 0, nofix.size(), gps, /*disconnected=*/true);
    CHECK(t2.state() == Tracker::State::Running);
    CHECK(texts(t2.frame())[2] == "GPS location not updated");
    step(t2, 5000, 0, gps, true);
    CHECK(t2.end_reason() == Tracker::EndReason::NotFound); // no 6-pass countdown

    // Connected but silent keeps Bruce's countdown (covered in detail above).
    Tracker t3(opts(temp_dir() + "/BruceGPS"));
    t3.start(0);
    step(t3, 0, 10, gps);
    step(t3, 5000, 0, gps);
    CHECK(t3.state() == Tracker::State::Running && texts(t3.frame()).back() == "No GPS data available");

    // Session level: a pty hung up before any data is reported.
    int master = -1, slave = -1;
    char name[128] = {};
    open_raw_pty(master, slave, name);
    Session s;
    OpenResult res;
    CHECK(s.open(name, 115200, res));
    ::close(slave);
    ::close(master);
    for (int i = 0; i < 100 && !s.hung_up(); ++i) {
        ::usleep(10000);
    }
    CHECK(s.hung_up() && s.take_available() == 0);
    s.close();
    std::printf("ok   review M4: unplug before/after data -> \"GPS not Found!\"\n");
}

// MED 5: tick() (run under the parser lock) does no file I/O; flush_io() does.
void test_review_io_outside_lock() {
    const std::string dir = temp_dir() + "/BruceGPS";
    Parser gps;
    Tracker t(opts(dir));
    t.start(0);
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, f);
    t.tick(0, f.size(), false, gps);
    CHECK(texts(t.frame())[2] == "GPS location updated");
    CHECK(t.coord_count() == 0);
    CHECK(::access(dir.c_str(), F_OK) != 0); // not even the folder yet
    t.flush_io();
    settle(t);
    CHECK(t.coord_count() == 1);
    CHECK(texts(t.frame()).back() == "Coord: 35.681236, 139.767125");
    CHECK(::access((dir + "/261122_152010_gps_tracker.gpx").c_str(), F_OK) == 0);
    t.flush_io(); // nothing pending: no second point
    CHECK(t.coord_count() == 1);
    std::printf("ok   review M5: file I/O only in flush_io(), outside the lock\n");
}

// MED 6: a GPS-time name that already exists is never appended to.
void test_review_existing_track() {
    const std::string dir = temp_dir() + "/BruceGPS";
    CHECK(::mkdir(dir.c_str(), 0755) == 0);
    const std::string old_track = dir + "/261122_152010_gps_tracker.gpx";
    const std::string old_content = std::string(kGoldenTrack);
    { std::ofstream(old_track, std::ios::binary) << old_content; }

    Parser gps;
    Tracker t(opts(dir));
    t.start(0);
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, f);
    step(t, 0, f.size(), gps);
    CHECK(t.filename() == "261122_152010_gps_tracker-1.gpx");
    // The banner shows the actual name (minus .gpx) on the next pass.
    feed(gps, f);
    step(t, 5000, f.size(), gps);
    CHECK(texts(t.frame())[1] == "File: 261122_152010_gps_tracker-1");
    t.stop();
    t.stop();
    settle(t);
    CHECK(read_file(old_track) == old_content); // untouched, no 2nd footer
    const std::string mine = read_file(dir + "/261122_152010_gps_tracker-1.gpx");
    size_t footers = 0;
    for (size_t pos = 0; (pos = mine.find("</gpx>", pos)) != std::string::npos; ++pos) {
        ++footers;
    }
    CHECK(footers == 1);

    // A session that never wrote a point finalizes nothing.
    Tracker idle(opts(dir));
    idle.start(0);
    const std::string nofix = cap_no_fix();
    Parser gps2;
    feed(gps2, nofix);
    step(idle, 0, nofix.size(), gps2);
    CHECK(!idle.filename().empty());
    idle.stop();
    CHECK(::access((dir + "/" + idle.filename()).c_str(), F_OK) != 0);
    std::printf("ok   review M6: exclusive create + suffix, single footer\n");
}

// MED 7: TinyGPS++ number handling.
void test_review_tinygps_fidelity() {
    CHECK(Parser::parseDecimal("1.239") == 123);   // truncated, not rounded
    CHECK(Parser::parseDecimal("-12.345") == -1234);
    CHECK(Parser::parseDecimal("7") == 700);
    CHECK(Parser::parseDecimal("152002.00") == 15200200);

    Parser p;
    feed(p, nmea("GNGGA,152010.00,3540.87416,N,13946.02750,E,1,08,1.239,41.259,M,39.0,M,,"));
    CHECK(near(p.hdop.hdop(), 1.23, 1e-12));
    CHECK(near(p.altitude.meters(), 41.25, 1e-12));
    CHECK(p.satellites.value() == 8);
    char line[32];
    std::snprintf(line, sizeof(line), "HDOP: %.2f", p.hdop.hdop());
    CHECK(std::string(line) == "HDOP: 1.23");

    // Empty fields keep the previous value (TinyGPS++ skips empty terms).
    feed(p, nmea("GNGGA,152011.00,3540.87416,N,13946.02750,E,1,,,,M,39.0,M,,"));
    CHECK(p.satellites.value() == 8 && near(p.hdop.hdop(), 1.23, 1e-12));

    // Term-based like TinyGPS++: a short GGA still commits what it carries.
    Parser shortp;
    feed(shortp, nmea("GPGGA,152012.00,3540.87416,N,13946.02750,E,1,05,1.5"));
    CHECK(shortp.location.isUpdated() && shortp.satellites.value() == 5);

    // RMC speed (knots) and course, committed with a fix.
    Parser rmc;
    feed(rmc, nmea("GNRMC,152013.00,A,3540.87416,N,13946.02750,E,5.678,123.456,221126,,,A,V"));
    CHECK(rmc.speed.value() == 567 && rmc.course.value() == 12345);

    // Missing checksum: ignored, not counted as failed; bad one is counted.
    Parser c;
    feed(c, "$GNRMC,152002.00,V,,,,,,,221126,,,N,V\r\n");
    CHECK(c.failedChecksum() == 0 && c.passedChecksum() == 0 && !c.date.isValid());
    feed(c, "$GNRMC,152002.00,V,,,,,,,221126,,,N,V*00\r\n");
    CHECK(c.failedChecksum() == 1);

    // TinyGPS++ parseDegrees precision (billionths).
    Parser d;
    feed(d, nmea("GNRMC,152013.00,A,3540.874167,N,13946.027501,E,0,0,221126,,,A,V"));
    CHECK(near(d.location.lat(), 35.681236116, 1e-9));
    std::printf("ok   review M7: 2-decimal truncation, empty fields, short sentences, speed\n");
}

// Exclusive access: a second session (second tracker) cannot share the tty.
void test_review_exclusive() {
    int master = -1, slave = -1;
    char name[128] = {};
    open_raw_pty(master, slave, name);
    Session a, b;
    OpenResult res;
    CHECK(a.open(name, 115200, res));
    CHECK(!b.open(name, 115200, res));
    CHECK(res.error == OpenError::Busy);
    a.close();
    CHECK(b.open(name, 115200, res)); // released on close
    b.close();
    ::close(slave);
    ::close(master);
    std::printf("ok   review: exclusive port (TIOCEXCL + flock)\n");
}

// === Regressions for the PR A re-review ============================================

uint64_t mono_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

// RR-H1: slow storage never blocks the caller (the LVGL thread): the header
// write, every point and the footer run on the storage worker; Back releases
// the UART at once and the late result still comes back.
void test_rereview_slow_storage() {
    int master = -1, slave = -1;
    char name[128] = {};
    open_raw_pty(master, slave, name);
    Session s;
    Tracker::Options o = opts(temp_dir() + "/BruceGPS");
    o.io_delay_ms = 1500; // every file operation takes 1.5 s
    Tracker t(o);
    CHECK(begin_tracker(s, t, name, 115200, 0));
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    Parser gps;
    feed(gps, f);

    uint64_t t0 = mono_ms();
    t.tick(0, f.size(), false, gps); // first pass queues the first point
    t.flush_io();                    // ... hands it to the worker
    CHECK(mono_ms() - t0 < 100);
    CHECK(t.coord_count() == 0 && !t.io_idle());

    // Back: release the UART, stop - all immediate although storage is busy.
    t0 = mono_ms();
    s.close();
    t.stop();
    CHECK(mono_ms() - t0 < 100);
    CHECK(!s.is_open());
    Session other; // the port is really free for someone else now
    OpenResult res;
    CHECK(other.open(name, 115200, res));
    other.close();
    CHECK(!t.io_idle());

    // The worker finishes header + point + footer and reports back.
    settle(t, 8000);
    CHECK(t.coord_count() == 1 && t.finalize_error().empty());
    CHECK(read_file(o.dir + "/261122_152010_gps_tracker.gpx").find("</gpx>") != std::string::npos);

    // Destroying a tracker while the worker is busy does not block either.
    {
        Tracker::Options o2 = opts(temp_dir() + "/BruceGPS");
        o2.io_delay_ms = 1500;
        auto t2 = std::make_unique<Tracker>(o2);
        t2->start(0);
        Parser g2;
        feed(g2, f);
        t2->tick(0, f.size(), false, g2);
        t2->flush_io();
        t0 = mono_ms();
        t2.reset();
        CHECK(mono_ms() - t0 < 100);
        // ... and the detached worker still completes and closes the track.
        const std::string path = o2.dir + "/261122_152010_gps_tracker.gpx";
        for (int i = 0; i < 160 && read_file(path).find("</gpx>") == std::string::npos; ++i) {
            ::usleep(50000);
        }
        CHECK(read_file(path).find("</gpx>") != std::string::npos);
    }
    ::close(slave);
    ::close(master);
    std::printf("ok   re-review H1: slow storage off the UI thread, Back releases UART at once\n");
}

// RR-M3: one shared entry refuses root before the UART is touched (checked in a
// user namespace where euid == 0 without real privileges).
void test_rereview_root_refusal() {
    int master = -1, slave = -1;
    char name[128] = {};
    open_raw_pty(master, slave, name);
    int pipefd[2];
    CHECK(::pipe(pipefd) == 0);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        ::close(pipefd[0]);
        const uid_t uid = ::getuid();
        const gid_t gid = ::getgid();
        std::string out = "skip\n";
        if (::unshare(CLONE_NEWUSER) == 0) {
            std::ofstream("/proc/self/setgroups") << "deny";
            std::ofstream("/proc/self/uid_map") << "0 " << uid << " 1";
            std::ofstream("/proc/self/gid_map") << "0 " << gid << " 1";
            if (::geteuid() == 0) {
                Session sess;
                Tracker tr(opts(temp_dir() + "/BruceGPS"));
                const bool started = begin_tracker(sess, tr, name, 115200, 0);
                out = std::string(started ? "started" : "refused") + "\n" + (sess.is_open() ? "open" : "closed") +
                      "\n" + tr.frame().popup_text + "\n";
            }
        }
        (void)!::write(pipefd[1], out.data(), out.size());
        ::_exit(0);
    }
    ::close(pipefd[1]);
    std::string out;
    char buf[256];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(pipefd[0]);
    ::waitpid(pid, nullptr, 0);
    if (out == "skip\n") {
        std::printf("skip re-review M3: user namespaces unavailable here\n");
    } else {
        CHECK(out == std::string("refused\nclosed\n") + tracker_root_refusal() + "\n");
        std::printf("ok   re-review M3: begin_tracker() refuses root before opening the UART\n");
    }
    // Deterministic: effective uid 0 is refused before the port is opened.
    {
        Session sr;
        Tracker tr(opts(temp_dir() + "/BruceGPS"));
        CHECK(!begin_tracker(sr, tr, name, 115200, 0, /*effective_uid=*/0));
        CHECK(!sr.is_open());
        CHECK(tr.frame().popup_text == tracker_root_refusal());
        CHECK(tr.end_reason() == Tracker::EndReason::OpenFailed);
        // Nothing grabbed the tty: a normal session can still take it.
        CHECK(sr.open(name, 115200, *std::make_unique<OpenResult>()));
        sr.close();
        std::printf("ok   re-review M3: effective uid 0 refused before the UART is opened\n");
    }
    // As a normal user the same entry opens and starts.
    Session s;
    Tracker t(opts(temp_dir() + "/BruceGPS"));
    CHECK(begin_tracker(s, t, name, 115200, 0) && s.is_open() && t.state() == Tracker::State::Waiting);
    s.close();
    ::close(slave);
    ::close(master);
}

// RR-M4: bytes, hang-up and parser come from one snapshot, so a last fix that
// arrives right before EOF is never dropped. A FIFO is used as the "port": its
// EOF is delivered strictly after the data (closing a pty master instead makes
// the kernel discard input that has not reached the slave yet).
void test_rereview_snapshot_ordering() {
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    const std::string dir = temp_dir();
    for (int trial = 0; trial < 40; ++trial) {
        const std::string fifo = dir + "/gps" + std::to_string(trial);
        CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
        Session s;
        OpenResult res;
        CHECK(s.open(fifo, 115200, res));
        const int w = ::open(fifo.c_str(), O_WRONLY);
        CHECK(w >= 0);
        CHECK(::write(w, f.data(), f.size()) == static_cast<ssize_t>(f.size()));
        if (trial % 4) {
            ::usleep(static_cast<useconds_t>(trial * 500)); // vary the race window
        }
        ::close(w); // EOF right after the last fix

        size_t total = 0;
        bool saw_hangup = false;
        bool fix_with_hangup = false;
        for (int i = 0; i < 500 && !saw_hangup; ++i) {
            s.snapshot([&](size_t bytes, bool hung_up, Parser& p) {
                total += bytes;
                if (hung_up) {
                    saw_hangup = true;
                    // Invariant: the hang-up is only visible once every byte
                    // before it was counted and parsed.
                    fix_with_hangup = total == f.size() && p.location.isValid();
                }
            });
            ::usleep(1000);
        }
        CHECK(saw_hangup && fix_with_hangup);
        s.close();
    }

    // Same ordering through the tracker: the fix is logged, then "GPS not Found!".
    const std::string fifo = dir + "/gps-tracker";
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    Session s;
    Tracker t(opts(temp_dir() + "/BruceGPS"));
    CHECK(begin_tracker(s, t, fifo, 115200, 0));
    const int w = ::open(fifo.c_str(), O_WRONLY);
    CHECK(w >= 0);
    CHECK(::write(w, f.data(), f.size()) == static_cast<ssize_t>(f.size()));
    ::close(w);
    uint64_t now = 0;
    for (int i = 0; i < 400 && t.state() != Tracker::State::Ended; ++i) {
        s.snapshot([&](size_t bytes, bool hung_up, Parser& p) { t.tick(now, bytes, hung_up, p); });
        t.flush_io();
        now += 250;
        ::usleep(2000);
    }
    settle(t);
    CHECK(t.end_reason() == Tracker::EndReason::NotFound);
    CHECK(t.coord_count() == 1);
    s.close();
    std::printf("ok   re-review M4: snapshot keeps the last fix before EOF (40 trials)\n");
}

// RR-5: $..TXT antenna status is committed only with a valid checksum.
void test_rereview_txt_checksums() {
    Parser p;
    feed(p, nmea("GPTXT,01,01,01,ANTENNA OPEN"));
    CHECK(p.antenna() == Antenna::Open);
    feed(p, "$GPTXT,01,01,01,ANTENNA OK\r\n"); // missing checksum: ignored
    CHECK(p.antenna() == Antenna::Open && p.failedChecksum() == 0);
    feed(p, "$GPTXT,01,01,01,ANTENNA OK*00\r\n"); // incorrect checksum
    CHECK(p.antenna() == Antenna::Open && p.failedChecksum() == 1);
    feed(p, "$GPTXT,01,01,01,ANTENNA OK*ZZ\r\n"); // malformed checksum
    CHECK(p.antenna() == Antenna::Open && p.failedChecksum() == 2);
    feed(p, "$GPTXT,01,01,01,ANTENNA OK"); // truncated, no terminator yet
    CHECK(p.antenna() == Antenna::Open);
    feed(p, nmea("GNTXT,01,01,01,ANTENNA OK"));
    CHECK(p.antenna() == Antenna::Ok);
    feed(p, nmea("GPTXT,01,01,02,ANTSTATUS=SHORT")); // u-blox wording, >14 chars
    CHECK(p.antenna() == Antenna::Short);
    feed(p, nmea("GPTXT,01,01,02,SW=URANUS5,V5.3.0.0")); // other TXT: unchanged
    CHECK(p.antenna() == Antenna::Short);
    feed(p, nmea("GPTXT,01,01")); // too few terms
    CHECK(p.antenna() == Antenna::Short);
    std::printf("ok   re-review 5: TXT antenna status needs a valid checksum\n");
}

// === Regressions for review round 3 =================================================

// R3-HIGH: a writer whose owner is gone (Back, Back) is still owned at process
// exit: exit() drains it, so the slow header/point/footer all land.
int child_exit_drain(const std::string& dir) {
    Tracker::Options o = opts(dir);
    o.io_delay_ms = 1000; // slow storage: each file operation takes 1 s
    auto t = std::make_unique<Tracker>(o);
    t->start(0);
    Parser gps;
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, f);
    t->tick(0, f.size(), false, gps);
    t->flush_io(); // header + point queued on the worker
    t->stop();     // Back: footer queued
    t.reset();     // second Back: the screen (and tracker) are gone
    std::exit(0);  // e.g. LVGL's SDL window close -> exit(0)
}

void test_round3_exit_drains_writers() {
    const std::string dir = temp_dir() + "/BruceGPS";
    const ChildResult r = run_child({"exit-drain", dir});
    CHECK(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0);
    const std::string gpx = read_file(dir + "/261122_152010_gps_tracker.gpx");
    CHECK(gpx.find("<trkpt") != std::string::npos);
    CHECK(gpx.size() > 9 && gpx.compare(gpx.size() - 8, 8, "</gpx>\r\n") == 0);

    // drain_writers() also closes a track whose tracker is still alive (exit
    // while the GPS Tracker screen is open) and joins every worker.
    Tracker::Options o = opts(temp_dir() + "/BruceGPS");
    o.io_delay_ms = 300;
    Tracker live(o);
    live.start(0);
    Parser gps;
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, f);
    live.tick(0, f.size(), false, gps);
    live.flush_io();
    {
        Tracker::Options o2 = opts(temp_dir() + "/BruceGPS");
        o2.io_delay_ms = 300;
        Tracker orphan(o2);
        orphan.start(0);
        Parser g2;
        feed(g2, f);
        orphan.tick(0, f.size(), false, g2);
        orphan.flush_io();
    } // owner gone while the worker is busy
    CHECK(live_writers() >= 2);
    reset_shutdown_for_tests();
    set_shutdown_timeout_ms(10000);
    CHECK(drain_writers() == 0);
    CHECK(live_writers() == 0);
    reset_shutdown_for_tests();
    const std::string live_gpx = read_file(o.dir + "/261122_152010_gps_tracker.gpx");
    CHECK(live_gpx.find("<trkpt") != std::string::npos && live_gpx.find("</gpx>") != std::string::npos);
    std::printf("ok   round 3 HIGH: exit()/shutdown drains and joins pending GPX writers\n");
}

// R3-MED: unplug + failed footer -> both messages, so the user knows the GPX
// is incomplete (the screen then waits for OK).
size_t golden_header_size() {
    return std::strlen(kGoldenTrack) - std::strlen(std::strstr(kGoldenTrack, "      <trkpt"));
}

size_t golden_point_size() {
    return static_cast<size_t>(std::strstr(kGoldenTrack, "      <trkpt lat=\"35.682236\"") -
                               std::strstr(kGoldenTrack, "      <trkpt"));
}

int child_finalize_priority(const std::string& dir) {
    Parser gps;
    Tracker t(opts(dir));
    t.start(0);
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, f);
    step(t, 0, f.size(), gps);
    step(t, 5000, 0, gps, /*disconnected=*/true); // unplugged
    settle(t);
    std::printf("%s|%s|%d", t.frame().popup_text.c_str(), t.finalize_error().c_str(),
                static_cast<int>(t.end_reason()));
    std::fflush(stdout);
    return 0;
}

void test_round3_finalize_error_priority() {
    // The footer will not fit: header + one point is the file-size limit.
    const ChildResult r = run_child({"finalize-priority", temp_dir() + "/BruceGPS"},
                                    golden_header_size() + golden_point_size());
    CHECK(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0);
    const std::string& out = r.out;
    const std::string fin = "Failed to finalize 261122_152010_gps_tracker.gpx";
    CHECK(out == "GPS not Found!\n" + fin + "|" + fin + "|" +
                     std::to_string(static_cast<int>(Tracker::EndReason::NotFound)));
    std::printf("ok   round 3 MED: unplug + failed footer shows both messages\n");
}

// R3-LOW: TXT checksum must be exactly two hex digits, then the line end.
void test_round3_strict_txt_checksum() {
    const char* const bad[] = {
        "$GPTXT,01,01,01,ANTENNA OPEN*25extra\r\n",
        "$GPTXT,01,01,01,ANTENNA OPEN*25 \r\n",
        "$GPTXT,01,01,01,ANTENNA OPEN*25*\r\n",
        "$GPTXT,01,01,01,ANTENNA OPEN*25,\r\n",
        "$GPTXT,01,01,01,ANTENNA OPEN*2\r\n",
        "$GPTXT,01,01,01,ANTENNA OPEN*250\r\n",
        "$GPTXT,01,01,01,ANTENNA OPEN*\r\n",
    };
    for (const char* line : bad) {
        Parser p;
        feed(p, line);
        CHECK(p.antenna() == Antenna::Unknown);
        CHECK(p.failedChecksum() >= 1); // "*25*" fails on both checksum terms
    }
    // Malformed first checksum, then a valid-looking one: still rejected, and
    // the rest of the sentence is discarded (counted once).
    Parser twice;
    feed(twice, "$GPTXT,01,01,01,ANTENNA OPEN*00*25\r\n");
    CHECK(twice.antenna() == Antenna::Unknown && twice.failedChecksum() == 1);
    feed(twice, "$GPTXT,01,01,01,ANTENNA OPEN*25\r\n"); // next sentence is fine
    CHECK(twice.antenna() == Antenna::Open);
    Parser ok;
    feed(ok, "$GPTXT,01,01,01,ANTENNA OPEN*25\r\n"); // the Cap's real sentence
    CHECK(ok.antenna() == Antenna::Open && ok.failedChecksum() == 0);
    Parser lf;
    feed(lf, "$GPTXT,01,01,01,ANTENNA OPEN*25\n"); // LF-only line end
    CHECK(lf.antenna() == Antenna::Open);
    std::printf("ok   round 3 LOW: TXT checksum = two hex digits + line end\n");
}

// === Regressions for review round 4 =================================================

// R4-MED: ONE process-wide shutdown deadline. With storage that never answers,
// the first drain waits until the deadline, reports the file once, and every
// later drain (the atexit hook) returns at once; exit() then fails (status 1).
int child_deadline(const std::string& dir) {
    reset_shutdown_for_tests();
    set_shutdown_timeout_ms(500);
    Tracker::Options o = opts(dir);
    o.io_delay_ms = 600000; // storage that does not respond
    auto t = std::make_unique<Tracker>(o);
    t->start(0);
    Parser gps;
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, f);
    t->tick(0, f.size(), false, gps);
    t->flush_io();
    t->stop();
    t.reset();
    const uint64_t a = mono_ms();
    const size_t stuck1 = drain_writers();
    const uint64_t b = mono_ms();
    const size_t stuck2 = drain_writers(); // e.g. atexit after main's drain
    const uint64_t c = mono_ms();
    std::printf("%zu %zu %llu %llu\n", stuck1, stuck2, static_cast<unsigned long long>(b - a),
                static_cast<unsigned long long>(c - b));
    std::fflush(stdout);
    std::exit(0); // atexit drain: same (expired) deadline -> status 1
}

void test_round4_single_deadline() {
    const uint64_t t0 = mono_ms();
    const ChildResult r = run_child({"deadline", temp_dir() + "/BruceGPS"});
    const int status = r.status;
    const std::string& out = r.out;
    const std::string& err = r.err;
    unsigned long long first_ms = 0, second_ms = 0;
    size_t stuck1 = 0, stuck2 = 0;
    CHECK(std::sscanf(out.c_str(), "%zu %zu %llu %llu", &stuck1, &stuck2, &first_ms, &second_ms) == 4);
    CHECK(stuck1 == 1 && stuck2 == 1);
    CHECK(first_ms >= 400 && first_ms < 1500); // waited until the 500 ms deadline
    CHECK(second_ms < 100);                    // ... and never again
    CHECK(mono_ms() - t0 < 3000); // no second 3 s (or 30 s) wait at exit
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    size_t reports = 0;
    for (size_t pos = 0; (pos = err.find("GPX write unfinished", pos)) != std::string::npos; ++pos) {
        ++reports;
    }
    CHECK(reports == 1); // logged once, by the first drain
    std::printf("ok   round 4 MED: one shutdown deadline, unfinished write logged once, exit 1\n");
}

// R4-LOW: the TXT checksum length is the received byte count.
void test_round4_txt_nul() {
    const std::string line("$GPTXT,01,01,01,ANTENNA OPEN*25\0extra\r\n", 40);
    CHECK(line.size() == 40 && line[31] == '\0');
    Parser p;
    p.encode(line.data(), line.size());
    CHECK(p.antenna() == Antenna::Unknown);
    CHECK(p.failedChecksum() >= 1);
    const std::string nul2("$GPTXT,01,01,01,ANTENNA OPEN*2\0" "5\r\n", 34);
    Parser q;
    q.encode(nul2.data(), nul2.size());
    CHECK(q.antenna() == Antenna::Unknown);
    std::printf("ok   round 4 LOW: TXT checksum counts received bytes (NUL-safe)\n");
}

// === Regressions for review round 5 =================================================

// R5-MED1: a footer write error nobody collected (screen gone, e.g. SIGTERM)
// is not lost: shutdown logs the file once and fails.
int child_orphan_footer(const std::string& dir) {
    Parser gps;
    auto t = std::make_unique<Tracker>(opts(dir));
    t->start(0);
    const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
    feed(gps, f);
    step(*t, 0, f.size(), gps); // header + point written (file-size limit allows it)
    t->stop();                  // footer queued; it will fail (limit reached)
    t.reset();                  // the screen is gone before the result arrives
    const size_t problems = drain_writers(); // main()'s orderly-shutdown drain
    std::printf("%zu\n", problems);
    std::fflush(stdout);
    return problems > 0 ? 1 : 0; // main(): non-zero; atexit agrees (status 1)
}

void test_round5_orphan_footer_error() {
    const std::string dir = temp_dir() + "/BruceGPS";
    const ChildResult r = run_child({"orphan-footer", dir}, golden_header_size() + golden_point_size());
    CHECK(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 1);
    CHECK(std::atoi(r.out.c_str()) >= 1);
    const std::string want = "gps: GPX write failed (Failed to finalize 261122_152010_gps_tracker.gpx): " + dir +
                             "/261122_152010_gps_tracker.gpx - the track is incomplete\n";
    CHECK(r.err == want); // logged exactly once, with the affected file
    const std::string gpx = read_file(dir + "/261122_152010_gps_tracker.gpx");
    CHECK(gpx.find("</trkpt>") != std::string::npos && gpx.find("</gpx>") == std::string::npos);

    // Same when a new tracker reaps the finished orphan before shutdown.
    const ChildResult r2 = run_child({"orphan-reap", temp_dir() + "/BruceGPS"},
                                     golden_header_size() + golden_point_size());
    CHECK(WIFEXITED(r2.status) && WEXITSTATUS(r2.status) == 1);
    CHECK(r2.err.find("Failed to finalize") != std::string::npos);
    std::printf("ok   round 5 MED1: orphaned footer error logged once, shutdown fails\n");
}

int child_orphan_reap(const std::string& dir) {
    {
        Parser gps;
        auto t = std::make_unique<Tracker>(opts(dir));
        t->start(0);
        const std::string f = fix_burst("152010", "3540.87416", "13946.02750");
        feed(gps, f);
        step(*t, 0, f.size(), gps);
        t->stop();
        t.reset();
    }
    ::usleep(300000); // the orphan's footer fails and its worker finishes
    // A new track: creating its writer reaps the finished orphan.
    Tracker again(opts(dir + "2"));
    again.start(0);
    Parser g2;
    const std::string nofix = cap_no_fix();
    feed(g2, nofix);
    step(again, 0, nofix.size(), g2);
    // No fix -> no writer was created by `again`; force one via a fix.
    const std::string f = fix_burst("152011", "3540.87416", "13946.02750");
    feed(g2, f);
    step(again, 5000, f.size(), g2);
    again.stop();
    settle(again);
    return drain_writers() > 0 ? 1 : 0;
}

int child_main(int argc, char** argv) {
    const std::string mode = argc > 2 ? argv[2] : "";
    const std::string dir = argc > 3 ? argv[3] : "";
    if (mode == "fsize") return child_fsize(dir);
    if (mode == "exit-drain") return child_exit_drain(dir);
    if (mode == "finalize-priority") return child_finalize_priority(dir);
    if (mode == "deadline") return child_deadline(dir);
    if (mode == "orphan-footer") return child_orphan_footer(dir);
    if (mode == "orphan-reap") return child_orphan_reap(dir);
    std::fprintf(stderr, "unknown child mode %s\n", mode.c_str());
    return 2;
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--child") {
        return child_main(argc, argv);
    }
    test_parser_cap_no_fix();
    test_parser_checksums();
    test_parser_fix();
    test_distance();
    test_fit_lines();
    test_tracker_filename();
    test_tracker_waiting_and_passes();
    test_tracker_not_found();
    test_tracker_waiting_for_valid_data_and_errors();
    test_session_pty();
    test_session_errors();
    test_end_to_end();
    test_review_burst_then_fix();
    test_review_write_failures();
    test_review_no_symlink_following();
    test_review_disconnect();
    test_review_io_outside_lock();
    test_review_existing_track();
    test_review_tinygps_fidelity();
    test_review_exclusive();
    test_rereview_slow_storage();
    test_rereview_root_refusal();
    test_rereview_snapshot_ordering();
    test_rereview_txt_checksums();
    test_round3_exit_drains_writers();
    test_round3_finalize_error_priority();
    test_round3_strict_txt_checksum();
    test_round4_single_deadline();
    test_round4_txt_nul();
    test_round5_orphan_footer_error();
    std::printf("gps_reader_selfcheck: all %d checks passed\n", g_checks);
    return 0;
}
