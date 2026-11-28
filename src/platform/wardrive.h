/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bruce firmware 1.17 Wardriving (src/modules/gps/wardriving.cpp) ported onto
 * the in-app GPS reader (platform::gps::Session / Parser, no gpsd), `iw` for
 * the Wi-Fi pass and `bluetoothctl` for the BLE pass. Firmware references are
 * given as wardriving.cpp:<line>.
 *
 * The firmware's blocking setup()/loop() maps 1:1 onto Wardriver::run(): it
 * runs in `<self> --wardrive ...`, a child process (root: `iw scan` and the
 * BLE scan need it). The child draws Bruce's screen into a ScreenFrame and
 * either prints it for a console, or (with --ui) streams it to GpsAppScreen
 * with the line protocol below. Everything that is not I/O is pure and
 * unit-tested (tests/wardrive_selfcheck.cpp).
 */

#pragma once

#include "ble_scan_parser.h"
#include "gps_parser.h"
#include "gps_tracker.h" // ScreenFrame / ScreenLine / PopupKind

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <sys/types.h>
#include <set>
#include <string>
#include <vector>

namespace platform {

// One AP from `iw dev <if> scan`.
struct ScanAp {
    std::string bssid; // as iw prints it ("aa:bb:..")
    std::string ssid;  // iw's \xHH escapes decoded; control bytes stay escaped
    std::string auth;  // Bruce AuthMode name (auth_mode_to_string): "WPA2_PSK", ...
    int channel = 0;
    int freq = 0;   // MHz, as iw reports it
    int signal = 0; // dBm
};

// 802.11 frequency (MHz) -> channel number (2.4, 5 and 6 GHz). 0 if unknown.
int freq_to_channel(int mhz);

// Bruce's auth_mode_to_string() names (wardriving.cpp:203-216) for what the
// ESP32 driver would report, derived from an `iw scan` BSS block: its
// capability Privacy bit and the "Authentication suites:" of its RSN: and
// WPA: elements ("" = element absent).
std::string bruce_auth_mode(bool privacy, bool has_rsn, const std::string& rsn_suites, bool has_wpa,
                            const std::string& wpa_suites);

// iw prints SSID bytes that are not printable (and '\\', and a leading or
// trailing space) as \xHH. Decode them back to the raw bytes (UTF-8 SSIDs),
// keeping control bytes (< 0x20, 0x7f) in their escaped form so a CSV row and
// the screen never get a raw newline. An all-NUL SSID (hidden) becomes "".
std::string decode_iw_ssid(const std::string& escaped);

// Parse `iw dev <if> scan` output into a list of APs.
std::vector<ScanAp> parse_iw_scan(const std::string& text);

namespace wardrive {

// GPS values a scan pass writes (read from TinyGPS++ at the start of the pass).
struct Fix {
    double lat = 0, lng = 0, alt = 0, hdop = 0;
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
};

// WiGLE 1.6 pre-header + header (wardriving.cpp:242-252), each ending in
// "\r\n" like Arduino's File::println(). The identity fields name this device.
std::string wigle_header(const std::string& app_version);
// One row each (wardriving.cpp:277-299 / 383-407), "\n"-terminated like the
// firmware's snprintf("...\n"). Wi-Fi MACs are uppercase (WiFi.BSSIDstr()),
// BLE addresses lowercase (NimBLEAddress::toString()).
std::string wifi_row(const ScanAp& ap, const Fix& fix);
std::string ble_row(const ble::Device& dev, const Fix& fix);
// A CSV field body for Bruce's "\"%s\"": '"' doubled (RFC 4180).
std::string csv_quoted(const std::string& s);

// "YYMMDD_HHMMSS_wardriving.csv" (create_filename(), wardriving.cpp:481-494).
std::string wardriving_filename(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute,
                                unsigned second);

// parseMacToU64() (wardriving.cpp:20-38).
bool parse_mac_u64(const std::string& mac, uint64_t& out);

// alert.txt (loadAlertMACs(), wardriving.cpp:447-479): one MAC per line,
// '#' comments, trimmed and lowercased.
std::set<std::string> parse_alert_macs(const std::string& text);
std::string alert_sample(); // the 4 comment lines, "\r\n" each

// --- GpsAppScreen line protocol (child -> screen) ----------------------------
//   @F <title>          frame start (drawMainBorderWithTitle)
//   @1 <text> / @2 <text>   padprintln() line, padx 1 / 2
//   @P M|E|W <text>     popup: displayTextLine / displayError / displayError(.., true)
//   @G 0|1              gpsConnected
//   @.                  frame end (the screen applies the frame atomically)
//   @X <reason>         the run ended: stopped|notfound|open|storage|error
// Any other line is log output (stderr of the child, sudo, ...).
std::string encode_frame(const gps::ScreenFrame& frame, bool popup_waits, bool gps_connected);

class FrameDecoder {
public:
    // Feed raw child output (any chunking). Returns true when a frame or the
    // end marker was completed.
    bool feed(const std::string& text);
    const gps::ScreenFrame& frame() const { return frame_; }
    bool popup_waits() const { return popup_waits_; }
    bool gps_connected() const { return gps_connected_; }
    bool ended() const { return !end_reason_.empty(); }
    const std::string& end_reason() const { return end_reason_; }
    bool any_frame() const { return frame_.version > 0; }
    // Non-protocol lines, most recent last (at most 20).
    const std::vector<std::string>& log() const { return log_; }

private:
    void line(const std::string& l);
    std::string partial_;
    gps::ScreenFrame frame_;
    gps::ScreenFrame building_;
    bool building_waits_ = false;
    bool in_frame_ = false;
    bool popup_waits_ = false;
    bool gps_connected_ = false;
    std::string end_reason_;
    std::vector<std::string> log_;
};

// GpsAppScreen's Back while the run's result has not been handled yet (the
// screen still in its running state): after ProcessRunner::stop() - reader
// joined, poll timer gone - the output the timer had not delivered is fed in
// here, also when the child had already exited before Back. True when the run
// ended on a storage error, which the screen must then still show (OK).
bool storage_error_after_back(FrameDecoder& decoder, const std::string& undelivered);

// --- storage ------------------------------------------------------------------

// The firmware's /BruceWardriving on the SD card, here <home>/BruceWardriving.
//
// Privilege boundary. The wardriving child runs as root for the scans; the
// files in the user's home are handled like this:
//  - setup() opens the home once with the process's own credentials and
//    takes its owner from fstat() - not from $SUDO_UID (plan_storage()). When
//    $SUDO_UID is set it must parse strictly and name that same owner,
//    otherwise the storage is unusable ("Storage setup error"); so is a home
//    that cannot be opened. Every failure gives the same message: --home is
//    no oracle on paths the user cannot reach.
//  - A home owned by another user than root is then only used through a
//    short-lived forked helper that switches to that user's credentials
//    (setgroups / setresgid / setresuid, verified with getres[ug]id and a
//    setresuid(0) that must fail, then PR_SET_DUMPABLE 0). The helper opens
//    the home by path with the user's rights, checks it is the inode setup()
//    saw, opens or creates the file with O_NOFOLLOW (O_CREAT|O_EXCL for a new
//    one, O_APPEND for an existing one - nothing is ever truncated), checks
//    it is a regular file with one link owned by the user - removing a file
//    it has just created if not - and hands the descriptor back over a
//    socketpair (SCM_RIGHTS). Root re-checks it and only writes through it.
//    No path lookup in the home runs with root's rights; nothing is chowned.
//  - A home owned by root (the app itself running as root), and any non-root
//    run, use the same opens in-process, relative to the descriptor setup()
//    opened. For root that home and the BruceWardriving folder must also be
//    root's and not group/other writable (no other user can prepare them).
//
// Durability: rows are written with write(2) during a pass, with SIGXFSZ
// blocked (a file size limit is an EFBIG error, not a kill). Closing the file
// (Bruce's file.close() at the end of a pass) hands fdatasync() + close() to
// one worker thread, so slow storage never blocks a pass, Back or the GPS
// release. A failure there ends the run at the next tick of the inter-pass
// wait (or at exit, reported over any other end reason); wait_idle() bounds
// the wait at shutdown.

// Who the home's files are opened as.
struct StorageOwner {
    bool switch_creds = false; // false: the process's own credentials
    uid_t uid = 0;
    gid_t gid = 0;
    std::vector<gid_t> groups; // supplementary groups (getgrouplist)
};

// Strict decimal uid/gid: digits only, no sign or space, below (uid_t)-1.
bool parse_id(const char* text, uint32_t& out);

// The owner decision of Storage::setup() (pure, unit-tested). `sudo_uid` is
// $SUDO_UID as given (nullptr when unset); home_uid is the opened home's
// st_uid. `lookup` fills the primary gid and supplementary groups of a uid
// (false: no passwd entry). Not root: own credentials. Root: the home's
// owner; with $SUDO_UID set it must be valid and equal to it.
struct StoragePlan {
    bool ok = false;
    StorageOwner owner;
};
StoragePlan plan_storage(bool is_root, const char* sudo_uid, uid_t home_uid, gid_t home_gid,
                         const std::function<bool(uid_t, gid_t&, std::vector<gid_t>&)>& lookup);

struct StorageOptions {
    const char* sudo_uid = nullptr; // $SUDO_UID as given
    int sync_delay_ms = 0;          // tests: storage that takes this long to sync
    int sync_fault_errno = 0;       // tests: fdatasync() of the CSV fails with this
    bool test_switch_to_self = false;  // tests: use the helper, switching to our own uid
    bool test_expect_other_owner = false; // tests: validation expects another owner
    bool test_strict_dirs = false;     // tests: the root in-process directory checks, as ourselves
};

class Storage {
public:
    using Owner = StorageOwner;

    explicit Storage(std::string home, StorageOptions options = {});
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    // getFsStorage(): the home can be used (decided once, see above).
    bool setup();
    // Read `name` (at most kMaxRead bytes, cut at the last line end); false
    // on error, with *err = errno (ENOENT: missing).
    bool read(const std::string& name, std::string& text, int* err = nullptr);
    static constexpr size_t kMaxRead = 1 << 20;
    // Create `name` with `text` only if it does not exist yet.
    bool create(const std::string& name, const std::string& text);
    // FILE_WRITE on a new file (is_new = true) else FILE_APPEND. -1 on error.
    int open(const std::string& name, bool& is_new);
    bool write(int handle, const std::string& data);
    void close(int handle); // queues fdatasync() + close()
    std::string take_error(); // a deferred sync/close error, "" if none
    bool wait_idle(int timeout_ms);

    std::string dir() const { return home_ + "/BruceWardriving"; }
    const Owner& owner() const { return owner_; }

    // Tests: errno of the last failed open (EMLINK hard link, EINVAL not a
    // regular file, EPERM wrong owner, ELOOP symlink, EXDEV home replaced).
    int last_errno() const { return last_errno_; }

private:
    int open_file(const std::string& name, int flags, int& err);
    struct Sync;
    std::string home_;
    StorageOptions options_;
    Owner owner_;
    bool setup_done_ = false;
    bool setup_ok_ = false;
    int hfd_ = -1; // the home, for the in-process opens
    bool strict_dirs_ = false; // root in-process: home/folder owner and mode checked
    uint64_t home_dev_ = 0, home_ino_ = 0;
    int last_errno_ = 0;
    std::shared_ptr<Sync> sync_;
};

// A scan tool run (no shell): stdout+stderr captured, an empty stdin pipe,
// its own process group. Ends at the tool's exit, at `timeout_ms` or when
// *stop is set; ending early (or leaving descendants) terminates the whole
// group - SIGTERM, then SIGKILL after `grace_ms` - before the leader is
// reaped, also when the tool closed its output and kept running.
//
// `input` is written to the tool's stdin as the tool reads it (non-blocking,
// at most 64 KiB per pass of the same loop, so Back and the timeout are
// checked between writes and at least every 50 ms), which is then
// closed; `input_incomplete` when the tool exited or closed its stdin first.
// With a `probe`, once `at_ms` passed while the tool still runs, what the
// pipe already holds (at most `max_snapshot` bytes) is read and
// probe->fn(output so far) is called - the tool keeps running meanwhile - and
// the tool's group is then ended like on a stop, but without `aborted`
// (`ended_early`, status 0). A tool that exits first gets no probe call.
struct Capture {
    std::string out;
    int status = -1;               // exit code, -1 when killed / not run
    bool aborted = false;          // stopped or timed out
    bool ended_early = false;      // ended after the probe
    bool input_incomplete = false; // not all of `input` reached the tool
};
struct CaptureProbe {
    int at_ms = 0;
    size_t max_snapshot = 256 * 1024;
    std::function<void(const std::string& out)> fn;
};
Capture run_capture(const std::vector<std::string>& argv, int timeout_ms, const std::atomic<bool>* stop,
                    int grace_ms = 100, const std::string& input = {}, const CaptureProbe* probe = nullptr);
namespace testing {
// The write() run_capture() feeds stdin with (selfcheck only: slow partial writes).
extern ssize_t (*capture_write)(int fd, const void* buf, size_t n);
} // namespace testing

// --- the port of class Wardriving --------------------------------------------

class Wardriver {
public:
    enum class EndReason { None, Stopped, NotFound, OpenFailed, StorageError };

    struct Hooks {
        std::function<uint64_t()> millis;           // ms since the run started
        std::function<void(uint32_t)> delay;        // vTaskDelay; may return early on stop
        std::function<bool()> esc;                  // check(EscPress) / SIGTERM
        // GPSserial.begin(): open the port. false + message when it cannot be
        // opened; `not_found` for a missing node (-> "GPS not Found!").
        std::function<bool(std::string& message, bool& not_found)> open_gps;
        // The GPS reader: fn(bytes received since the last call, hung up, parser).
        std::function<void(const std::function<void(size_t, bool, gps::Parser&)>&)> gps;
        // WiFi.scanNetworks() (blocking). `ok` false = the scan failed.
        std::function<std::vector<ScanAp>(bool& ok)> scan_wifi;
        // BLE scan for `seconds` (blocking). false = BLE could not start.
        // Every device should come with the RSSI it advertised (NimBLE's
        // getRSSI()); for one the scan output gave none, the hook may look it
        // up while discovery still runs, if need_rssi(mac) (not logged yet).
        // A device left with rssi 0 is not logged in this pass.
        std::function<bool(int seconds, const std::function<bool(const std::string& mac)>& need_rssi,
                           ble::ScanResult& out)>
            scan_ble;
        // Called whenever the drawn screen changed (before each wait / scan).
        std::function<void(const gps::ScreenFrame&, bool popup_waits, bool gps_connected)> draw;
        std::function<void(const std::string&)> serial; // Serial.println()
    };

    struct Options {
        bool scan_wifi = true;
        bool scan_ble = false;
        bool auto_port = false; // GPS Port = Auto: "Searching GPS..." while probing
        int current_year = 0;   // CURRENT_YEAR, 0 = gps::build_year()
        std::string app_version = "0.0.0";
    };

    Wardriver(Options options, Hooks hooks, Storage& storage);

    // setup() + loop(); returns when the run ended (ESC, GPS not Found!, an
    // open or storage error).
    void run();

    EndReason end_reason() const { return end_reason_; }

    // Shutdown found a failed sync/close of the CSV after the run ended:
    // draws the "Failed to write file" error (waits for OK) and makes the
    // end reason StorageError, whatever ended the run.
    void report_storage_error(const std::string& detail);
    const gps::ScreenFrame& frame() const { return frame_; }
    bool popup_waits() const { return popup_waits_; }
    bool gps_connected() const { return gps_connected_; }
    int wifi_count() const { return wifi_network_count_; }
    int ble_count() const { return bluetooth_device_count_; }
    int alert_count() const { return found_mac_address_count_; }
    const std::string& filename() const { return filename_; }
    double distance_m() const { return distance_; }
    uint32_t mac_cache_clears() const { return mac_cache_clears_; }

    static constexpr size_t kMaxRegisteredMacs = 250; // MAX_REGISTERED_MACS
    static constexpr int kScanTime = 5;               // ble_common.h SCANTIME

private:
    void setup();
    bool begin_gps();
    void loop();
    void end(EndReason reason);
    void display_banner();
    void dump_gps_data(gps::Parser& gps);
    void set_position(gps::Parser& gps);
    void scan_wifi_ble(const Fix& fix);
    void enforce_registered_mac_limit();
    void load_alert_macs();
    void check_for_alert(const std::string& mac, const std::string& type, const std::string& name);
    void create_filename(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s);
    void padprintln(const std::string& text, int padx = 1);
    void popup(gps::PopupKind kind, const std::string& text, bool waits = false);
    void display_error(const std::string& text, bool wait_key = false);
    void display_text_line(const std::string& text);
    void storage_failed(const std::string& text);
    bool deferred_storage_error();
    void flush();
    void wait(uint32_t ms);
    uint64_t take_bytes(bool& hung_up);

    Options options_;
    Hooks hooks_;
    Storage& storage_;

    gps::ScreenFrame frame_;
    bool popup_waits_ = false;
    bool dirty_ = false;
    EndReason end_reason_ = EndReason::None;
    bool ended_ = false;
    bool gps_connected_ = false;
    bool return_to_menu_ = false;
    size_t available_ = 0;

    bool date_time_updated_ = false;
    bool initial_position_set_ = false;
    double cur_lat_ = 0, cur_lng_ = 0;
    double distance_ = 0;
    uint64_t session_start_ms_ = 0;
    std::string filename_;
    std::set<uint64_t> registered_macs_;
    std::set<std::string> alert_macs_;
    int wifi_network_count_ = 0;
    int bluetooth_device_count_ = 0;
    int found_mac_address_count_ = 0;
    uint32_t mac_cache_clears_ = 0;
};

const char* end_reason_name(Wardriver::EndReason r);

} // namespace wardrive

// Entry point for `--wardrive wifi|ble|both [--port <node>|Auto] [--baud <bps>]
// [--home <dir>] [--iface <if>] [--ui] [iface]` (also --wifi/--ble/--both).
// main() fills a missing --port / --baud from GPS > Config (gps.conf, env on
// top); the GPS menu passes every option, so the root child never reads HOME:
// the files go to <--home>/BruceWardriving, else (root) the sudo user's home
// from the passwd database, else $HOME. As root the files are created with
// the credentials of the home's owner (see Storage). SIGTERM / Ctrl-C (Back) stops within ~100 ms, also
// during an Auto probe or a scan. Exit 0 when stopped, 1 when the run ended
// on an error, 2 on bad options.
int wardrive_main(int argc, char** argv);

} // namespace platform
