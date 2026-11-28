/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bruce firmware's GPS Tracker (src/modules/gps/gps_tracker.cpp, 1.17) ported
 * as a non-blocking state machine. The firmware runs a blocking loop on the
 * display; here the same steps run from tick() so the LVGL thread never blocks.
 * The texts, the order of the lines, the 1 s "Waiting GPS: Ns" counter, the
 * 5 s pass (MAX_WAIT), the >5 empty passes -> "GPS not Found!" rule, the file
 * name and the GPX content are the firmware's.
 *
 * Output is a ScreenFrame (title + lines + optional popup), i.e. what Bruce's
 * drawMainBorderWithTitle()/padprintln()/displayTextLine()/displayError() would
 * draw. The view renders it; fit_lines() adapts it to the shorter screen.
 */

#pragma once

#include "gps_parser.h"
#include "gpx_writer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace platform::gps {

// One padprintln() line. padx is Bruce's left padding unit (1 = default,
// 2 = the indented detail lines).
struct ScreenLine {
    int padx = 1;
    std::string text;
};

enum class PopupKind {
    None,
    Message, // displayTextLine(): primary-colour box
    Error,   // displayError(): red box
};

struct ScreenFrame {
    std::string title; // as passed to drawMainBorderWithTitle (printTitle uppercases)
    std::vector<ScreenLine> lines;
    PopupKind popup = PopupKind::None;
    std::string popup_text;
    uint32_t version = 0; // bumped on every change
};

// Adapt a frame's lines to a screen that shows at most `max_lines` body lines.
// Bruce text is kept verbatim; when it does not fit, blank lines are dropped
// first, then Wardriving's "Start Scanning..." once "Scan done. ..." follows,
// then Bruce's paired detail lines are merged onto one line ("Date:"+"Time:",
// "Sat:"+"HDOP:", "GPS Coordinates:"+"Distance:"), then Wardriving's "GPS
// location updated" right above its "Coord:" line, and as a last resort the
// bottom is clipped, as on the firmware's display.
std::vector<ScreenLine> fit_lines(const std::vector<ScreenLine>& lines, size_t max_lines);

// GPX 1.1 pieces, byte-identical to the firmware's add_initial_file_data /
// add_coord / add_final_file_data.
std::string gpx_header();
std::string gpx_point(double lat, double lng, double ele_m, double hdop, uint32_t sats);
std::string gpx_footer();

// "YYMMDD_HHMMSS_gps_tracker.gpx" from GPS UTC (create_filename()).
std::string tracker_filename(unsigned year, unsigned month, unsigned day, unsigned hour,
                             unsigned minute, unsigned second);

// The build year: Bruce's CURRENT_YEAR (current_year.h is generated at build).
int build_year();

class Tracker {
public:
    enum class State { Idle, Waiting, Running, Ended };
    enum class EndReason { None, Stopped, NotFound, OpenFailed, StorageError, FileError };

    struct Options {
        std::string dir;      // Bruce: "/BruceGPS" on the SD card; here ~/BruceGPS
        int current_year = 0; // 0 = build_year()
        int io_delay_ms = 0;  // tests only: slow storage simulation
    };

    explicit Tracker(Options options);
    ~Tracker();

    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    // setup(): banner, "Initializing...", then begin_gps()'s "Waiting for GPS
    // data". The port is already open (the caller owns the Session).
    void start(uint64_t now_ms);

    // The port could not be opened. Linux-only path (an ESP32 UART always
    // "opens"): shows the error popup and ends.
    void fail_open(const std::string& message, bool not_found);

    // Phase 1 of a tick, run with the parser locked (Session::with_gps): the
    // firmware's TinyGPS++ reads and screen text. `bytes` = bytes received
    // since the previous call (GPSserial.available()); `disconnected` = the
    // port hung up (USB GPS unplugged) - once the buffered bytes are used up
    // that is reported as "GPS not Found!" at once instead of after Bruce's
    // empty-pass countdown, which still applies to a connected-but-silent
    // receiver. No file I/O happens here.
    void tick(uint64_t now_ms, size_t bytes, bool disconnected, Parser& gps);

    // Phase 2, run WITHOUT the parser lock, on the caller's (UI) thread: hand
    // the point the last pass queued (add_coord()) to the storage worker and
    // apply the worker's finished results (point counted + "Coord:" line, or
    // the error). Never blocks on storage.
    void flush_io();

    // ESC: end() + the destructor's add_final_file_data(). Idempotent and
    // non-blocking: the footer is written by the storage worker.
    void stop();

    // True when the storage worker has nothing in flight and every result was
    // applied (the track is closed after end()). Front ends wait for this -
    // without blocking - before leaving, to report a late write error.
    bool io_idle();

    State state() const { return state_; }
    EndReason end_reason() const { return end_reason_; }
    bool gps_connected() const { return gps_connected_; }
    const ScreenFrame& frame() const { return frame_; }

    const std::string& filename() const { return filename_; }
    std::string filepath() const;
    int coord_count() const { return gps_coord_count_; }
    double distance_m() const { return distance_; }
    // Non-empty when closing the GPX (footer / close) failed.
    const std::string& finalize_error() const { return finalize_error_; }

private:
    using Point = GpxPoint;

    void display_banner();
    void padprintln(const std::string& text, int padx = 1);
    void popup(PopupKind kind, const std::string& text);
    void pass(Parser& gps);
    void dump_gps_data(Parser& gps);
    void set_position(Parser& gps);
    void apply(const GpxWriter::Result& r);
    void add_final_file_data();
    void create_filename(Parser& gps);
    void fail(EndReason reason, const std::string& line);
    void end(EndReason reason);

    Options options_;
    State state_ = State::Idle;
    EndReason end_reason_ = EndReason::None;
    ScreenFrame frame_;

    // begin_gps()
    int wait_count_ = 0;
    uint64_t next_wait_ms_ = 0;
    // loop()
    int empty_count_ = 0;
    uint64_t next_pass_ms_ = 0;
    size_t available_ = 0;
    bool disconnected_ = false;
    bool gps_connected_ = false;

    bool date_time_updated_ = false;
    bool initial_position_set_ = false;
    double cur_lat_ = 0, cur_lng_ = 0;
    double distance_ = 0;
    std::string filename_;
    int gps_coord_count_ = 0;

    bool has_pending_ = false;
    Point pending_;
    std::unique_ptr<GpxWriter> writer_; // created with the first point
    bool finalize_queued_ = false;
    std::string finalize_error_;
};

// ~/BruceGPS for the current user ($HOME). The tracker never runs privileged:
// it only needs read access to the serial port (dialout group).
Tracker::Options tracker_options_from_env();

class Session;

// The one entry point both front ends (GpsAppScreen, --gps-tracker) use to
// start the tracker: refuses to run as root (the track must belong to the
// user; only the dialout group is needed) before the UART is touched, then
// opens `port` and either starts the tracker or shows the open error.
// Returns true when the tracker is running.
bool begin_tracker(Session& session, Tracker& tracker, const std::string& port, int baud,
                   uint64_t now_ms);
// Same, with the effective uid given explicitly (the overload above passes
// geteuid()); lets the root refusal be tested without privileges.
bool begin_tracker(Session& session, Tracker& tracker, const std::string& port, int baud,
                   uint64_t now_ms, unsigned effective_uid);

// The refusal text shown when run as root (exposed for tests).
const char* tracker_root_refusal();

// `<self> --gps-tracker [port] [baud]`: the same tracker on the console (SSH
// testing on the device). main() fills missing args from GPS > Config
// (~/.bruce/gps.conf, after $GPS_UART / $GPS_BAUD), else /dev/serial0 @ 115200
// (LoRa+GNSS Cap). Port "Auto" probes the candidate ports for NMEA first.
// Refuses to run as root.
int gps_tracker_main(int argc, char** argv);

} // namespace platform::gps
