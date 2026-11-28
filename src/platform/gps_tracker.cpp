/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Line-by-line port of Bruce firmware 1.17 src/modules/gps/gps_tracker.cpp.
 * Firmware references are given as gps_tracker.cpp:<line>.
 */

#include "gps_tracker.h"

#include "gps_port.h"
#include "gps_session.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace platform::gps {
namespace {

constexpr uint64_t kMaxWaitMs = 5000;  // gps_tracker.cpp:15 MAX_WAIT
constexpr uint64_t kWaitTickMs = 1000; // begin_gps(): vTaskDelay(1000)
constexpr int kMaxEmptyPasses = 5;     // loop(): if (count > 5) "GPS not Found!"

std::string format(const char* fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

// Strip Bruce's trailing "\n" from a padprintf format result.
std::string chomp(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

#if defined(__linux__)
volatile sig_atomic_t g_stop = 0;
// Cancels a running Auto port probe (lock-free, so safe in the handler).
std::atomic<bool> g_cancel_probe{false};
// First SIGINT/SIGTERM: orderly stop (release the UART, close the track within
// the shutdown deadline). A second one forces the exit at once.
void on_signal(int sig) {
    if (g_stop) {
        _exit(128 + sig);
    }
    g_stop = 1;
    g_cancel_probe.store(true);
}

uint64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}
#endif

} // namespace

// --- layout adaptation ------------------------------------------------------------

std::vector<ScreenLine> fit_lines(const std::vector<ScreenLine>& lines, size_t max_lines) {
    std::vector<ScreenLine> out = lines;
    if (out.size() <= max_lines) {
        return out;
    }
    // 1. Bruce's blank spacer lines go first.
    std::vector<ScreenLine> compact;
    for (const auto& l : out) {
        if (!l.text.empty()) {
            compact.push_back(l);
        }
    }
    out.swap(compact);

    // 2. Wardriving: once "Scan done. ..." is printed, the "Start Scanning..."
    //    above it is history (the firmware shows both).
    for (size_t i = 0; out.size() > max_lines && i + 1 < out.size(); ++i) {
        if (out[i].text != "Start Scanning...") {
            continue;
        }
        bool done = false;
        for (size_t j = i + 1; j < out.size(); ++j) {
            done = done || starts_with(out[j].text, "Scan done.");
        }
        if (done) {
            out.erase(out.begin() + static_cast<long>(i));
        }
        break;
    }

    // 3. Merge Bruce's paired detail lines, bottom-most pair first.
    static const char* const kPairs[][2] = {
        {"Sat:", "HDOP:"},
        {"Date:", "Time:"},
        {"GPS Coordinates:", "Distance:"},
    };
    for (const auto& pair : kPairs) {
        if (out.size() <= max_lines) {
            break;
        }
        for (size_t i = 0; i + 1 < out.size(); ++i) {
            if (starts_with(out[i].text, pair[0]) && starts_with(out[i + 1].text, pair[1])) {
                out[i].text += "   " + out[i + 1].text;
                out.erase(out.begin() + static_cast<long>(i) + 1);
                break;
            }
        }
    }

    // 4. Wardriving: "GPS location updated" goes when the "Coord: ..." line
    //    right after it says the same (a MAC cache clear or an extra line
    //    would otherwise push "Scan done." off the screen).
    for (size_t i = 0; out.size() > max_lines && i + 1 < out.size(); ++i) {
        if (out[i].text == "GPS location updated" && starts_with(out[i + 1].text, "Coord:")) {
            out.erase(out.begin() + static_cast<long>(i));
            break;
        }
    }

    // 5. Clip the bottom, as text beyond the firmware's display edge is lost.
    if (out.size() > max_lines) {
        out.resize(max_lines);
    }
    return out;
}

// --- GPX (gps_tracker.cpp:164-187, 228-233, 189-202) ----------------------------------------
//
// Arduino's File::println() terminates with "\r\n" while file.printf("...\n")
// writes a bare "\n"; the firmware mixes both, and so does this output, so a
// track is byte-identical to one recorded by Bruce on an ESP32.

std::string gpx_header() {
    std::string s;
    auto println = [&s](const char* line) {
        s += line;
        s += "\r\n";
    };
    println("<?xml version=\"1.0\" encoding=\"ISO-8859-1\" standalone=\"yes\"?>");
    println("<?xml-stylesheet type=\"text/xsl\" href=\"details.xsl\"?>");
    println("<gpx");
    println("  version=\"1.1\"");
    println("  creator=\"Bruce Firmware\"");
    println("  xmlns=\"http://www.topografix.com/GPX/1/1\"");
    println("  xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"");
    println("  xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd\"");
    println(">");
    println("  <metadata>");
    println("    <name>Bruce GPS Tracker</name>");
    println("    <desc>GPS Tracker using Bruce Firmware</desc>");
    println("    <link href=\"https://bruce.computer\">");
    println("      <text>Bruce Website</text>");
    println("    </link>");
    println("  </metadata>");
    println("  <trk>");
    println("    <name>Bruce Route</name>");
    println("    <desc>GPS route captured by Bruce firmware</desc>");
    println("    <trkseg>");
    return s;
}

std::string gpx_point(double lat, double lng, double ele_m, double hdop, uint32_t sats) {
    std::string s;
    s += format("      <trkpt lat=\"%f\" lon=\"%f\">\n", lat, lng);
    s += "        <sym>Waypoint</sym>\r\n";
    s += format("        <ele>%f</ele>\n", ele_m);
    s += format("        <hdop>%f</hdop>\n", hdop);
    s += format("        <sat>%lu</sat>\n", static_cast<unsigned long>(sats));
    s += "      </trkpt>\r\n";
    return s;
}

std::string gpx_footer() {
    return "    </trkseg>\r\n  </trk>\r\n</gpx>\r\n";
}

std::string tracker_filename(unsigned year, unsigned month, unsigned day, unsigned hour,
                             unsigned minute, unsigned second) {
    return format("%02u%02u%02u_%02u%02u%02u", year % 100, month % 100, day % 100, hour % 100,
                  minute % 100, second % 100) +
           "_gps_tracker.gpx";
}

int build_year() {
    // __DATE__ = "Mmm dd yyyy"
    static const char* const kDate = __DATE__;
    return std::atoi(kDate + 7);
}

Tracker::Options tracker_options_from_env() {
    Tracker::Options opt;
    const char* home = std::getenv("HOME");
    if (home && home[0]) {
        opt.dir = std::string(home) + "/BruceGPS";
    }
    // Test hook (tests/gps_tracker_cli_check.sh): simulate storage that does
    // not respond by delaying every GPX file operation.
    if (const char* d = std::getenv("BRUCE_GPS_TEST_IO_DELAY_MS"); d && std::atoi(d) > 0) {
        opt.io_delay_ms = std::atoi(d);
    }
    return opt;
}

// --- Tracker ----------------------------------------------------------------------

Tracker::Tracker(Options options) : options_(std::move(options)) {
    if (options_.current_year <= 0) {
        options_.current_year = build_year();
    }
}

Tracker::~Tracker() {
    stop(); // queues the footer; the worker finishes it even after we are gone
}

std::string Tracker::filepath() const {
    return filename_.empty() ? std::string() : options_.dir + "/" + filename_;
}

void Tracker::padprintln(const std::string& text, int padx) {
    frame_.lines.push_back(ScreenLine{padx, text});
    ++frame_.version;
}

void Tracker::popup(PopupKind kind, const std::string& text) {
    frame_.popup = kind;
    frame_.popup_text = text;
    ++frame_.version;
}

// gps_tracker.cpp:124-135
void Tracker::display_banner() {
    frame_.title = "GPS Tracker"; // drawMainBorderWithTitle() clears the screen
    frame_.lines.clear();
    frame_.popup = PopupKind::None;
    frame_.popup_text.clear();
    ++frame_.version;
    padprintln("");

    if (gps_coord_count_ > 0) {
        padprintln("File: " + filename_.substr(0, filename_.size() - 4), 2);
        padprintln("GPS Coordinates: " + std::to_string(gps_coord_count_), 2);
        padprintln(chomp(format("Distance: %.2fkm\n", distance_ / 1000)), 2);
    }

    padprintln("");
}

// gps_tracker.cpp:28-38 setup() + begin_gps() up to the wait loop.
void Tracker::start(uint64_t now_ms) {
    display_banner();
    padprintln("Initializing...");
    padprintln("Waiting for GPS data");
    state_ = State::Waiting;
    wait_count_ = 0;
    next_wait_ms_ = now_ms;
}

void Tracker::fail_open(const std::string& message, bool not_found) {
    display_banner();
    padprintln("Initializing...");
    popup(PopupKind::Error, message);
    end(not_found ? EndReason::NotFound : EndReason::OpenFailed);
}

void Tracker::tick(uint64_t now_ms, size_t bytes, bool disconnected, Parser& gps) {
    available_ += bytes;
    disconnected_ = disconnected;

    if (state_ == State::Waiting) {
        // begin_gps(): while (GPSserial.available() <= 0) {...}, once a second.
        if (now_ms < next_wait_ms_) {
            return;
        }
        if (available_ == 0) {
            if (disconnected_) {
                // Linux only: the port went away before any byte arrived.
                popup(PopupKind::Error, "GPS not Found!");
                end(EndReason::NotFound);
                return;
            }
            popup(PopupKind::Message, "Waiting GPS: " + std::to_string(wait_count_) + "s");
            ++wait_count_;
            next_wait_ms_ += kWaitTickMs;
            if (next_wait_ms_ <= now_ms) {
                next_wait_ms_ = now_ms + kWaitTickMs;
            }
            return;
        }
        gps_connected_ = true; // gpsConnected = true
        state_ = State::Running;
        empty_count_ = 0;
        next_pass_ms_ = now_ms; // setup() calls loop() right away
    }

    if (state_ == State::Running && now_ms >= next_pass_ms_) {
        pass(gps);
        // The firmware's 5 s wait never ends early: encode() only runs at the
        // top of a pass, so location.isUpdated() cannot flip during the wait.
        next_pass_ms_ = now_ms + kMaxWaitMs;
    }
}

// gps_tracker.cpp:72-111, one iteration of loop() (the add_coord() file write
// is deferred to flush_io()).
void Tracker::pass(Parser& gps) {
    display_banner();

    if (available_ > 0) {
        empty_count_ = 0;
        available_ = 0;

        if (gps.location.isUpdated()) {
            padprintln("GPS location updated");
            set_position(gps);
            // add_coord(), part 1: what it reads from TinyGPS++.
            if (filename_.empty()) {
                create_filename(gps);
            }
            pending_.lat = gps.location.lat();
            pending_.lng = gps.location.lng();
            pending_.ele = gps.altitude.meters();
            pending_.hdop = gps.hdop.hdop();
            pending_.sats = gps.satellites.value();
            has_pending_ = true;
        } else {
            padprintln("GPS location not updated");
            dump_gps_data(gps);

            if (filename_.empty() && gps.date.year() >= options_.current_year &&
                gps.date.year() < options_.current_year + 5) {
                create_filename(gps);
            }
        }
    } else {
        if (empty_count_ > kMaxEmptyPasses || disconnected_) {
            popup(PopupKind::Error, "GPS not Found!");
            end(EndReason::NotFound);
            return;
        }
        padprintln("No GPS data available");
        ++empty_count_;
    }
}

void Tracker::flush_io() {
    if (has_pending_) {
        has_pending_ = false;
        if (state_ == State::Running) {
            if (!writer_) {
                writer_ = std::make_unique<GpxWriter>(options_.dir, options_.io_delay_ms);
            }
            writer_->write_point(filename_, pending_); // add_coord(), part 2: on the worker
        }
    }
    if (writer_) {
        GpxWriter::Result r;
        while (writer_->take_result(r)) {
            apply(r);
        }
    }
}

bool Tracker::io_idle() {
    flush_io();
    return !has_pending_ && (!writer_ || writer_->idle());
}

// gps_tracker.cpp:113-122
void Tracker::set_position(Parser& gps) {
    const double lat = gps.location.lat();
    const double lng = gps.location.lng();

    if (initial_position_set_) {
        distance_ += Parser::distanceBetween(cur_lat_, cur_lng_, lat, lng);
    } else {
        initial_position_set_ = true;
    }

    cur_lat_ = lat;
    cur_lng_ = lng;
}

// gps_tracker.cpp:137-147
void Tracker::dump_gps_data(Parser& gps) {
    if (!date_time_updated_ && (!gps.date.isUpdated() || !gps.time.isUpdated())) {
        padprintln("Waiting for valid GPS data");
        return;
    }
    date_time_updated_ = true;
    const unsigned year = gps.date.year();
    const unsigned month = gps.date.month();
    const unsigned day = gps.date.day();
    padprintln(chomp(format("Date: %02u-%02u-%02u\n", year, month, day)), 2);
    const unsigned hour = gps.time.hour();
    const unsigned minute = gps.time.minute();
    const unsigned second = gps.time.second();
    padprintln(chomp(format("Time: %02u:%02u:%02u\n", hour, minute, second)), 2);
    padprintln(chomp(format("Sat:  %u\n", static_cast<unsigned>(gps.satellites.value()))), 2);
    padprintln(chomp(format("HDOP: %.2f\n", gps.hdop.hdop())), 2);
}

// gps_tracker.cpp:149-162
void Tracker::create_filename(Parser& gps) {
    const unsigned year = gps.date.year();
    const unsigned month = gps.date.month();
    const unsigned day = gps.date.day();
    const unsigned hour = gps.time.hour();
    const unsigned minute = gps.time.minute();
    const unsigned second = gps.time.second();
    filename_ = tracker_filename(year, month, day, hour, minute, second);
}

// A finished storage job (gps_tracker.cpp:204-240 add_coord(), part 3). A point
// only counts once it is fully written; the "Coord:" line follows the write as
// in the firmware.
void Tracker::apply(const GpxWriter::Result& r) {
    if (r.kind == GpxWriter::Result::Kind::Finalized) {
        if (!r.ok) {
            // The track is incomplete: this always shows, after any earlier
            // error (e.g. "GPS not Found!" from an unplug), and the screen
            // waits for OK instead of auto-returning.
            finalize_error_ = r.error;
            if (frame_.popup == PopupKind::Error && !frame_.popup_text.empty() &&
                frame_.popup_text != finalize_error_) {
                popup(PopupKind::Error, frame_.popup_text + "\n" + finalize_error_);
            } else {
                popup(PopupKind::Error, finalize_error_);
            }
        }
        return;
    }
    if (r.ok) {
        filename_ = r.filename; // the worker may have added a "-N" suffix
        ++gps_coord_count_;
        padprintln(chomp(format("Coord: %.6f, %.6f\n", r.point.lat, r.point.lng)), 2);
        return;
    }
    if (state_ == State::Ended) {
        // The session already ended (Back): the point was lost - report it.
        if (finalize_error_.empty()) {
            finalize_error_ = r.error;
            popup(PopupKind::Error, finalize_error_);
        }
        return;
    }
    fail(r.storage_error ? EndReason::StorageError : EndReason::FileError, r.error);
}

// The firmware prints the error line and leaves at once; keep it readable here
// with the same text as a popup (the screen waits for a key).
void Tracker::fail(EndReason reason, const std::string& line) {
    padprintln(line);
    popup(PopupKind::Error, line);
    end(reason);
}

// gps_tracker.cpp:189-202 (run by ~GPSTracker()): queued on the storage worker,
// which only finalizes a track it created; the result comes back via flush_io().
void Tracker::add_final_file_data() {
    if (writer_ && !finalize_queued_) {
        finalize_queued_ = true;
        writer_->finalize();
    }
}

// end() + ~GPSTracker(): close the track, drop gpsConnected.
void Tracker::end(EndReason reason) {
    if (state_ == State::Ended) {
        return;
    }
    state_ = State::Ended;
    end_reason_ = reason;
    gps_connected_ = false;
    has_pending_ = false;
    add_final_file_data();
}

void Tracker::stop() {
    if (state_ == State::Waiting || state_ == State::Running) {
        end(EndReason::Stopped);
    }
}

// --- shared entry ------------------------------------------------------------------

const char* tracker_root_refusal() {
    return "GPS Tracker must run as your user, not root (it only needs the dialout group)";
}

bool begin_tracker(Session& session, Tracker& tracker, const std::string& port, int baud,
                   uint64_t now_ms) {
#if defined(__linux__)
    return begin_tracker(session, tracker, port, baud, now_ms, static_cast<unsigned>(::geteuid()));
#else
    return begin_tracker(session, tracker, port, baud, now_ms, 1u);
#endif
}

bool begin_tracker(Session& session, Tracker& tracker, const std::string& port, int baud,
                   uint64_t now_ms, unsigned effective_uid) {
    if (effective_uid == 0) {
        // Checked before the UART is touched: a root-owned track in the user's
        // home is never created, whichever front end started us.
        tracker.fail_open(tracker_root_refusal(), false);
        return false;
    }
    OpenResult res;
    if (!session.open(port, baud, res)) {
        tracker.fail_open(res.message, res.error == OpenError::NotFound);
        return false;
    }
    tracker.start(now_ms);
    return true;
}

// --- console front-end ------------------------------------------------------------

#if defined(__linux__)
int gps_tracker_main(int argc, char** argv) {
    struct sigaction sa{};
    sa.sa_handler = on_signal; // not SA_RESTART: usleep() returns at once
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::string port = "/dev/serial0";
    int baud = 115200;
    if (const char* env = std::getenv("GPS_UART"); env && env[0]) {
        port = env;
    }
    if (const char* env = std::getenv("GPS_BAUD"); env && std::atoi(env) > 0) {
        baud = std::atoi(env);
    }
    if (argc >= 1 && argv[0] && argv[0][0]) {
        port = argv[0];
    }
    if (argc >= 2 && argv[1] && std::atoi(argv[1]) > 0) {
        baud = std::atoi(argv[1]);
    }

    // GPS Port = Auto: probe the candidates first (the root refusal below
    // still comes before any port is opened).
    if (port == "Auto" && ::geteuid() != 0) {
        const auto ports = auto_order("/dev/serial0", port_candidates());
        std::printf("Auto: searching GPS on %zu port(s)...\n", ports.size());
        // Bruce's Baudrate list (model::gps_config::baud_rates()).
        const ProbeResult res =
            auto_detect(ports, baud, {9600, 19200, 38400, 57600, 115200}, 1500, &g_cancel_probe);
        if (res.kind != ProbeResult::Kind::Found) {
            std::printf("[error] %s\n", res.message.c_str());
            return 1;
        }
        std::printf("Auto: %s\n", res.message.c_str());
        port = res.port;
        baud = res.baud;
    }

    const Tracker::Options options = tracker_options_from_env();
    std::printf("GPS Tracker on %s @ %d bps -> %s (Ctrl-C to stop)\n", port.c_str(), baud,
                options.dir.c_str());

    Session session;
    Tracker tracker(options);
    uint32_t shown = 0;
    auto show = [&]() {
        const ScreenFrame& f = tracker.frame();
        if (f.version == shown) {
            return;
        }
        shown = f.version;
        std::string title = f.title;
        for (char& c : title) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        std::printf("---- %s ----\n", title.c_str());
        for (const auto& l : f.lines) {
            std::printf("%s%s\n", l.padx >= 2 ? "  " : "", l.text.c_str());
        }
        if (f.popup != PopupKind::None) {
            std::printf("[%s] %s\n", f.popup == PopupKind::Error ? "error" : "popup", f.popup_text.c_str());
        }
    };

    if (!begin_tracker(session, tracker, port, baud, now_ms())) {
        show();
        return 1;
    }
    show();
    while (!g_stop && tracker.state() != Tracker::State::Ended) {
        // Bytes, hang-up flag and parser in one synchronized snapshot.
        session.snapshot([&](size_t bytes, bool hung_up, Parser& gps) {
            tracker.tick(now_ms(), bytes, hung_up, gps);
        });
        tracker.flush_io(); // hands file work to the storage worker
        show();
        usleep(100000);
    }
    session.close(); // release the UART first
    tracker.stop();
    // Let the worker close the track, bounded by the one process-wide shutdown
    // deadline (a second signal exits at once, see on_signal()).
    const auto deadline = shutdown_deadline();
    while (!tracker.io_idle() && std::chrono::steady_clock::now() < deadline) {
        show();
        usleep(50000);
    }
    show();
    if (drain_writers() > 0) { // same deadline: no second wait; logs the file
        std::fprintf(stderr, "error: GPX storage did not finish before shutdown\n");
        return 1;
    }
    if (!tracker.finalize_error().empty()) {
        std::fprintf(stderr, "error: %s\n", tracker.finalize_error().c_str());
        return 1;
    }
    std::printf("Stopped. %d coordinates, %.2fkm%s%s\n", tracker.coord_count(), tracker.distance_m() / 1000,
                tracker.coord_count() > 0 ? " -> " : "",
                tracker.coord_count() > 0 ? tracker.filepath().c_str() : "");
    return tracker.end_reason() == Tracker::EndReason::Stopped ? 0 : 1;
}
#else
int gps_tracker_main(int, char**) {
    std::fputs("GPS Tracker is only available on Linux builds.\n", stdout);
    return 1;
}
#endif

} // namespace platform::gps
