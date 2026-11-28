/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "tool_catalog.h"

#include "gps_config.h"
#include "id_parse.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace model {
namespace {

// The "Use CC1101 CAP" action re-execs this very binary with --cc1101-detect.
// The app installs outside PATH (/usr/share/APPLaunch/bin), so resolve the
// running executable instead of relying on execvp's PATH search; fall back to
// the bare name if /proc is unavailable.
std::string self_exe_path() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return "bruce_cardputerzero";
    }
    buf[n] = '\0';
    return std::string(buf);
}

// Single-quote a word for /bin/sh. Only ever applied to fixed catalog argv
// (e.g. {"responder", "-I", "wlan0"}), never to user or scan-derived text.
std::string sh_quote(const std::string& word) {
    std::string out = "'";
    for (const char c : word) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
}

// Where user scripts live: $BRUCE_SCRIPTS_DIR, else ~/.bruce/scripts.
std::string scripts_dir() {
    if (const char* env = std::getenv("BRUCE_SCRIPTS_DIR"); env && env[0] != '\0') {
        return env;
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::string(home) + "/.bruce/scripts";
    }
    return "/root/.bruce/scripts";
}

// ToolAction stores label/description as const char*, so dynamically discovered
// script names need storage that outlives catalog init. A function-local deque
// (stable addresses across push_back, unlike vector) owns them for the process
// lifetime. ponytail: scan runs once at first catalog access, so new scripts
// need an app restart - fine for v1; make the catalog rebuildable if that itches.
std::vector<ToolAction> scan_scripts() {
    static std::deque<std::string> pool;
    const std::string dir = scripts_dir();

    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* entry = readdir(d)) {
            if (entry->d_name[0] == '.') {
                continue; // skip ., .., and dotfiles
            }
            const std::string path = dir + "/" + entry->d_name;
            struct stat st{};
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            if ((st.st_mode & S_IXUSR) == 0) {
                continue; // executable bit required - run via the script's shebang
            }
            names.emplace_back(entry->d_name);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());

    std::vector<ToolAction> actions;
    for (const auto& name : names) {
        pool.push_back(name);
        const char* label = pool.back().c_str();
        pool.push_back(dir + "/" + name);
        const std::string& full = pool.back();
        actions.push_back(ToolAction{label, full.c_str(), {full}});
    }

    if (actions.empty()) {
        // The menu can't show an empty runnable category, so guide the user.
        pool.push_back("No scripts yet");
        const char* label = pool.back().c_str();
        pool.push_back("Add an executable script (shebang + chmod +x) to " + dir);
        const char* desc = pool.back().c_str();
        actions.push_back(ToolAction{
            label, desc,
            {"sh", "-c",
             "echo 'No executable scripts found in " + dir + "'; "
             "echo 'Add a script with a shebang, chmod +x, then restart Bruce.'"}});
    }

    return actions;
}

} // namespace

std::string rf_recordings_dir() {
    if (const char* env = std::getenv("BRUCE_RF_DIR"); env && env[0] != '\0') {
        return env;
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::string(home) + "/BruceRF";
    }
    return "/tmp/BruceRF";
}

std::string rfid_recordings_dir() {
    if (const char* env = std::getenv("BRUCE_NFC_DIR"); env && env[0] != '\0') {
        return env;
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::string(home) + "/BruceNFC";
    }
    return "/tmp/BruceNFC";
}

namespace {

std::vector<ToolAction> build_rf_actions(bool cc1101_present, int cs_gpio, int gdo0_gpio) {
    static std::deque<std::string> pool;
    pool.clear();
    auto own = [](std::string value) -> const char* {
        pool.push_back(std::move(value));
        return pool.back().c_str();
    };

    std::vector<ToolAction> actions;
    ToolAction detect_action{
        "Detect / refresh CC1101 CAP",
        "Power and probe the CAP, then return to this menu to refresh RF actions.",
        {self_exe_path(), "--cc1101-detect"}};
    if (!cc1101_present) {
        actions.push_back(std::move(detect_action));
    }

    actions.push_back({"Scan / Capture", "Identify received OOK protocols.", {}, false, false, false, false,
                       RfTool::ProtocolMonitor});
    actions.push_back({"Signal Spectrum", "Inspect received OOK pulses.", {}, false, false, false, false, RfTool::Scan});
    actions.push_back({"RSSI Spectrum", "Sweep received power by frequency.", {}, false, false, false, false,
                       RfTool::Spectrum});
    actions.push_back({"Record RAW", "Record and export OOK pulses to a .sub file.", {}, false, false, false, false,
                       RfTool::Record});
    actions.push_back({"Spectrogram", "View recent RSSI sweeps over time.", {}, false, false, false, false,
                       RfTool::Spectrogram});

    if (cc1101_present) {
        actions.push_back(std::move(detect_action));
        actions.push_back(ToolAction{
            "Radio status / RSSI (433.92)",
            own("Native CC1101 RX status using detected pins: CS=G" + std::to_string(cs_gpio) +
                " GDO0=G" + std::to_string(gdo0_gpio)),
            {self_exe_path(), "--rf-info", "433.92"}});

        const std::string dir = rf_recordings_dir();
        actions.push_back(ToolAction{
            "OOK test signal",
            "Send the fixed Princeton bench-test pattern at 433.92 MHz.",
            {self_exe_path(), "--rf-send", "A5C39A", "433.92", "350", "10"}});

        std::vector<std::string> files;
        if (DIR* directory = opendir(dir.c_str())) {
            while (dirent* entry = readdir(directory)) {
                const std::string name = entry->d_name;
                if (name.size() < 5 || name.substr(name.size() - 4) != ".sub") continue;
                const std::string path = dir + "/" + name;
                struct stat st{};
                if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) files.push_back(path);
            }
            closedir(directory);
        }
        std::sort(files.begin(), files.end());
        for (const auto& path : files) {
            const size_t slash = path.find_last_of('/');
            const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
            actions.push_back(ToolAction{
                own("Replay " + name),
                own("Transmit the RAW durations and frequency stored in " + path + "."),
                {self_exe_path(), "--rf-replay", path}});
        }
        if (files.empty()) {
            actions.push_back(ToolAction{
                "No saved RAW files yet",
                own("Use Record + save; .sub files from " + dir + " will appear here."),
                {"sh", "-c", "echo 'No saved .sub files yet. Use Record RAW and select Save.'"}});
        }
    }

    actions.push_back(ToolAction{
        "Capture 433MHz (30s, RTL-SDR dongle)", "timeout 30 rtl_433",
        {"sh", "-c",
         "command -v rtl_433 >/dev/null 2>&1 || "
         "{ echo 'rtl_433 is not installed (and it needs an RTL-SDR USB dongle).'; "
         "echo 'Run: sudo apt install rtl-433'; exit 127; }; timeout 30 rtl_433 2>&1"}});
    actions.push_back(ToolAction{
        "List SDR devices (RTL-SDR dongle)", "rtl_test -t",
        {"sh", "-c",
         "command -v rtl_test >/dev/null 2>&1 || "
         "{ echo 'rtl_test is not installed (and it needs an RTL-SDR USB dongle).'; "
         "echo 'Run: sudo apt install rtl-sdr'; exit 127; }; rtl_test -t 2>&1"}});
    actions.push_back(ToolAction{
        "FM radio RX 100.0MHz (RTL-SDR)", "rtl_fm | aplay",
        {"sh", "-c",
         "command -v rtl_fm >/dev/null 2>&1 || "
         "{ echo 'rtl_fm is not installed. Run: sudo apt install rtl-sdr'; exit 127; }; "
         "rtl_fm -f 100M -M wbfm -s 200k -r 48k 2>/dev/null | aplay -r 48k -f S16_LE 2>&1 || "
         "echo 'playback failed - needs an RTL-SDR dongle + alsa-utils (aplay)'"}});
    return actions;
}

// In-memory toggle for the RFID "Setup Modules >" drill-down (same pattern as
// tz_picker_open()): the detect rows and the PN532/libnfc setup help live in a
// sub-view so the main RFID list only carries each present module's tag ops.
// Flipped by on_navigation_action() / on_category_back(), read on every rebuild.
bool& rfid_setup_open() {
    static bool open = false;
    return open;
}

// Build the "RFID" rows. The main view shows a module's tag operations only when
// it was detected; the per-module "Detect" rows and the PN532/libnfc setup help
// are tucked behind a "Setup Modules >" drill-down. Mirrors build_rf_actions().
std::vector<ToolAction> build_rfid_actions(bool st25r, bool rfid2, bool pn532) {
    std::vector<ToolAction> actions;

    if (rfid_setup_open()) {
        // "Setup Modules" sub-view: a Back row, then the native detect probes
        // for every module and the PN532/libnfc setup help.
        actions.push_back({"<  Back", "Return to the RFID menu.", {}});
        actions.push_back({"ST25R: Detect chip",
                           "Probe the ST25R3916 on SPI then every I2C bus at 0x50; reads IC_IDENTITY.",
                           {self_exe_path(), "--st25r-detect"}});
        actions.push_back({"RFID2: Detect reader",
                           "Probe every I2C bus for an MFRC522/WS1850S and read its VersionReg.",
                           {self_exe_path(), "--rfid2-detect"}});
        actions.push_back({"PN532: Detect reader",
                           "Native GetFirmwareVersion probe on I2C (0x24) and SPI (spidev0.0 / $PN532_SPIDEV).",
                           {self_exe_path(), "--pn532-detect"}});
        actions.push_back({"PN532: Setup / buses help",
                           "How to point libnfc at your PN532, and list available buses.",
                           {"sh", "-c",
                            "echo 'PN532 via libnfc. Set the device persistently in /etc/nfc/libnfc.conf:'; "
                            "echo '  device.connstring = \"pn532_i2c:/dev/i2c-1\"   (or pn532_uart:/dev/ttyS0,'; "
                            "echo '   pn532_spi:/dev/spidev0.0:500000)  and  allow_autoscan = true'; echo; "
                            "echo 'I2C buses:'; ls /dev/i2c-* 2>/dev/null || echo '  (none; enable i2c overlay)'; "
                            "echo 'SPI:'; ls /dev/spidev* 2>/dev/null || echo '  (none)'; "
                            "echo 'Serial:'; ls /dev/ttyS* /dev/ttyUSB* /dev/ttyAMA* 2>/dev/null || echo '  (none)'; "
                            "echo; nfc-list -v 2>&1 | head -4 || echo 'install libnfc-bin'"}});
        return actions;
    }

    // --- M5 RF+NFC CAP: ST25R3916 over HAT SPI, or an I2C NFC unit at 0x50 ---
    if (st25r) {
        // These three open the live RfidScreen (Tag-O-Matic-style continuous
        // reader) via rfid_tool; the argv stays as the CLI fallback and lets the
        // screen find this binary for the Clone emulate child.
        ToolAction read{"Read tag (UID/ATQA/SAK)",
                        "Read an ISO14443A tag's UID/ATQA/SAK on the detected ST25R3916 (SPI CAP or I2C @0x50).",
                        {self_exe_path(), "--rfid-read"}};
        read.rfid_tool = RfidTool::Read;
        actions.push_back(read);
        ToolAction dump{"Dump tag (Type-2 / NTAG)",
                        "Read UID then dump the data pages of a Type-2 tag (NTAG/Ultralight).",
                        {self_exe_path(), "--rfid-dump"}};
        dump.rfid_tool = RfidTool::Dump;
        actions.push_back(dump);
        ToolAction clone{"Clone -> emulate tag",
                         "Read a tag's UID, then emulate it as an NFC-A target until you go back.",
                         {self_exe_path(), "--rfid-clone"}};
        clone.rfid_tool = RfidTool::Clone;
        actions.push_back(clone);
    }

    // --- M5 RFID2 unit: MFRC522 / WS1850S over I2C (native driver) ---
    if (rfid2) {
        actions.push_back({"Read tag (UID/ATQA/SAK)",
                           "Read a tag's UID/ATQA/SAK on the detected M5 RFID2 (MFRC522 @0x28 on I2C).",
                           {self_exe_path(), "--rfid2-read"}});
        actions.push_back({"Dump tag (Classic / NTAG)",
                           "Dump MIFARE Classic 1K/4K (key dictionary) or Ultralight/NTAG pages.",
                           {self_exe_path(), "--rfid2-dump"}});
    }

    // --- PN532 tag operations over libnfc (shown only when a PN532 answered) --
    if (pn532) {
        actions.push_back({"Scan via I2C (/dev/i2c-1)",
                           "Test a PN532 wired on I2C bus 1 and read any tag present (nfc-list).",
                           {"sh", "-c",
                            "LIBNFC_DEFAULT_DEVICE=pn532_i2c:/dev/i2c-1 nfc-list -v 2>&1 || "
                            "echo 'No PN532 on I2C bus 1. Check wiring / install libnfc-bin.'"}});
        actions.push_back({"Scan via UART (/dev/ttyS0)",
                           "Test a PN532 wired on UART and read any tag present (nfc-list).",
                           {"sh", "-c",
                            "LIBNFC_DEFAULT_DEVICE=pn532_uart:/dev/ttyS0 nfc-list -v 2>&1 || "
                            "echo 'No PN532 on ttyS0. Check wiring/baud / install libnfc-bin.'"}});
        actions.push_back({"Poll for tag (configured)",
                           "nfc-poll using the device in /etc/nfc/libnfc.conf or $LIBNFC_DEFAULT_DEVICE.",
                           {"sh", "-c",
                            "nfc-poll 2>&1 || echo 'No PN532 found. Use a Scan entry to test the bus, "
                            "or set /etc/nfc/libnfc.conf. Needs libnfc-bin.'"}});
        actions.push_back({"Dump MIFARE Classic",
                           "nfc-mfclassic read (default keys) to /tmp/bruce-mfc.mfd.",
                           {"sh", "-c",
                            "nfc-mfclassic r a /tmp/bruce-mfc.mfd 2>&1 || "
                            "echo 'Needs a PN532 + MIFARE Classic tag (default keys). Install libnfc-bin.'"}});
        actions.push_back({"Dump Ultralight/NTAG",
                           "nfc-mfultralight read to /tmp/bruce-ul.mfd.",
                           {"sh", "-c",
                            "nfc-mfultralight r /tmp/bruce-ul.mfd 2>&1 || "
                            "echo 'Needs a PN532 + Ultralight/NTAG tag. Install libnfc-bin.'"}});
        actions.push_back({"Emulate NDEF tag",
                           "Emulate a Type-2 NDEF tag (nfc-emulate-forum-tag2).",
                           {"sh", "-c",
                            "nfc-emulate-forum-tag2 2>&1 || "
                            "echo 'Needs a PN532. nfc-emulate-forum-tag2 is in libnfc-examples.'"}});
        actions.push_back({"Emulate UID 01:02:03:04",
                           "Emulate a tag advertising a fixed UID (nfc-emulate-uid).",
                           {"sh", "-c",
                            "nfc-emulate-uid 01020304 2>&1 || "
                            "echo 'Needs a PN532. nfc-emulate-uid is in libnfc-examples.'"}});
    }

    // Always offer the way into the detect/setup sub-view (empty argv = nav row).
    actions.push_back({"Setup Modules  >",
                       "Detect ST25R / RFID2 / PN532 readers and open the PN532/libnfc setup help.",
                       {}});
    return actions;
}

// --- GPS menu (Bruce firmware GpsMenu) --------------------------------------
//
// The GPS category is an in-place drill-down (like the timezone picker): one
// in-memory view state, rows rebuilt from it on every menu entry, navigation
// rows with an empty argv. Labels, order and headers follow Bruce 1.17's
// GpsMenu.cpp / settings.cpp; see build_gps_actions().

// --- Nmap guided flow: discover hosts -> pick host -> Fast/Full port scan -----
//
// The menu is a flat two-level list, so the drill-down is driven by two tiny
// state files under /tmp/bruce-nmap-<uid>: `hosts` (one live IP per line, written by
// the discovery step) and `selected` (the host the user tapped). The "Nmap"
// category's action list is rebuilt from this state on every menu entry
// (refresh_dynamic_categories), and host/"back" rows carry an empty argv so the
// menu treats them as navigation (on_navigation_action) rather than tools.

// Per-uid so a dir created by another user (or a stale root-owned one from an
// older build) can't block or spoof the flow. Digits only, so it is safe to
// embed single-quoted in the discovery script.
std::string nmap_dir() { return "/tmp/bruce-nmap-" + std::to_string(getuid()); }

// Create the state dir (0700) if missing and vet it: it must be a real
// directory (not a symlink) owned by us with no group/other access, since it
// lives in world-writable /tmp and root-run rows write into it. The app (never
// root scripts) is the only creator of this dir. Returns "" when safe, else the reason
// (logged to stderr); callers must then not use the dir at all.
std::string ensure_nmap_dir() {
    const std::string d = nmap_dir();
    std::string problem;
    if (mkdir(d.c_str(), 0700) != 0 && errno != EEXIST) {
        problem = std::string("mkdir failed: ") + std::strerror(errno);
    } else {
        struct stat st {};
        if (lstat(d.c_str(), &st) != 0) {
            problem = std::string("lstat failed: ") + std::strerror(errno);
        } else if (S_ISLNK(st.st_mode)) {
            problem = "is a symlink";
        } else if (!S_ISDIR(st.st_mode)) {
            problem = "not a directory";
        } else if (st.st_uid != getuid()) {
            problem = "owned by uid " + std::to_string(st.st_uid) + ", expected " +
                      std::to_string(getuid());
        } else if ((st.st_mode & 077) != 0) {
            char mode[8];
            std::snprintf(mode, sizeof mode, "%04o", static_cast<unsigned>(st.st_mode & 07777));
            problem = std::string("permissions ") + mode + ", expected 0700";
        }
    }
    if (!problem.empty()) {
        std::fprintf(stderr, "nmap: unsafe state dir %s: %s; not using it\n", d.c_str(),
                     problem.c_str());
    }
    return problem;
}

// Shared by the Nmap discovery row and Wi-Fi "Scan LAN hosts (nmap)": pick the
// default-route interface's IPv4 network (fallback: first non-lo, non-bridge
// address) and clamp very wide prefixes (< /24) to /24 so a sweep stays short.
// Sets $dev (may be empty) and $net (empty if no network).
constexpr const char* kNmapSubnetSh =
    "dev=$(ip -o route show default 2>/dev/null | awk '{for(i=1;i<NF;i++) if($i==\"dev\"){print $(i+1); exit}}'); "
    "net=$(ip -o -f inet addr show ${dev:+dev \"$dev\"} 2>/dev/null | awk '$2!=\"lo\"{print $4; exit}'); "
    "[ -z \"$net\" ] && net=$(ip -o -f inet addr show 2>/dev/null | awk '$2!=\"lo\" && $2!~/^(docker|veth|br-|virbr)/{print $4; exit}'); "
    "case \"${net#*/}\" in [0-9]|1[0-9]|2[0-3]) net=\"${net%/*}/24\";; esac; ";

// Evil Portal clone-AP drill-down state: `targets` holds one "channel\tssid"
// line per scanned AP, `selected` the picked line (same pattern as bruce-nmap).
std::string portal_dir() { return "/tmp/bruce-portal"; }

// "channel\tssid" -> {channel, ssid}.
std::pair<std::string, std::string> split_target(const std::string& line) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
        return {"1", line};
    }
    return {line.substr(0, tab), line.substr(tab + 1)};
}

std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
    out.flush();
    return static_cast<bool>(out);
}

// Shared "nmap missing" guard. Store installs may skip Recommends (nmap), so
// point at the in-app installer first, then the manual command.
constexpr const char* kNmapMissingSh =
    "command -v nmap >/dev/null 2>&1 || { echo 'nmap is not installed - run System / Clock > "
    "\"Install optional tools (apt)\" or: sudo apt install nmap'; exit 127; }; ";

// Discovery parser: nmap normal output (-oN) -> one host per line,
// "ip<TAB>mac<TAB>vendor<TAB>flag" (flag: gw | self | empty), gateway first,
// then numeric IP order. $gw/$me are the default gateway and this device.
// The sort key is a zero-padded prefix stripped again by cut, so this works
// with mawk, gawk and busybox awk/sort alike.
constexpr const char* kNmapHostsAwk = R"SH('
function flush_host(  f, q, k) {
    if (ip == "") return
    f = (ip == gw) ? "gw" : ((ip == me) ? "self" : "")
    split(ip, q, ".")
    k = sprintf("%d%03d%03d%03d%03d", (f == "gw") ? 0 : 1, q[1], q[2], q[3], q[4])
    printf "%s\t%s\t%s\t%s\t%s\n", k, ip, mac, ven, f
    ip = ""
}
/^Nmap scan report for / { flush_host(); ip = $NF; gsub(/[()]/, "", ip); mac = ""; ven = ""; next }
/^MAC Address: / {
    mac = $3; ven = $0
    sub(/^[^(]*\(/, "", ven); sub(/\) *$/, "", ven)
    if (ven == "Unknown") ven = ""
}
END { flush_host() }')SH";

// Port-scan filter for Fast/Full: turns `nmap -v` output into a HOST INFO
// style log. Open ports stream as nmap discovers them ("+ 80/tcp open"),
// --stats-every lines condense to "progress N%", heartbeat ticks (no-PTY
// fallback) become "still scanning... Ns elapsed", nmap's PORT table (with
// service names) is kept, warnings pass through, and the rest is dropped -
// but the last 20 dropped lines are replayed if nmap fails, so errors (which
// share the PTY with stdout) are never hidden. nmap's exit status arrives as
// a "__nmap_rc=N" marker and becomes awk's (= the script's) exit status.
// CRs from the PTY are stripped. fflush() after every print, plus
// `-W interactive` on mawk (which otherwise block-buffers its *input* from a
// pipe), keeps the log live.
constexpr const char* kNmapPortsAwk = R"SH('
{ sub(/\r$/, "") }
/^__nmap_rc=/ { rc = substr($0, 11) + 0; next }
/^__tick$/ { t += 15; print "still scanning... " t "s elapsed"; fflush(); next }
/^Discovered open port / {
    p = $4
    if (!(p in seen)) { seen[p] = 1; n++; print "+ " p " open"; fflush() }
    next
}
/Timing: About [0-9.]+% done/ {
    s = $0; sub(/.*About /, "", s); sub(/% done.*/, "", s)
    e = ""
    if (match($0, /\([0-9:]+ remaining\)/)) e = " (" substr($0, RSTART + 1, RLENGTH - 12) " left)"
    print "progress " int(s) "%" e; fflush(); next
}
/^PORT +STATE +SERVICE/ { intable = 1; print; fflush(); next }
intable && /^[0-9]+\// { print; fflush(); next }
intable && /^ *$/ { intable = 0; next }
/^(WARNING|Warning|QUITTING)/ { print; fflush(); next }
/./ { keep[nk++ % 20] = $0 }
END {
    if (rc != 0) {
        for (i = (nk > 20 ? nk - 20 : 0); i < nk; i++) print keep[i % 20]
        print "nmap failed (exit " rc ")"; exit rc
    }
    if (n == 0) print "No open ports found"
    else print "Done: " n " open port(s)"
}')SH";

// Fast/Full scan script (non-root: nmap falls back to a TCP connect scan, like
// Bruce's HostInfo). Host details arrive as positional parameters
// ($1 ip, $2 mac, $3 vendor, $4 tag) - never spliced into the script text;
// $ip is re-checked to be digits/dots before it is used in a command line.
//
// Progress: nmap only emits --stats-every output when stdout is a TTY (a pipe
// gets nothing, and -oX has no <taskprogress> either), so nmap runs under
// util-linux script(1) (bsdutils: always present on Debian / Raspberry Pi
// OS), which gives it a PTY and passes its exit status back (-e). Without it
// (e.g. a busybox-only system) nmap runs directly and a 15s heartbeat stands
// in for the percentage (fd 3 is closed for it, so a pending `sleep` never
// holds the log pipe open after nmap ends). No temp files either way
// (typescript -> /dev/null).
// Stop kills the whole process group (ProcessRunner setsid()s): sh, script,
// awk and the heartbeat die directly; nmap, in script's PTY session, gets
// SIGHUP when script closes the PTY master.
std::string nmap_port_scan_script(const std::string& extra) {
    return std::string(
               "ip=$1; mac=$2; ven=$3; tag=$4; "
               "case \"$ip\" in ''|*[!0-9.]*) echo \"invalid host: $ip\"; exit 2;; esac; ") +
           kNmapMissingSh +
           "echo 'HOST INFO'; echo \"IP: $ip${tag:+ $tag}\"; "
           "if [ -n \"$mac\" ]; then echo \"MAC: $mac  Vendor: ${ven:-unknown}\"; "
           "else echo 'MAC: (not available)'; fi; "
           "echo 'Scanning ports... wait'; "
           "A=awk; awk -W version 2>/dev/null | grep -q mawk && A='awk -W interactive'; "
           "c=\"nmap -Pn -n -v " + extra + " $ip\"; "
           "{ if script -V 2>/dev/null | grep -q util-linux; then "
           "SHELL=/bin/sh script -qefc \"$c\" /dev/null </dev/null 2>&3; "
           "else "
           "( while sleep 15 </dev/null >/dev/null 2>&1; do echo __tick; done ) 3>&- & hb=$!; "
           "$c </dev/null 2>&3; r=$?; kill \"$hb\" 2>/dev/null; (exit \"$r\"); "
           "fi; echo \"__nmap_rc=$?\"; } 3>&2 | $A " +
           kNmapPortsAwk;
}

struct NmapHost {
    std::string ip, mac, vendor, flag; // flag: "gw", "self" or ""
};

bool valid_ipv4(const std::string& s) {
    int parts = 0;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = i;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
        if (j == i || j - i > 3 || std::stoi(s.substr(i, j - i)) > 255) return false;
        ++parts;
        if (j == s.size()) break;
        if (s[j] != '.') return false;
        i = j + 1;
    }
    return parts == 4;
}

bool valid_mac(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i % 3 == 2 ? c != ':' : !std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// Parse one `hosts` line ("ip\tmac\tvendor\tflag"; a bare IP from older builds
// also parses). The file is root-written, so every field is validated or
// sanitized before it reaches a label or argv; invalid IP => rejected.
bool parse_nmap_host(const std::string& line, NmapHost& out) {
    std::vector<std::string> f;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        f.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    f.resize(4);
    if (!valid_ipv4(f[0])) return false;
    out.ip  = f[0];
    out.mac = valid_mac(f[1]) ? f[1] : "";
    out.vendor.clear();
    for (const char c : f[2]) {
        if (std::isalnum(static_cast<unsigned char>(c)) || std::strchr(" .,&+-/()_", c)) out.vendor += c;
    }
    if (out.vendor.size() > 40) out.vendor.resize(40);
    out.flag = (f[3] == "gw" || f[3] == "self") ? f[3] : "";
    return true;
}

std::string serialize_nmap_host(const NmapHost& h) {
    return h.ip + "\t" + h.mac + "\t" + h.vendor + "\t" + h.flag;
}

std::vector<NmapHost> read_nmap_hosts(const std::string& dir) {
    std::vector<NmapHost> out;
    for (const auto& line : read_lines(dir + "/hosts")) {
        NmapHost h;
        if (parse_nmap_host(line, h)) out.push_back(std::move(h));
    }
    return out;
}

// The picked host, or false when nothing (valid) is selected.
bool read_nmap_selected(const std::string& dir, NmapHost& out) {
    return parse_nmap_host(read_first_line(dir + "/selected"), out);
}

std::string nmap_host_tag(const NmapHost& h) {
    return h.flag == "gw" ? "(GTW)" : (h.flag == "self" ? "(this device)" : "");
}

// Vendor shortened for the 320px list: whole words up to ~14 chars.
std::string short_vendor(const std::string& v) {
    constexpr size_t kMax = 14;
    if (v.size() <= kMax) return v;
    const size_t cut = v.rfind(' ', kMax);
    return v.substr(0, cut == std::string::npos || cut == 0 ? kMax : cut);
}

std::string nmap_host_label(const NmapHost& h) {
    std::string s = h.ip;
    const std::string tag = h.flag == "gw" ? "(GTW)" : (h.flag == "self" ? "(me)" : "");
    if (!tag.empty()) s += " " + tag;
    if (!h.vendor.empty()) s += " " + short_vendor(h.vendor);
    return s;
}

std::vector<ToolAction> build_nmap_actions() {
    // Owns the dynamically built label strings (ToolAction::label is const char*).
    // Cleared each rebuild; safe because screens copy label text when they build,
    // and no rebuild happens while a ToolRunScreen is live (see menu_screen.cpp).
    static std::deque<std::string> pool;
    pool.clear();
    auto own = [](std::string s) -> const char* {
        pool.push_back(std::move(s));
        return pool.back().c_str();
    };

    const std::string dir = nmap_dir();
    std::vector<ToolAction> acts;

    const std::string problem = ensure_nmap_dir();
    if (!problem.empty()) {
        // Never hand an unvetted dir to root-run rows: a single non-root row
        // that only explains the problem. Strip quotes so the message can be
        // single-quoted safely.
        std::string msg = "unsafe nmap state dir " + dir + ": " + problem;
        msg.erase(std::remove(msg.begin(), msg.end(), '\''), msg.end());
        ToolAction err;
        err.label       = "(Nmap unavailable: unsafe state dir)";
        err.description = own(msg + ". Remove it and re-open Nmap.");
        err.argv        = {"sh", "-c", "echo '" + msg + "'; exit 1"};
        acts.push_back(std::move(err));
        return acts;
    }

    NmapHost sel;
    const bool have_sel = read_nmap_selected(dir, sel);
    const std::string uid = std::to_string(getuid());

    if (!have_sel) {
        // Step 1: discover hosts on the local subnet. Runs as root (nmap -sn
        // uses ARP/ICMP only as root). The app's uid is baked in as $o. The
        // script never creates the dir (ensure_nmap_dir, run non-root, is the
        // only creator - a root mkdir could race a planted dir); it re-vets it
        // (real dir, owner $o, no group/other bits) before writing, and chowns
        // (-h, never following links) its results back to $o when it runs as
        // someone else, so the app can read them later.
        ToolAction a;
        a.label       = "1. Scan / refresh hosts";
        a.description = "Ping/ARP-sweep the local subnet (needs root): IP, MAC, vendor, gateway. Then go Back and pick a host.";
        a.argv        = {"sh", "-c",
             "d='" + dir + "'; o=" + uid + "; " + std::string(kNmapMissingSh) +
             "[ -e \"$d\" ] || [ -L \"$d\" ] || "
             "{ echo 'nmap state dir missing; reopen the Nmap menu'; exit 1; }; "
             "[ -d \"$d\" ] && [ ! -L \"$d\" ] && [ \"$(stat -c %u \"$d\")\" = \"$o\" ] && "
             "case \"$(stat -c %a \"$d\")\" in *00) true;; *) false;; esac || "
             "{ echo \"unsafe nmap state dir $d\"; exit 1; }; "
             "rm -f \"$d/selected\" \"$d/hosts\" \"$d/scan.nmap\"; " +
             std::string(kNmapSubnetSh) +
             "if [ -z \"$net\" ]; then echo 'no network (connect Wi-Fi first)'; exit 1; fi; "
             "me=${net%/*}; "
             "gw=$(ip -o route show default 2>/dev/null | awk '{for(i=1;i<NF;i++) if($i==\"via\"){print $(i+1); exit}}'); "
             "echo \"Discovering hosts on $net${dev:+ ($dev)} ...\"; "
             "nmap -sn -n \"$net\" -oN \"$d/scan.nmap\" 2>&1; rc=$?; "
             "{ [ -f \"$d/scan.nmap\" ] && awk -v gw=\"$gw\" -v me=\"$me\" " + std::string(kNmapHostsAwk) +
             " \"$d/scan.nmap\" | LC_ALL=C sort | cut -f2- > \"$d/hosts.tmp\" && mv -f \"$d/hosts.tmp\" \"$d/hosts\"; } "
             "|| rm -f \"$d/hosts.tmp\"; "
             "[ \"$(id -u)\" != \"$o\" ] && chown -h \"$o\" \"$d/hosts\" \"$d/scan.nmap\" 2>/dev/null; "
             "if [ \"$rc\" -ne 0 ]; then echo \"nmap failed (exit $rc)\"; exit \"$rc\"; fi; "
             "n=$(grep -c . \"$d/hosts\" 2>/dev/null); echo; "
             "if [ \"${n:-0}\" -eq 0 ]; then echo 'No hosts found'; else "
             "echo \"Done: $n host(s) found${gw:+ (gateway $gw)}. Go Back, pick a host, then choose Fast or Full.\"; fi"};
        a.needs_root  = true;
        acts.push_back(std::move(a));

        // Step 2: one navigation row per discovered host (empty argv), labelled
        // like Bruce's ARP scanner list: "192.168.3.1 (GTW) TP-LINK".
        const auto hosts = read_nmap_hosts(dir);
        if (hosts.empty()) {
            acts.push_back(ToolAction{
                "(no hosts yet - run Scan above)",
                "Run 'Scan / refresh hosts' first; discovered hosts appear here.",
                {"sh", "-c", "echo 'Run Scan / refresh hosts first.'"}});
        } else {
            for (const auto& h : hosts) {
                std::string desc = "Pick " + h.ip;
                if (!h.mac.empty()) desc += " (" + h.mac + (h.vendor.empty() ? "" : ", " + h.vendor) + ")";
                acts.push_back(ToolAction{
                    own(nmap_host_label(h)),
                    own(desc + ", then choose Fast or Full port scan."),
                    {}}); // empty argv => navigation row
            }
        }
    } else {
        // Step 3: a host is selected - offer Fast / Full, plus a way back.
        // Both run non-root (TCP connect scan, no password prompt on store
        // installs without the sudoers rule) and print a HOST INFO block.
        acts.push_back(ToolAction{
            "< Back to host list",
            "Clear the selected host and choose another.",
            {}}); // navigation row
        const std::string tag = nmap_host_tag(sel);
        ToolAction fast;
        fast.label       = own("Fast scan " + sel.ip + "  (top 100 ports)");
        fast.description = "nmap -Pn -F -T4 (TCP connect, no root): the most common ports, shown as they are found.";
        fast.argv        = {"sh", "-c", nmap_port_scan_script("-F -T4"),
                            "nmap-fast", sel.ip, sel.mac, sel.vendor, tag};
        acts.push_back(std::move(fast));
        ToolAction full;
        full.label       = own("Full scan " + sel.ip + "  (all 65535 ports)");
        full.description = "nmap -Pn -p- -T4 (TCP connect, no root): every TCP port - slower; progress every 15s.";
        full.argv        = {"sh", "-c", nmap_port_scan_script("-p- -T4 --stats-every 15s"),
                            "nmap-full", sel.ip, sel.mac, sel.vendor, tag};
        acts.push_back(std::move(full));
    }

    return acts;
}

std::vector<ToolAction> build_theme_actions() {
    // Names are string literals (static lifetime), so no owning pool is needed.
    // Empty argv marks each row as a navigation row: selecting it applies the
    // accent via the view layer instead of launching a tool (see menu_screen).
    std::vector<ToolAction> acts;
    for (const auto& preset : accent_presets()) {
        acts.push_back(ToolAction{preset.name, "Set the UI accent color (applies instantly).", {}});
    }
    return acts;
}

// --- Wi-Fi category ---------------------------------------------------------
//
// A fixed prefix (recon, monitor mode, attacks, radio toggles) plus the Evil
// Portal block (native --evil-portal subcommand) and the clone-AP drill-down,
// rebuilt on every menu entry like Nmap's host list.

// Interns an interface-bearing label/description so a ToolAction/ToolCategory
// `const char*` field can be built at runtime from wifi_interface(). The pool
// is persistent (never cleared) and deduplicated, so the handful of distinct
// strings get stable addresses and repeated rebuilds do not grow it - and
// build_wifi_static_actions().size() (called just for a count in
// on_navigation_action) can never dangle pointers held by the live catalog.
const char* intern_iface_text(const std::string& s) {
    static std::deque<std::string> pool; // deque: push_back keeps element addresses stable
    for (const auto& e : pool) {
        if (e == s) {
            return e.c_str();
        }
    }
    pool.push_back(s);
    return pool.back().c_str();
}

std::vector<ToolAction> build_wifi_static_actions() {
    const std::string w = wifi_interface(); // real NIC, not a hardcoded "wlan0"
    const std::string mon = w + "mon";      // airmon-ng's monitor VIF name (<iface>mon)
    return {
        // --- recon ---
        // needs_root rows are escalated by model::launch_argv (sudo -n <self>
        // --root-sh <script>); no in-script `id -u` guard needed.
        // wifi_view rows open the structured WifiScanScreen; the argv is the
        // scan command it runs. auto_run makes the recon rows scan on entry.
        {"Scan nearby networks", intern_iface_text("iw dev " + w + " scan (needs root)"),
         {"sh", "-c", "ip link set " + w + " up 2>/dev/null; iw dev " + w + " scan"},
         true, false, false, true, RfTool::None, WifiView::ScanList},
        {"List SSIDs + signal", "iw scan | ssid/signal/freq",
         {"sh", "-c", "ip link set " + w + " up 2>/dev/null; iw dev " + w + " scan"},
         true, false, false, true, RfTool::None, WifiView::SsidList},
        {"Channel usage (APs/chan)", "scan grouped by channel",
         {"sh", "-c",
          "iw dev " + w + " scan 2>/dev/null | awk '/freq:/{print $2}' | sort -n | uniq -c | "
          "awk '{printf \"  %s MHz: %s APs\\n\", $2, $1}' || echo 'scan failed'"},
         true, false, false, true},
        {"Show Wi-Fi status", intern_iface_text("ip addr show " + w), {"ip", "addr", "show", w},
         false, false, false, true},
        {"Scan LAN hosts (arp-scan)", "arp-scan --localnet",
         {"sh", "-c", "arp-scan --localnet 2>&1 || echo 'needs arp-scan (apt install arp-scan)'"},
         true, false, false, true},
        {"Scan LAN hosts (nmap)", "nmap -sn <subnet> (needs root)",
         {"sh", "-c",
          std::string(kNmapMissingSh) + kNmapSubnetSh +
          "if [ -z \"$net\" ]; then echo 'no network (connect Wi-Fi first)'; exit 1; fi; "
          "echo \"Scanning $net${dev:+ ($dev)} ...\"; "
          "nmap -sn -n \"$net\" 2>&1"},
         true, false, false, true},

        // --- monitor mode (prerequisite for the attacks below) ---
        // airmon-ng frequently produces no wlan0mon on this Wi-Fi chip, so fall
        // back to a manual monitor VIF (mon0) and report what to attack with.
        {"Monitor mode ON (airmon-ng)", "airmon-ng; fallback: manual mon0 VIF",
         {"sh", "-c",
          "airmon-ng check kill 2>&1; airmon-ng start " + w + " 2>&1; "
          "if ! ip link show " + mon + " >/dev/null 2>&1; then "
          "echo; echo 'airmon-ng produced no " + mon + " - creating mon0:'; "
          "iw dev " + w + " interface add mon0 type monitor 2>&1 && ip link set mon0 up; fi; "
          "echo; mon=; for d in " + mon + " mon0; do ip link show $d >/dev/null 2>&1 && mon=$d && break; done; "
          "[ -n \"$mon\" ] && echo \"monitor interface: $mon (airodump/mdk4 find it automatically)\" || "
          "echo 'no monitor interface - this radio does not support monitor mode'"},
         true},
        {"Monitor mode OFF", "remove monitor ifaces; restart NetworkManager",
         {"sh", "-c",
          "for d in " + mon + " mon0; do ip link del $d 2>/dev/null && echo \"removed $d\"; done; "
          "airmon-ng stop " + mon + " 2>/dev/null; rfkill unblock wifi; "
          "systemctl restart NetworkManager 2>/dev/null; echo done"},
         true},
        // Consumers pick whichever monitor VIF exists (wlan0mon from airmon-ng
        // or mon0 from the manual fallback) instead of hardcoding wlan0mon.
        {"Airodump scan (APs+clients)", "airodump-ng <monitor if>",
         {"sh", "-c",
          "mon=; for d in " + mon + " mon0; do [ -e /sys/class/net/$d ] && mon=$d && break; done; "
          "[ -n \"$mon\" ] || { echo 'enable Monitor mode first'; exit 1; }; airodump-ng \"$mon\" 2>&1"}},
        {"Capture handshakes -> /tmp", "airodump-ng -w /tmp/bruce-wifi <mon>",
         {"sh", "-c",
          "mon=; for d in " + mon + " mon0; do [ -e /sys/class/net/$d ] && mon=$d && break; done; "
          "[ -n \"$mon\" ] || { echo 'enable Monitor mode first'; exit 1; }; "
          "airodump-ng -w /tmp/bruce-wifi \"$mon\" 2>&1"}},

        // --- attacks (authorized testing only) ---
        {"Deauth flood (mdk4)", "mdk4 <monitor if> d",
         {"sh", "-c",
          "mon=; for d in " + mon + " mon0; do [ -e /sys/class/net/$d ] && mon=$d && break; done; "
          "[ -n \"$mon\" ] || { echo 'needs Monitor mode + mdk4'; exit 1; }; "
          "mdk4 \"$mon\" d 2>&1 || echo 'needs mdk4'"}},
        {"Beacon spam (mdk4)", "mdk4 <monitor if> b -s 100",
         {"sh", "-c",
          "mon=; for d in " + mon + " mon0; do [ -e /sys/class/net/$d ] && mon=$d && break; done; "
          "[ -n \"$mon\" ] || { echo 'needs Monitor mode + mdk4'; exit 1; }; "
          "mdk4 \"$mon\" b -s 100 2>&1 || echo 'needs mdk4'"}},
        // hostapd needs driver=nl80211 (it refuses to start without it) and
        // clients need DHCP+DNS to land anywhere: dnsmasq answers every name
        // with the AP. Both run in this script's process group, so the tool
        // Stop button kills them together.
        {"Rogue open AP (hostapd)", intern_iface_text("open AP 'Free WiFi' + DHCP/DNS captive on " + w),
         {"sh", "-c",
          "command -v hostapd >/dev/null 2>&1 || "
          "{ echo 'needs hostapd + dnsmasq (apt install hostapd dnsmasq)'; exit 1; }; "
          "cfg=$(mktemp); printf 'driver=nl80211\\ninterface=" + w + "\\nssid=Free WiFi\\nchannel=6\\n"
          "hw_mode=g\\nauth_algs=1\\n' > \"$cfg\"; "
          "ip addr replace 192.168.4.1/24 dev " + w + " 2>/dev/null; ip link set " + w + " up; "
          "dns=$(mktemp); printf 'interface=" + w + "\\nbind-interfaces\\nexcept-interface=lo\\nno-resolv\\n"
          "dhcp-authoritative\\ndhcp-range=192.168.4.10,192.168.4.50,255.255.255.0,12h\\n"
          "dhcp-option=3,192.168.4.1\\ndhcp-option=6,192.168.4.1\\naddress=/#/192.168.4.1\\n' > \"$dns\"; "
          "echo 'open AP \"Free WiFi\" @ 192.168.4.1 (captive: every name resolves here)'; "
          "hostapd \"$cfg\" & dnsmasq -C \"$dns\" -d & wait"},
         true},

        // --- evil portal (Linux port of the ESP32 Bruce feature) ---
        {"Evil Portal: Default (Google)",
         "Open AP + DNS-hijacked captive portal capturing credentials to "
         "~/.bruce/BruceEvilCreds (Bruce default page). Needs root + hostapd/dnsmasq.",
         {self_exe_path(), "--evil-portal", "default"}},
        {"Evil Portal: Router Update (verify pwd)",
         "'Router firmware update' portal; verifies each stolen password against "
         "the real network (AP pauses seconds per check). Needs root + hostapd/dnsmasq.",
         {self_exe_path(), "--evil-portal", "verify"}},
        {"Evil Portal: Custom HTML",
         "Serves ~/.bruce/evil_portal/index.html; a first line <!-- AP=\"Name\" --> "
         "names the AP, captures go to <name>_creds.csv (Bruce Custom Html).",
         {self_exe_path(), "--evil-portal", "custom", "~/.bruce/evil_portal/index.html"}},

        // --- radio toggles ---
        {"Enable Wi-Fi radio", "rfkill unblock wifi", {"rfkill", "unblock", "wifi"}},
        {"Disable Wi-Fi radio", "rfkill block wifi", {"rfkill", "block", "wifi"}},
    };
}

// Evil Portal clone-AP drill-down (Bruce's "Clone Portal"): scan -> pick an AP
// -> the portal clones its SSID and channel. Rows carry an empty argv so the
// menu treats them as navigation (see on_navigation_action).
std::vector<ToolAction> build_portal_rows() {
    // Owns the dynamically built label strings (ToolAction::label is const
    // char*). Cleared each rebuild; no rebuild happens while a ToolRunScreen
    // is live (see menu_screen.cpp).
    static std::deque<std::string> pool;
    pool.clear();
    auto own = [](std::string s) -> const char* {
        pool.push_back(std::move(s));
        return pool.back().c_str();
    };

    const std::string w = wifi_interface();
    const std::string dir = portal_dir();
    const std::string selected = read_first_line(dir + "/selected");
    std::vector<ToolAction> acts;

    if (selected.empty()) {
        acts.push_back(ToolAction{
            "Evil Portal: Clone an AP  (1. scan first)",
            "Scan nearby APs, then go Back and pick one to clone. Needs root.",
            {"sh", "-c",
             "mkdir -p " + dir + "; rm -f " + dir + "/selected; "
             "ip link set " + w + " up 2>/dev/null; "
             "iw dev " + w + " scan 2>/dev/null | awk "
             "'function flush(){ if (ss!=\"\") printf \"%d\\t%s\\n\", ch, ss } "
             "/^BSS /{flush(); ss=\"\"; ch=1; next} "
             "$1==\"freq:\"{ch=($2>=5000)?int(($2-5000)/5):int(($2-2407)/5); if(ch<1)ch=1; next} "
             "$1==\"SSID:\"{ss=substr($0,index($0,\":\")+1); sub(/^ +/,\"\",ss); next} "
             "END{flush()}' | sort -u | tee " + dir + "/targets; "
             "echo; echo 'Done. Go Back and pick an AP to clone.'"},
            true});

        const auto targets = read_lines(dir + "/targets");
        if (targets.empty()) {
            acts.push_back(ToolAction{
                "(no APs yet - run the scan above)",
                "Run the scan on the row above; discovered APs appear here.",
                {"sh", "-c", "echo 'Run the scan first.'"}});
        } else {
            for (const auto& t : targets) {
                const auto [chan, ssid] = split_target(t);
                acts.push_back(ToolAction{
                    own("Clone \"" + ssid + "\"  (ch " + chan + ")  >"),
                    "Pick this AP: the Evil Portal clones its SSID and channel.",
                    {}}); // empty argv => navigation row
            }
        }
    } else {
        const auto [chan, ssid] = split_target(selected);
        acts.push_back(ToolAction{
            "< Back to AP list",
            "Clear the picked AP and clone another.",
            {}}); // navigation row
        acts.push_back(ToolAction{
            own("Evil Portal: Clone \"" + ssid + "\"  (ch " + chan + ")"),
            "Starts the cloned AP + captive portal; captures land in "
            "~/.bruce/BruceEvilCreds/default_creds.csv.",
            {self_exe_path(), "--evil-portal", "clone", ssid, chan}});
    }
    return acts;
}

std::vector<ToolAction> build_wifi_actions() {
    std::vector<ToolAction> acts = build_wifi_static_actions();
    const std::vector<ToolAction> portal = build_portal_rows();
    acts.insert(acts.end(), portal.begin(), portal.end());
    return acts;
}

} // namespace

const std::vector<AccentPreset>& accent_presets() {
    static const std::vector<AccentPreset> presets = {
        {"Purple (Bruce)", 0xB026FF, 0xD48BFF},
        {"Violet",         0x7C4DFF, 0xB39DFF},
        {"Blue",           0x2B6BFF, 0x7AA6FF},
        {"Cyan",           0x00C8D4, 0x6FF0F7},
        {"Teal",           0x1BC4A6, 0x79E8D4},
        {"Green",          0x24D17A, 0x7FF0B5},
        {"Lime",           0x9CCC00, 0xCDF06A},
        {"Yellow",         0xF2C200, 0xFFE066},
        {"Orange",         0xFF8A1F, 0xFFB366},
        {"Red",            0xFF3B30, 0xFF8A82},
        {"Pink",           0xFF3EA5, 0xFF9FD0},
        {"White",          0xD7D7E4, 0xFFFFFF},
    };
    return presets;
}

// Phosphor-Fill glyph codepoints (official @phosphor-icons "Fill" map, verified
// against the bundled font). Kept here as data so the model owns nothing from
// the view layer; the menu carousel just renders category.icon.
namespace icon {
constexpr const char* kWifi      = ""; // wifi-high
constexpr const char* kBluetooth = ""; // bluetooth
constexpr const char* kRf        = ""; // broadcast
constexpr const char* kNfc       = "\U0000EBB6"; // scan (ST25R3916 NFC/RFID)
constexpr const char* kScan      = "\U0000EBE0"; // list-magnifying-glass (nmap)
constexpr const char* kIr        = "\U0000E754"; // television (infrared)
constexpr const char* kGps       = "\U0000E316"; // map-pin (GPS)
constexpr const char* kEth       = "\U0000EB5A"; // plugs-connected (ethernet)
constexpr const char* kFiles     = "\U0000E260"; // folders (files)
constexpr const char* kClock     = "\U0000E19A"; // clock (system/clock)
constexpr const char* kPalette   = "\U0000E6C8"; // palette (theme colors)
constexpr const char* kNetwork   = ""; // network
constexpr const char* kStorage   = ""; // hard-drives
constexpr const char* kShield    = ""; // shield-warning
constexpr const char* kInfo      = ""; // info
constexpr const char* kI2c       = ""; // circuitry
constexpr const char* kSpi       = ""; // memory
constexpr const char* kUart      = ""; // terminal-window
constexpr const char* kOneWire   = ""; // git-commit
constexpr const char* kGpio      = ""; // cpu
} // namespace icon

// Common IANA timezones offered as a picker under "System / Clock". Selecting
// one sets the system zone (timedatectl, or the zoneinfo symlink fallback) so
// the clock shows the right local time; NTP only fixes UTC, not the zone.
std::vector<ToolAction> build_timezone_actions() {
    static const char* const kZones[] = {
        "UTC",               "Europe/London",    "Europe/Lisbon",
        "Europe/Madrid",     "Europe/Paris",     "Europe/Berlin",
        "Europe/Athens",     "Europe/Moscow",    "America/New_York",
        "America/Chicago",   "America/Denver",   "America/Los_Angeles",
        "America/Sao_Paulo", "Asia/Dubai",       "Asia/Kolkata",
        "Asia/Shanghai",     "Asia/Tokyo",       "Australia/Sydney",
        "Pacific/Auckland",
    };
    // ToolAction::label is a const char*; dynamically built labels live in a
    // static pool, cleared each rebuild (safe: a live ToolRunScreen has already
    // copied the label text, and no rebuild happens while it is open).
    static std::deque<std::string> pool;
    pool.clear();
    auto own = [](std::string s) -> const char* {
        pool.push_back(std::move(s));
        return pool.back().c_str();
    };
    std::vector<ToolAction> acts;
    acts.reserve(sizeof(kZones) / sizeof(kZones[0]));
    for (const char* z : kZones) {
        const std::string zone = z;
        ToolAction a;
        a.label       = own("Timezone: " + zone);
        a.description = own("Set the system timezone to " + zone +
                            " (needs root); the clock then shows this zone's local time.");
        a.argv        = {"sh", "-c",
                         "tz='" + zone + "'; "
                         "timedatectl set-timezone \"$tz\" 2>/dev/null || "
                         "ln -sf \"/usr/share/zoneinfo/$tz\" /etc/localtime 2>&1; "
                         "echo \"timezone set to $tz\"; date"};
        a.needs_root  = true;
        acts.push_back(std::move(a));
    }
    return acts;
}
// System / Clock utilities plus a single "Set timezone >" entry that drills
// into the timezone picker in place (Nmap-style), so the long zone list never
// clutters the category. tz_picker_open() is the in-memory toggle, flipped by
// on_navigation_action() and read here on every rebuild.
bool& tz_picker_open() {
    static bool open = false;
    return open;
}

std::vector<ToolAction> build_system_clock_actions() {
    if (tz_picker_open()) {
        // Dedicated picker view: a Back row, then the runnable zone rows.
        std::vector<ToolAction> picker;
        picker.push_back(ToolAction{"<  Back", "Return to System / Clock.", {}});
        for (auto& z : build_timezone_actions()) {
            picker.push_back(std::move(z));
        }
        return picker;
    }
    return {
        {"Clock", "A full-screen themed clock \xe2\x80\x94 just the time and date",
         {"sh", "-c", "while true; do printf '\\r%s' \"$(date '+%Y-%m-%d %H:%M:%S')\"; sleep 1; done"},
         false, /*clock_view=*/true},
        {"Sync time (NTP)",
         "Set the system clock from the internet (needs root). Tries "
         "ntpdate/sntp/chrony, else enables systemd-timesyncd.",
         {"sh", "-c",
          "ok=0; "
          "for c in 'ntpdate -u pool.ntp.org' 'sntp -s pool.ntp.org' 'chronyc -a makestep'; do "
          "bin=${c%% *}; command -v \"$bin\" >/dev/null 2>&1 || continue; "
          "echo \"+ $c\"; $c 2>&1 && ok=1 && break; done; "
          "if [ \"$ok\" = 0 ] && command -v timedatectl >/dev/null 2>&1; then "
          "echo '+ timedatectl set-ntp true'; timedatectl set-ntp true 2>&1 && ok=1; "
          "echo '(systemd-timesyncd syncs in the background; give it ~1 min)'; fi; "
          "[ \"$ok\" = 0 ] && echo 'no NTP client; install one: sudo apt install ntpdate (or sntp/chrony)'; "
          "echo; echo \"now: $(date)\""},
         /*needs_root=*/true},
        {"Set timezone  >",
         "Pick your timezone so the clock shows the right local time.",
         {}}, // empty argv => navigation row that opens the picker
        {"Set time manually",
         "Opens the terminal; type  sudo date -s \"YYYY-MM-DD HH:MM:SS\"  "
         "to set the clock by hand (quote the whole date).",
         {"sh", "-c", "echo 'set the clock with: sudo date -s \"YYYY-MM-DD HH:MM:SS\"'"},
         false, false, /*terminal_view=*/true},
        {"Countdown timer (60s)", "sleep 60; beep",
         {"sh", "-c", "for i in $(seq 60 -1 1); do printf '\\r%2ds remaining ' \"$i\"; sleep 1; done; "
          "echo; echo 'Time up!'"}},
        {"Uptime + load", "uptime", {"uptime"}},
        {"Install optional tools (apt)",
         "apt-get install nmap + the Suggests set (aircrack-ng, mdk4, hostapd, arp-scan, "
         "dnsmasq, ...). Needs root: passwordless with the sudoers rule, else asks for the sudo password.",
         {"sh", "-c",
          "export DEBIAN_FRONTEND=noninteractive; "
          "apt-get update 2>&1; "
          "apt-get install -y nmap aircrack-ng mdk4 hostapd arp-scan dnsmasq "
          "v4l-utils lirc alsa-utils dsniff 2>&1; echo; echo 'done.'"},
         true},
        {"QR code (this device URL)", "qrencode -t UTF8",
         {"sh", "-c", "ip=$(hostname -I 2>/dev/null | awk '{print $1}'); "
          "qrencode -t UTF8 \"http://$ip:8080\" 2>&1 || echo 'needs qrencode'"}},
        {"Microphone record (5s)", "arecord -d 5 /tmp/rec.wav",
         {"sh", "-c", "arecord -d 5 -f cd /tmp/bruce-rec.wav 2>&1 && echo 'saved /tmp/bruce-rec.wav' || "
          "echo 'needs a mic + alsa-utils (arecord)'"}},
        {"iButton read (1-Wire ROM)", "cat /sys/bus/w1/devices/*/id",
         {"sh", "-c", "for d in /sys/bus/w1/devices/*/; do n=$(basename \"$d\"); "
          "[ \"$n\" != w1_bus_master1 ] && echo \"iButton/1-Wire: $n\"; done 2>/dev/null || "
          "echo 'no 1-Wire device (needs w1-gpio overlay + an iButton)'"}},
    };
}

// Fallback raw-log script for the BLE Scan row. The row normally opens
// BleScanScreen, which runs bluetoothctl without this awk filter so it can
// parse device names/RSSI. bluetoothctl is the supported D-Bus client (it
// coexists with bluetoothd, unlike hcitool) and stops discovery on exit
// (unlike btmgmt); this pipeline is only the log fallback if that screen is
// ever dropped. It strips BlueZ's ANSI/readline markers and collapses the
// RSSI/property flood to one entry per device.
constexpr const char* kBluetoothLeScanScript = R"BLE(timeout 8 bluetoothctl --timeout 5 scan le 2>&1 |
sed -u -e 's/\x1b\[[0-9;]*m//g' -e 's/[\x01\x02]//g' |
awk '
    $0 == "SetDiscoveryFilter success" || $0 == "Discovery started" { print; fflush(); next }
    $1 == "[CHG]" && $2 == "Controller" && /Discovering:/ { print; fflush(); next }
    $1 == "[NEW]" && $2 == "Device" {
        mac = $3
        if (!(mac in seen)) { seen[mac] = 1; want_rssi[mac] = 1; print; fflush() }
        next
    }
    $1 == "[CHG]" && $2 == "Device" && /RSSI:/ {
        mac = $3
        if (!(mac in seen)) { seen[mac] = 1; print; fflush() }
        else if (want_rssi[mac] && !got_rssi[mac]) { got_rssi[mac] = 1; print; fflush() }
        next
    }
    $1 == "[CHG]" && $2 == "Device" {
        mac = $3
        if (!(mac in seen)) { seen[mac] = 1; print; fflush() }
        next
    }
    $0 !~ /^[[:space:]]/ && $0 !~ /^\[(NEW|CHG|DEL)\]/ && $0 !~ /^hci[0-9]/ {
        print; fflush()
    }
')BLE";

namespace {

// Bruce firmware "GPS Tracker" (src/modules/gps/gps_tracker.cpp): reads the GPS
// UART in-app and logs a GPX track to ~/BruceGPS. Opens GpsAppScreen.
ToolAction gps_tracker_action() {
    ToolAction action{"GPS Tracker",
                      "Log your route as GPX to ~/BruceGPS (Bruce firmware GPS Tracker)",
                      {"sh", "-c", "echo 'GPS Tracker runs on its own screen: open it from the GPS menu.'"}};
    action.gps_app = GpsApp::Tracker;
    return action;
}

namespace gpscfg = model::gps_config;

// Which level of Bruce's GPS menu tree the category shows.
enum class GpsView { Root, Wardriving, Config, Baudrate, Port, PortPicker };

struct GpsMenuState {
    GpsView view = GpsView::Root;
    // GPS Port submenu (Bruce setUARTPinsMenu): the edited value and whether
    // it changed since the submenu opened ("Save Config" is enabled then).
    std::string port;
    bool changed = false;
    // Port picker rows, captured when the picker is built so the selected
    // row maps back to the same node (index 0 = Auto).
    std::vector<std::string> picker;
    // Error from the last navigation row (a failed save), for the screen's
    // error popup; taken (and cleared) by take_navigation_error().
    std::string error;
};

GpsMenuState& gps_state() {
    static GpsMenuState state;
    return state;
}

std::vector<std::string>& gps_port_candidates() {
    static std::vector<std::string> ports;
    return ports;
}

// Label storage for the dynamic GPS rows (ToolAction::label is const char*).
// Cleared on each rebuild; no rebuild happens while a screen holding a row is
// open (same contract as the timezone picker).
const char* gps_own(std::string s, bool reset = false) {
    static std::deque<std::string> pool;
    if (reset) {
        pool.clear();
        return nullptr;
    }
    pool.push_back(std::move(s));
    return pool.back().c_str();
}

ToolAction gps_nav(const char* label, const char* description) {
    return ToolAction{label, description, {}};
}

// The app user's home for ~/BruceWardriving: the wardriving child runs as
// root, so it gets the directory in argv and never looks at its own $HOME.
std::string app_user_home() {
    // The app itself started with plain `sudo` (HOME is then root's): the
    // invoking user's home, so the wardriving files are theirs. The child
    // checks the home's owner against $SUDO_UID anyway.
    if (::geteuid() == 0) {
        uint32_t id = 0;
        if (model::parse_id(std::getenv("SUDO_UID"), id)) {
            if (const passwd* pw = ::getpwuid(static_cast<uid_t>(id)); pw && pw->pw_dir) {
                return pw->pw_dir;
            }
        }
    }
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        return home;
    }
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) {
        return pw->pw_dir;
    }
    return "/tmp";
}

// GpsMenu::wardrivingMenu() rows: Bruce's Wardriving(scanWiFi, scanBLE) on
// GpsAppScreen, run as the root child `--wardrive <mode> --ui`. The
// configured port/baud (after $GPS_UART / $GPS_BAUD) and the user's home
// travel in argv, so the root child never reads the user's config or HOME.
ToolAction wardrive_action(const char* label, const char* mode, const char* description) {
    const auto eff = gpscfg::resolve(gpscfg::load());
    ToolAction action{label, description,
                      {self_exe_path(), "--wardrive", mode, "--ui", "--port", eff.port, "--baud",
                       std::to_string(eff.baud), "--home", app_user_home(), "--iface", wifi_interface()},
                      /*needs_root=*/true};
    action.gps_app = GpsApp::Wardriving;
    return action;
}

std::vector<ToolAction> build_gps_actions() {
    gps_own("", /*reset=*/true);
    GpsMenuState& st = gps_state();
    std::vector<ToolAction> acts;
    switch (st.view) {
        case GpsView::Root:
            // GpsMenu::optionsMenu(): Wardriving / GPS Tracker / Config + Main Menu.
            acts.push_back(gps_nav("Wardriving", "Scan Wi-Fi / BLE and log them with GPS positions"));
            acts.push_back(gps_tracker_action());
            acts.push_back(gps_nav("Config", "GPS baudrate and serial port"));
            acts.push_back(gps_nav("Main Menu", "Back to the main menu"));
            break;
        case GpsView::Wardriving:
            // GpsMenu::wardrivingMenu().
            acts.push_back(wardrive_action("Scan WiFi Networks", "wifi",
                                           "Log Wi-Fi networks with GPS positions to ~/BruceWardriving (WiGLE CSV)"));
            acts.push_back(wardrive_action("Scan BLE Devices", "ble",
                                           "Log BLE devices with GPS positions to ~/BruceWardriving (WiGLE CSV)"));
            acts.push_back(wardrive_action("Scan Both", "both",
                                           "Log Wi-Fi networks and BLE devices to ~/BruceWardriving (WiGLE CSV)"));
            acts.push_back(gps_nav("Back", "Back to the GPS menu"));
            break;
        case GpsView::Config:
            // GpsMenu::configMenu(): Baudrate / GPS Pins / Back.
            acts.push_back(gps_nav("Baudrate", "Serial speed of the GPS receiver"));
            acts.push_back(gps_nav("GPS Port", "Serial port of the GPS (HAT UART, USB GPS or Auto)"));
            acts.push_back(gps_nav("Back", "Back to the GPS menu"));
            break;
        case GpsView::Baudrate: {
            // setGpsBaudrateMenu(): the five values, the current one marked.
            const int current = gpscfg::load().baud;
            for (const int b : gpscfg::baud_rates()) {
                std::string label = std::to_string(b) + " bps";
                if (b == current) {
                    label += "  *";
                }
                acts.push_back(gps_nav(gps_own(label), "Set the GPS baudrate (saved at once)"));
            }
            break;
        }
        case GpsView::Port: {
            // setUARTPinsMenu(): "RX = n" / "TX = n" become one "Port = <node>".
            acts.push_back(gps_nav(gps_own("Port = " + st.port), "Pick the GPS serial port"));
            ToolAction save = gps_nav("Save Config", "Save the GPS port to ~/.bruce/gps.conf");
            save.dimmed = !st.changed;
            acts.push_back(std::move(save));
            acts.push_back(gps_nav("Main Menu", "Leave without saving"));
            break;
        }
        case GpsView::PortPicker: {
            st.picker.clear();
            st.picker.push_back(gpscfg::kAutoPort);
            for (const auto& p : gps_port_candidates()) {
                st.picker.push_back(p);
            }
            // Keep the configured node selectable even while it is unplugged.
            if (!gpscfg::is_auto(st.port) &&
                std::find(st.picker.begin(), st.picker.end(), st.port) == st.picker.end()) {
                st.picker.push_back(st.port);
            }
            for (const auto& p : st.picker) {
                const bool missing = !gpscfg::is_auto(p) &&
                    std::find(gps_port_candidates().begin(), gps_port_candidates().end(), p) ==
                        gps_port_candidates().end();
                acts.push_back(gps_nav(gps_own(missing ? p + " (not present)" : p),
                                       gpscfg::is_auto(p)
                                           ? "Probe the serial ports for NMEA when a GPS feature starts"
                                           : "Use this serial port for the GPS"));
            }
            break;
        }
    }
    return acts;
}

std::string gps_header() {
    switch (gps_state().view) {
        case GpsView::Root:
            // The baud the GPS features will really use: $GPS_BAUD, else the
            // saved value, else the board default.
            return "GPS (" + std::to_string(gpscfg::resolve(gpscfg::load()).baud) + " bps)";
        case GpsView::Wardriving:
            return "Wardriving";
        case GpsView::Config:
            return "GPS Config";
        case GpsView::Baudrate:
            return "Baudrate";
        case GpsView::Port:
            return "GPS Port";
        case GpsView::PortPicker:
            return "Select GPS Port";
    }
    return "GPS";
}

// False on failure, with the popup text left in gps_state().error.
bool save_gps_config(const gpscfg::GpsConfig& cfg) {
    std::string err;
    if (!gpscfg::save(cfg, &err)) {
        std::fprintf(stderr, "gps: cannot save %s: %s\n", gpscfg::config_path().c_str(), err.c_str());
        // The popup keeps only the reason ("Permission denied", "No space
        // left on device"): the full path is long and names the user's home.
        const size_t colon = err.rfind(": ");
        gps_state().error =
            "Cannot save GPS config: " + (colon == std::string::npos ? err : err.substr(colon + 2));
        return false;
    }
    return true;
}

// Returns true when the row leaves the category ("Main Menu").
bool gps_navigation(int item) {
    GpsMenuState& st = gps_state();
    switch (st.view) {
        case GpsView::Root:
            if (item == 0) {
                st.view = GpsView::Wardriving;
            } else if (item == 2) {
                st.view = GpsView::Config;
            } else if (item == 3) {
                return true; // addOptionToMainMenu()
            }
            return false;
        case GpsView::Wardriving:
            if (item == 3) {
                st.view = GpsView::Root;
            }
            return false;
        case GpsView::Config:
            if (item == 0) {
                st.view = GpsView::Baudrate;
            } else if (item == 1) {
                st.port = gpscfg::load().port;
                st.changed = false;
                st.view = GpsView::Port;
            } else if (item == 2) {
                st.view = GpsView::Root;
            }
            return false;
        case GpsView::Baudrate: {
            const auto& rates = gpscfg::baud_rates();
            if (item >= 0 && item < static_cast<int>(rates.size())) {
                auto cfg = gpscfg::load();
                cfg.baud = gpscfg::validate_baud(rates[static_cast<size_t>(item)]); // setGpsBaudrate() saves
                if (!save_gps_config(cfg)) {
                    return false; // stay on the list (error popup), pick again to retry
                }
                st.view = GpsView::Root; // the new "GPS (<baud> bps)" header shows at once
            }
            return false;
        }
        case GpsView::Port:
            if (item == 0) {
                st.view = GpsView::PortPicker;
            } else if (item == 1) {
                if (!st.changed) {
                    return false; // "Save Config" is disabled until something changed
                }
                auto cfg = gpscfg::load();
                cfg.port = st.port;
                if (!save_gps_config(cfg)) {
                    return false; // keep the edited port and "Save Config" enabled for a retry
                }
                st.changed = false;
                st.view = GpsView::Root;
            } else if (item == 2) {
                st.view = GpsView::Root;
                return true; // "Main Menu": discard
            }
            return false;
        case GpsView::PortPicker:
            if (item >= 0 && item < static_cast<int>(st.picker.size())) {
                st.port = st.picker[static_cast<size_t>(item)];
                st.changed = true; // Bruce marks the submenu changed on any pick
            }
            st.view = GpsView::Port;
            return false;
    }
    return false;
}

bool gps_back() {
    GpsMenuState& st = gps_state();
    switch (st.view) {
        case GpsView::Root:
            return false;
        case GpsView::Wardriving:
        case GpsView::Config:
            st.view = GpsView::Root;
            return true;
        case GpsView::Baudrate:
        case GpsView::Port:
            st.view = GpsView::Config;
            return true;
        case GpsView::PortPicker:
            st.view = GpsView::Port;
            return true;
    }
    return false;
}

int gps_preferred_selection() {
    const GpsMenuState& st = gps_state();
    if (st.view == GpsView::Baudrate) {
        const int current = gpscfg::load().baud;
        const auto& rates = gpscfg::baud_rates();
        for (size_t i = 0; i < rates.size(); ++i) {
            if (rates[i] == current) {
                return static_cast<int>(i);
            }
        }
    }
    if (st.view == GpsView::PortPicker) {
        for (size_t i = 0; i < st.picker.size(); ++i) {
            if (st.picker[i] == st.port) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

ToolAction ble_scan_action() {
    ToolAction action{"BLE Scan", "Scan nearby BLE devices (5s)",
                      {"sh", "-c", kBluetoothLeScanScript}, true};
    action.ble_scan_view = true;
    return action;
}

} // namespace

// Mutable backing store so data-driven categories (Nmap) can be rebuilt in
// place; tool_catalog() hands out a const view of it.
std::vector<ToolCategory>& mutable_catalog() {
    static std::vector<ToolCategory> catalog = {
        {
            "Wi-Fi",
            intern_iface_text("Scan, recon, 802.11 attacks and Evil Portal (" + wifi_interface() + "; root needed)"),
            icon::kWifi,
            false,
            build_wifi_actions(),
        },
        {
            "Bluetooth LE",
            "Discover and manage nearby BLE devices",
            icon::kBluetooth,
            false,
            {
                ble_scan_action(),
                {"Show adapter info", "bluetoothctl show", {"bluetoothctl", "show"}},
                {"BLE Spam: Apple pairing", "Broadcast Apple Continuity pairing popups",
                 {self_exe_path(), "--ble-spam", "apple"}},
                {"BLE Spam: Apple action", "Broadcast Apple NearbyAction setup modals",
                 {self_exe_path(), "--ble-spam", "apple-action"}},
                {"BLE Spam: Android (Fast Pair)", "Broadcast Google Fast Pair popups",
                 {self_exe_path(), "--ble-spam", "google"}},
                {"BLE Spam: Samsung", "Broadcast Samsung EasySetup (Watch/Buds) popups",
                 {self_exe_path(), "--ble-spam", "samsung"}},
                {"BLE Spam: Windows (Swift Pair)", "Broadcast Microsoft Swift Pair toasts",
                 {self_exe_path(), "--ble-spam", "microsoft"}},
                {"BLE Spam: All (rotate)", "Rotate through all BLE spam families",
                 {self_exe_path(), "--ble-spam", "all"}},
                {"Enable Bluetooth radio", "rfkill unblock bluetooth", {"rfkill", "unblock", "bluetooth"}, true},
                {"Disable Bluetooth radio", "rfkill block bluetooth", {"rfkill", "block", "bluetooth"}, true},
            },
        },
        {
            "RFID",
            "13.56MHz NFC - M5 ST25R3916 (SPI CAP or I2C) / RFID2 (MFRC522 I2C) native, or PN532 (libnfc)",
            icon::kNfc,
            false,
            // Rows are filled per detected module by refresh_rfid_category();
            // this static placeholder shows only the per-module Detect rows.
            build_rfid_actions(false, false, false),
        },
        {
            "Sub-GHz / RF",
            "M5 CAP CC1101 over SPI, or an SDR dongle (e.g. RTL-SDR)",
            icon::kRf,
            false,
            build_rf_actions(false, -1, -1),
        },
        {
            "Network Tools",
            "curl / ping / netcat helpers",
            icon::kNetwork,
            false,
            {
                {"My public IP (curl)", "curl -s http://ifconfig.me",
                 {"curl", "-s", "http://ifconfig.me"}},
                {"Fetch example.com headers (curl)", "curl -sI http://example.com",
                 {"curl", "-sI", "http://example.com"}},
                {"Ping default gateway", "ping default gateway x4",
                 {"sh", "-c", "ping -c 4 \"$(ip route | awk '/default/ {print $3; exit}')\""}},
                {"Port scan default gateway (nc)", "nc -zv <gateway> 1-1024",
                 {"sh", "-c", "nc -zv \"$(ip route | awk '/default/ {print $3; exit}')\" 1-1024"}},
                {"Start netcat listener :4444", "nc -lvp 4444", {"nc", "-lvp", "4444"}},
            },
        },
        {
            "Nmap",
            "Guided scan: discover hosts, pick one, then Fast or Full port scan",
            icon::kScan,
            false,
            build_nmap_actions(),
        },
        {
            "SMB Client",
            "Anonymous SMB share discovery",
            icon::kStorage,
            false,
            {
                {"List shares on default gateway", "smbclient -L //<gateway> -N",
                 {"sh", "-c", "smbclient -L \"//$(ip route | awk '/default/ {print $3; exit}')\" -N"}},
                {"smbclient version", "smbclient --version", {"smbclient", "--version"}},
            },
        },
        {
            "Responder",
            "LLMNR/NBT-NS/mDNS poisoner - authorized use only",
            icon::kShield,
            false,
            {
                {intern_iface_text("Start Responder on " + wifi_interface()),
                 intern_iface_text("responder -I " + wifi_interface()),
                 {"responder", "-I", wifi_interface()}, true},
                {"Start Responder on eth0", "responder -I eth0",
                 {"responder", "-I", "eth0"}, true},
            },
        },
        {
            "I2C Bus",
            "SDA=G2 SCL=G3 - scan and read I2C devices (i2c-tools)",
            icon::kI2c,
            false,
            {
                {"List I2C buses", "i2cdetect -l", {"i2cdetect", "-l"}},
                {"Scan bus 1", "i2cdetect -y 1", {"i2cdetect", "-y", "1"}},
                {"Dump 0x50 (EEPROM)", "i2cdump -y 1 0x50", {"i2cdump", "-y", "1", "0x50"}},
                {"Read 0x50 reg 0x00", "i2cget -y 1 0x50 0x00", {"i2cget", "-y", "1", "0x50", "0x00"}},
            },
        },
        {
            "SPI Bus",
            "spidev0.0 - probe/read SPI flash (flashrom)",
            icon::kSpi,
            false,
            {
                {"List SPI devices", "ls /dev/spidev*",
                 {"sh", "-c", "ls -l /dev/spidev* 2>/dev/null || echo 'no /dev/spidev* (enable SPI overlay)'"}},
                {"Probe SPI flash", "flashrom -p linux_spi:dev=/dev/spidev0.0",
                 {"flashrom", "-p", "linux_spi:dev=/dev/spidev0.0,spispeed=2000"}},
                {"Read flash -> /tmp/spi.bin", "flashrom ... -r /tmp/spi.bin",
                 {"flashrom", "-p", "linux_spi:dev=/dev/spidev0.0,spispeed=2000", "-r", "/tmp/spi.bin"}},
            },
        },
        {
            "UART / Serial",
            "TX=G14 RX=G15 - monitor a serial line (read only)",
            icon::kUart,
            false,
            {
                {"List serial ports", "ls /dev/ttyS* ...",
                 {"sh", "-c", "ls -l /dev/ttyS* /dev/ttyAMA* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo 'no serial ports found'"}},
                {"Monitor ttyS0 @1200", "stty 1200; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 1200 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @2400", "stty 2400; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 2400 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @4800", "stty 4800; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 4800 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @9600", "stty 9600; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 9600 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @19200", "stty 19200; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 19200 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @38400", "stty 38400; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 38400 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @57600", "stty 57600; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 57600 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @115200", "stty 115200; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 115200 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @230400", "stty 230400; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 230400 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @460800", "stty 460800; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 460800 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
                {"Monitor ttyS0 @921600", "stty 921600; cat /dev/ttyS0",
                 {"sh", "-c", "stty -F /dev/ttyS0 921600 raw -echo 2>/dev/null; cat /dev/ttyS0"}},
            },
        },
        {
            "1-Wire",
            "needs w1-gpio overlay - list/read 1-Wire slaves",
            icon::kOneWire,
            false,
            {
                {"List 1-Wire devices", "ls /sys/bus/w1/devices/",
                 {"sh", "-c", "ls /sys/bus/w1/devices/ 2>/dev/null || echo 'w1 not loaded (needs w1-gpio overlay)'"}},
                {"Read all slaves", "cat .../w1_slave",
                 {"sh", "-c", "for d in /sys/bus/w1/devices/*/w1_slave; do [ -e \"$d\" ] && { echo \"== $d ==\"; cat \"$d\"; }; done"}},
            },
        },
        {
            "GPIO / DIO",
            "G7/G9/G10/G11/G22/G23/G26 - libgpiod (verify offsets: G# != line#)",
            icon::kGpio,
            false,
            {
                {"Detect gpiochips", "gpiodetect", {"gpiodetect"}},
                {"Info gpiochip0", "gpioinfo gpiochip0", {"gpioinfo", "gpiochip0"}},
                {"Read line 7", "gpioget gpiochip0 7", {"gpioget", "gpiochip0", "7"}},
                {"Set line 7 = 1 (5s)", "gpioset --mode=time --sec=5 ... 7=1",
                 {"gpioset", "--mode=time", "--sec=5", "gpiochip0", "7=1"}},
                {"Monitor line 7", "gpiomon gpiochip0 7", {"gpiomon", "gpiochip0", "7"}},
            },
        },
        {
            "Infrared",
            "Send/receive IR via a gpio-ir overlay (/dev/lirc*) - needs an IR LED/RX",
            icon::kIr,
            false,
            {
                {"List IR devices", "ls /dev/lirc*",
                 {"sh", "-c", "ls -l /dev/lirc* 2>/dev/null || "
                  "echo 'no /dev/lirc* - enable the gpio-ir / gpio-ir-tx overlay'"}},
                {"IR receive (record 15s)", "ir-ctl -r -d /dev/lirc0",
                 {"sh", "-c", "timeout 15 ir-ctl -r -d /dev/lirc1 2>&1 || timeout 15 ir-ctl -r "
                  "-d /dev/lirc0 2>&1 || echo 'needs an IR receiver + v4l-utils (ir-ctl)'"}},
                {"IR send last capture", "ir-ctl -d /dev/lirc0 --send=/tmp/ir.txt",
                 {"sh", "-c", "[ -f /tmp/ir.txt ] && ir-ctl -d /dev/lirc0 --send=/tmp/ir.txt 2>&1 || "
                  "echo 'capture first (redirect IR receive to /tmp/ir.txt), or use lirc/irsend'"}},
                {"TV-B-Gone (note)", "power-off code blast needs a code DB",
                 {"sh", "-c", "echo 'TV-B-Gone needs a per-brand IR code set. Capture codes with IR "
                  "receive, save under ~/.bruce/scripts as a sender script, then run from Scripts.'"}},
            },
        },
        {
            "GPS",
            "Wardriving, GPS Tracker and GPS config",
            icon::kGps,
            false,
            build_gps_actions(),
        },
        {
            "Ethernet",
            "Wired-LAN recon and L2 attacks on eth0 - authorized use only",
            icon::kEth,
            false,
            {
                {"Scan hosts (arp-scan eth0)", "arp-scan -I eth0 --localnet",
                 {"sh", "-c", "arp-scan -I eth0 --localnet 2>&1 || echo 'needs arp-scan + eth0 up + root'"}},
                {"MAC flood (macof)", "macof -i eth0",
                 {"sh", "-c", "macof -i eth0 2>&1 || echo 'needs dsniff (macof) + root'"}},
                {"DHCP starvation (dhcpstarv)", "dhcpstarv -i eth0",
                 {"sh", "-c", "dhcpstarv -i eth0 2>&1 || echo 'needs dhcpstarv (or yersinia) + root'"}},
            },
        },
        {
            "Files",
            "Browse storage and serve files over HTTP (WebUI)",
            icon::kFiles,
            false,
            {
                {"List home + removable", "ls ~ /media /mnt",
                 {"sh", "-c", "echo '== home =='; ls -la ~ 2>/dev/null; echo '== /media =='; "
                  "ls -la /media/* 2>/dev/null; echo '== /mnt =='; ls -la /mnt 2>/dev/null"}},
                {"Disk usage", "df -h", {"df", "-h"}},
                {"WebUI: serve home :8080", "python3 -m http.server 8080",
                 {"sh", "-c", "cd ~ && echo \"Serving $PWD at http://$(hostname -I 2>/dev/null | "
                  "awk '{print $1}'):8080 (Ctrl-C/back to stop)\"; python3 -m http.server 8080 2>&1 || "
                  "echo 'needs python3'"}},
            },
        },
        {
            "System / Clock",
            "Clock, timer, uptime and handy local tools",
            icon::kClock,
            false,
            build_system_clock_actions(),
        },
        {
            "Terminal",
            "Interactive shell - run commands as the user or as root (sudo)",
            icon::kUart,
            false,
            {
                {"Terminal (user)", "Interactive shell as the logged-in user",
                 {"sh", "-c", "echo 'terminal view unavailable'"},
                 false, false, true},
                {"Root terminal (sudo)",
                 "Interactive shell; commands run as root through the "
                 "sudoers-scoped --root-sh passthrough",
                 {"sh", "-c", "echo 'terminal view unavailable'"},
                 false, false, true},
            },
        },
        {
            "Scripts",
            "Run your own executable scripts from ~/.bruce/scripts (GPIO, RF, net...)",
            icon::kUart,
            false,
            scan_scripts(),
        },
        {
            "Theme",
            "Pick the UI accent color - applies instantly",
            icon::kPalette,
            false,
            build_theme_actions(),
        },
        {
            "About",
            "Version and usage notice",
            icon::kInfo,
            true,
            {},
        },
    };
    return catalog;
}

const std::vector<ToolCategory>& tool_catalog() {
    return mutable_catalog();
}

std::string nmap_state_dir() { return nmap_dir(); }

void refresh_dynamic_categories(int category_index) {
    auto& catalog = mutable_catalog();
    for (size_t i = 0; i < catalog.size(); ++i) {
        auto& category = catalog[i];
        const std::string label = category.label;
        if (label == "Nmap") {
            category.actions = build_nmap_actions();
        } else if (label == "Wi-Fi") {
            category.actions = build_wifi_actions();
        } else if (label == "System / Clock") {
            category.actions = build_system_clock_actions();
        } else if (label == "GPS") {
            category.actions = build_gps_actions();
        }
    }
}

void refresh_gps_category(const std::vector<std::string>& port_candidates) {
    gps_port_candidates() = port_candidates;
    for (auto& category : mutable_catalog()) {
        if (std::string(category.label) == "GPS") {
            category.actions = build_gps_actions();
            return;
        }
    }
}

std::string category_header(int category_index) {
    const auto& catalog = mutable_catalog();
    if (category_index < 0 || category_index >= static_cast<int>(catalog.size())) {
        return "";
    }
    const std::string label = catalog[static_cast<size_t>(category_index)].label;
    if (label == "GPS") {
        return gps_header();
    }
    return label;
}

int preferred_selection(int category_index) {
    const auto& catalog = mutable_catalog();
    if (category_index < 0 || category_index >= static_cast<int>(catalog.size())) {
        return -1;
    }
    if (std::string(catalog[static_cast<size_t>(category_index)].label) == "GPS") {
        return gps_preferred_selection();
    }
    return -1;
}

void refresh_rf_category(bool cc1101_present, int cs_gpio, int gdo0_gpio) {
    for (auto& category : mutable_catalog()) {
        if (std::string(category.label) == "Sub-GHz / RF") {
            category.actions = build_rf_actions(cc1101_present, cs_gpio, gdo0_gpio);
            return;
        }
    }
}

void refresh_rfid_category(bool st25r_present, bool rfid2_present, bool pn532_present) {
    for (auto& category : mutable_catalog()) {
        if (std::string(category.label) == "RFID") {
            category.actions = build_rfid_actions(st25r_present, rfid2_present, pn532_present);
            return;
        }
    }
}

bool rfid_setup_is_open() { return rfid_setup_open(); }

std::string take_navigation_error(int category_index) {
    const auto& catalog = mutable_catalog();
    if (category_index < 0 || category_index >= static_cast<int>(catalog.size()) ||
        std::string(catalog[static_cast<size_t>(category_index)].label) != "GPS") {
        return "";
    }
    std::string err;
    err.swap(gps_state().error);
    return err;
}

bool on_navigation_action(int category_index, int item_index) {
    if (category_index < 0) {
        return false;
    }
    const auto& catalog = mutable_catalog();
    if (category_index >= static_cast<int>(catalog.size())) {
        return false;
    }
    const std::string label = catalog[static_cast<size_t>(category_index)].label;

    if (label == "GPS") {
        return gps_navigation(item_index);
    }

    if (label == "Nmap") {
        // Mirror build_nmap_actions()'s row layout to map the tapped row to an action.
        if (!ensure_nmap_dir().empty()) {
            return false; // unsafe dir: don't read or write state there
        }
        const std::string dir = nmap_dir();
        NmapHost sel;
        if (!read_nmap_selected(dir, sel)) {
            // Row 0 is "Scan" (runnable); rows >= 1 are discovered hosts.
            const auto hosts = read_nmap_hosts(dir);
            const int host = item_index - 1;
            if (host >= 0 && host < static_cast<int>(hosts.size())) {
                if (!write_file(dir + "/selected", serialize_nmap_host(hosts[static_cast<size_t>(host)]))) {
                    std::fprintf(stderr, "nmap: cannot write %s/selected\n", dir.c_str());
                }
            }
        } else if (item_index == 0) {
            // Row 0 is "Back to host list".
            const std::string sel = dir + "/selected";
            if (std::remove(sel.c_str()) != 0 && errno != ENOENT) {
                std::fprintf(stderr, "nmap: cannot remove %s: %s\n", sel.c_str(), std::strerror(errno));
            }
        }
        return false;
    }

    if (label == "Wi-Fi") {
        // Evil Portal clone rows sit after the fixed prefix; row layout mirrors
        // build_portal_rows(): [scan][no-APs] / [scan][AP...] / [back][clone].
        const int static_count = static_cast<int>(build_wifi_static_actions().size());
        if (item_index < static_count) {
            return false;
        }
        const std::string dir = portal_dir();
        const std::string selected = read_first_line(dir + "/selected");
        if (selected.empty()) {
            // The block's row 0 is the scan (runnable); rows >= 1 are targets.
            const auto targets = read_lines(dir + "/targets");
            const int target = item_index - static_count - 1;
            if (target >= 0 && target < static_cast<int>(targets.size())) {
                write_file(dir + "/selected", targets[static_cast<size_t>(target)]);
            }
        } else if (item_index == static_count) {
            // "< Back to AP list".
            std::remove((dir + "/selected").c_str());
        }
    }

    if (label == "System / Clock") {
        // One nav row toggles the in-place timezone picker: "Set timezone >"
        // opens it, "<  Back" closes it. Zone rows are runnable (not nav rows).
        const auto& acts = catalog[static_cast<size_t>(category_index)].actions;
        if (item_index < 0 || item_index >= static_cast<int>(acts.size())) {
            return false;
        }
        const std::string row = acts[static_cast<size_t>(item_index)].label;
        if (row.rfind("Set timezone", 0) == 0) {
            tz_picker_open() = true;
        } else if (!row.empty() && row[0] == '<') {
            tz_picker_open() = false;
        }
        return false;
    }

    if (label == "RFID") {
        // "Setup Modules >" opens the detect/setup sub-view; "<  Back" closes it.
        const auto& acts = catalog[static_cast<size_t>(category_index)].actions;
        if (item_index < 0 || item_index >= static_cast<int>(acts.size())) {
            return false;
        }
        const std::string row = acts[static_cast<size_t>(item_index)].label;
        if (row.rfind("Setup Modules", 0) == 0) {
            rfid_setup_open() = true;
        } else if (!row.empty() && row[0] == '<') {
            rfid_setup_open() = false;
        }
        return false;
    }
    return false;
}

bool on_category_back(int category_index) {
    const auto& catalog = mutable_catalog();
    if (category_index < 0 || category_index >= static_cast<int>(catalog.size())) {
        return false;
    }
    const std::string label = catalog[static_cast<size_t>(category_index)].label;
    // Each case does exactly what that category's "< Back ..." row does.
    auto clear_selected = [](const std::string& dir) {
        const std::string sel = dir + "/selected";
        if (std::remove(sel.c_str()) != 0 && errno != ENOENT) {
            std::fprintf(stderr, "cannot remove %s: %s\n", sel.c_str(), std::strerror(errno));
        }
    };
    if (label == "Nmap") {
        NmapHost sel;
        if (!ensure_nmap_dir().empty() || !read_nmap_selected(nmap_dir(), sel)) {
            return false; // host list (or unusable dir): leave the category
        }
        clear_selected(nmap_dir());
        return true;
    }
    if (label == "Wi-Fi") {
        if (read_first_line(portal_dir() + "/selected").empty()) {
            return false;
        }
        clear_selected(portal_dir());
        return true;
    }
    if (label == "System / Clock") {
        if (!tz_picker_open()) {
            return false;
        }
        tz_picker_open() = false;
        return true;
    }
    if (label == "RFID") {
        if (!rfid_setup_open()) {
            return false; // main view: leave the category as usual
        }
        rfid_setup_open() = false; // close the Setup Modules sub-view
        return true;
    }
    if (label == "GPS") {
        return gps_back();
    }
    return false;
}

namespace {

// Shared sudo escalation for a non-root app: `sudo <flags> <command>`, where
// command is either a native re-exec of this binary (already authorized by the
// sudoers rule) or, for other root-needing rows, this binary's `--root-sh`
// passthrough. `flags` selects the authentication mode: `{"-n"}` for the
// passwordless rule, `{"-S", "-p", ""}` for the password prompt path.
std::vector<std::string> escalated_argv(const ToolAction& action,
                                        const std::vector<const char*>& flags) {
    std::vector<std::string> out;
    if (action.argv[0] == self_exe_path()) {
        out.reserve(action.argv.size() + flags.size() + 1);
    } else {
        out.reserve(flags.size() + 4);
    }
    out.emplace_back("sudo");
    for (const char* flag : flags) {
        out.emplace_back(flag);
    }
    // Native re-execs (--evil-portal, --ble-spam, ...): run directly.
    if (action.argv[0] == self_exe_path()) {
        out.insert(out.end(), action.argv.begin(), action.argv.end());
        return out;
    }
    // Everything else root-needing (iw scans, monitor mode, rogue AP,
    // responder): route through the same sudoers-authorized binary via its
    // --root-sh passthrough, so the rule stays scoped to this one file.
    std::string script;
    if (action.argv[0] == "sh" && action.argv.size() >= 3 && action.argv[1] == "-c") {
        script = action.argv[2];
    } else {
        script = "exec";
        for (const std::string& word : action.argv) {
            script += " " + sh_quote(word); // exec: the Stop button kills the tool, not sh
        }
    }
    out.push_back(self_exe_path());
    out.emplace_back("--root-sh");
    out.push_back(std::move(script));
    return out;
}

} // namespace

std::vector<std::string> launch_argv(const ToolAction& action) {
    // A root app already has the rights; empty argv is a navigation row;
    // unflagged external rows (nmap, curl, ...) run as-is.
    if (action.argv.empty() || geteuid() == 0 ||
        !(action.argv[0] == self_exe_path() || action.needs_root)) {
        return action.argv;
    }
    return escalated_argv(action, {"-n"}); // never prompt: the sudoers.d rule makes it passwordless
}

std::string action_command_text(const ToolAction& action) {
    if (action.argv.empty()) {
        return "";
    }
    // `sh -c "<script>"` rows: the script in argv[2] is the real command.
    if (action.argv[0] == "sh" && action.argv.size() >= 3 &&
        (action.argv[1] == "-c" || action.argv[1] == "-ec")) {
        return action.argv[2];
    }
    std::string out = action.argv[0];
    for (size_t i = 1; i < action.argv.size(); ++i) {
        out += ' ';
        out += action.argv[i];
    }
    return out;
}

std::string wifi_interface() {
    const char* base = "/sys/class/net";
    DIR* dir = ::opendir(base);
    if (!dir) {
        return "wlan0";
    }
    std::string first;
    for (struct dirent* e = ::readdir(dir); e; e = ::readdir(dir)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        struct stat st;
        const std::string path = std::string(base) + "/" + e->d_name + "/phy80211";
        if (::stat(path.c_str(), &st) == 0) {
            if (std::string(e->d_name) == "wlan0") {
                ::closedir(dir);
                return "wlan0"; // prefer the device's own name when present
            }
            if (first.empty()) {
                first = e->d_name;
            }
        }
    }
    ::closedir(dir);
    return first.empty() ? "wlan0" : first;
}

bool action_needs_root(const ToolAction& action) {
    return !action.argv.empty() && geteuid() != 0 &&
           (action.argv[0] == self_exe_path() || action.needs_root);
}

bool sudo_passwordless() {
    if (geteuid() == 0) {
        return true;
    }
    const std::string self = self_exe_path(); // before fork: no allocation in the child
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        // Same session context as ProcessRunner, which setsid()s every child:
        // sudo caches credentials per tty, so a probe that kept the launching
        // terminal's tty could pass on that terminal's cached ticket while the
        // real tty-less `sudo -n` launch then failed with "a password is
        // required" - and the password dialog was never offered.
        ::setsid();
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        // Probe the exact command the sudoers rule authorizes (this binary),
        // not `true`: the rule does not cover `true`, so `sudo -n true` only
        // passed while a cached sudo timestamp happened to be valid.
        ::execlp("sudo", "sudo", "-n", self.c_str(), "--root-sh", "true",
                 static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    return ::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::vector<std::string> sudo_argv_with_password(const ToolAction& action) {
    // Same gating as launch_argv: only rows it would wrap belong here.
    if (action.argv.empty() || geteuid() == 0 ||
        !(action.argv[0] == self_exe_path() || action.needs_root)) {
        return action.argv;
    }
    return escalated_argv(action, {"-S", "-p", ""});
}

TerminalAuth terminal_auth_mode() {
    if (geteuid() == 0) {
        return TerminalAuth::NotNeeded;
    }
    return sudo_passwordless() ? TerminalAuth::Passwordless : TerminalAuth::Password;
}

std::vector<std::string> terminal_command_argv(const std::string& command, bool as_root) {
    // User commands - and root commands when the app itself is root - run in a
    // plain shell. ProcessRunner::stop() signals the process group, so Stop
    // kills both sh and the command it spawned.
    if (!as_root || geteuid() == 0) {
        return {"sh", "-c", command};
    }
    // Root commands ride the same sudoers-scoped `--root-sh` passthrough as
    // every root catalog row (see escalated_argv): the rule authorizes exactly
    // this binary, with any argv, so a user-typed command adds no new access.
    std::vector<std::string> out;
    out.reserve(6);
    out.emplace_back("sudo");
    if (terminal_auth_mode() == TerminalAuth::Passwordless) {
        out.emplace_back("-n");
    } else {
        out.emplace_back("-S");
        out.emplace_back("-p");
        out.emplace_back("");
    }
    out.push_back(self_exe_path());
    out.emplace_back("--root-sh");
    out.push_back(command);
    return out;
}

} // namespace model
