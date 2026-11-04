/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "tool_catalog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
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

// --- Nmap guided flow: discover hosts -> pick host -> Fast/Full port scan -----
//
// The menu is a flat two-level list, so the drill-down is driven by two tiny
// state files under /tmp/bruce-nmap: `hosts` (one live IP per line, written by
// the discovery step) and `selected` (the host the user tapped). The "Nmap"
// category's action list is rebuilt from this state on every menu entry
// (refresh_dynamic_categories), and host/"back" rows carry an empty argv so the
// menu treats them as navigation (on_navigation_action) rather than tools.

std::string nmap_dir() { return "/tmp/bruce-nmap"; }

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

void write_file(const std::string& path, const std::string& content) {
    std::ofstream(path) << content;
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
    const std::string selected = read_first_line(dir + "/selected");

    std::vector<ToolAction> acts;

    if (selected.empty()) {
        // Step 1: discover hosts on the local subnet.
        acts.push_back(ToolAction{
            "1. Scan / refresh hosts",
            "Ping-sweep the local subnet for live hosts, then go Back and pick one.",
            {"sh", "-c",
             "mkdir -p " + dir + "; "
             "net=$(ip -o -f inet addr show | awk '$2!=\"lo\"{print $4; exit}'); "
             "if [ -z \"$net\" ]; then echo 'no network (connect Wi-Fi first)'; exit 1; fi; "
             "echo \"Discovering hosts on $net ...\"; "
             "rm -f " + dir + "/selected; "
             "nmap -sn \"$net\" -oG - 2>/dev/null | awk '/Status: Up/{print $2}' | tee " + dir + "/hosts; "
             "echo; echo 'Done. Go Back, pick a host, then choose Fast or Full.'"}});

        // Step 2: one navigation row per discovered host (empty argv).
        const auto hosts = read_lines(dir + "/hosts");
        if (hosts.empty()) {
            acts.push_back(ToolAction{
                "(no hosts yet - run Scan above)",
                "Run 'Scan / refresh hosts' first; discovered hosts appear here.",
                {"sh", "-c", "echo 'Run Scan / refresh hosts first.'"}});
        } else {
            for (const auto& h : hosts) {
                acts.push_back(ToolAction{
                    own("Host " + h + "  >  choose scan"),
                    "Select this host, then pick Fast or Full port scan.",
                    {}}); // empty argv => navigation row
            }
        }
    } else {
        // Step 3: a host is selected - offer Fast / Full, plus a way back.
        acts.push_back(ToolAction{
            "< Back to host list",
            "Clear the selected host and choose another.",
            {}}); // navigation row
        acts.push_back(ToolAction{
            own("Fast scan " + selected + "  (top 100 ports)"),
            "nmap -F -T4: quick scan of the most common ports.",
            {"nmap", "-F", "-T4", selected}});
        acts.push_back(ToolAction{
            own("Full scan " + selected + "  (all 65535 ports)"),
            "nmap -p- -T4: every TCP port - thorough but slower.",
            {"nmap", "-p-", "-T4", selected}});
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

std::vector<ToolAction> build_wifi_static_actions() {
    return {
        // --- recon ---
        // needs_root rows are escalated by model::launch_argv (sudo -n <self>
        // --root-sh <script>); no in-script `id -u` guard needed.
        {"Scan nearby networks", "iw dev wlan0 scan (needs root)",
         {"sh", "-c", "ip link set wlan0 up 2>/dev/null; iw dev wlan0 scan"},
         true},
        {"List SSIDs + signal", "iw scan | ssid/signal/freq",
         {"sh", "-c",
          "iw dev wlan0 scan 2>/dev/null | grep -E 'SSID:|signal:|freq:' || "
          "echo 'scan failed (is wlan0 up?)'"},
         true},
        {"Channel usage (APs/chan)", "scan grouped by channel",
         {"sh", "-c",
          "iw dev wlan0 scan 2>/dev/null | awk '/freq:/{print $2}' | sort -n | uniq -c | "
          "awk '{printf \"  %s MHz: %s APs\\n\", $2, $1}' || echo 'scan failed'"},
         true},
        {"Show Wi-Fi status", "ip addr show wlan0", {"ip", "addr", "show", "wlan0"}},
        {"Scan LAN hosts (arp-scan)", "arp-scan --localnet",
         {"sh", "-c", "arp-scan --localnet 2>&1 || echo 'needs arp-scan (apt install arp-scan)'"},
         true},
        {"Scan LAN hosts (nmap)", "nmap -sn <subnet>",
         {"sh", "-c",
          "nmap -sn \"$(ip -o -f inet addr show wlan0 | awk '{print $4}')\" 2>&1 || "
          "echo 'needs nmap; is wlan0 connected?'"}},

        // --- monitor mode (prerequisite for the attacks below) ---
        // airmon-ng frequently produces no wlan0mon on this Wi-Fi chip, so fall
        // back to a manual monitor VIF (mon0) and report what to attack with.
        {"Monitor mode ON (airmon-ng)", "airmon-ng; fallback: manual mon0 VIF",
         {"sh", "-c",
          "airmon-ng check kill 2>&1; airmon-ng start wlan0 2>&1; "
          "if ! ip link show wlan0mon >/dev/null 2>&1; then "
          "echo; echo 'airmon-ng produced no wlan0mon (common on this chip) - creating mon0:'; "
          "iw dev wlan0 interface add mon0 type monitor 2>&1 && ip link set mon0 up; fi; "
          "echo; mon=; for d in wlan0mon mon0; do ip link show $d >/dev/null 2>&1 && mon=$d && break; done; "
          "[ -n \"$mon\" ] && echo \"monitor interface: $mon (airodump/mdk4 find it automatically)\" || "
          "echo 'no monitor interface - this radio does not support monitor mode'"},
         true},
        {"Monitor mode OFF", "remove monitor ifaces; restart NetworkManager",
         {"sh", "-c",
          "for d in wlan0mon mon0; do ip link del $d 2>/dev/null && echo \"removed $d\"; done; "
          "airmon-ng stop wlan0mon 2>/dev/null; rfkill unblock wifi; "
          "systemctl restart NetworkManager 2>/dev/null; echo done"},
         true},
        // Consumers pick whichever monitor VIF exists (wlan0mon from airmon-ng
        // or mon0 from the manual fallback) instead of hardcoding wlan0mon.
        {"Airodump scan (APs+clients)", "airodump-ng <monitor if>",
         {"sh", "-c",
          "mon=; for d in wlan0mon mon0; do [ -e /sys/class/net/$d ] && mon=$d && break; done; "
          "[ -n \"$mon\" ] || { echo 'enable Monitor mode first'; exit 1; }; airodump-ng \"$mon\" 2>&1"}},
        {"Capture handshakes -> /tmp", "airodump-ng -w /tmp/bruce-wifi <mon>",
         {"sh", "-c",
          "mon=; for d in wlan0mon mon0; do [ -e /sys/class/net/$d ] && mon=$d && break; done; "
          "[ -n \"$mon\" ] || { echo 'enable Monitor mode first'; exit 1; }; "
          "airodump-ng -w /tmp/bruce-wifi \"$mon\" 2>&1"}},

        // --- attacks (authorized testing only) ---
        {"Deauth flood (mdk4)", "mdk4 <monitor if> d",
         {"sh", "-c",
          "mon=; for d in wlan0mon mon0; do [ -e /sys/class/net/$d ] && mon=$d && break; done; "
          "[ -n \"$mon\" ] || { echo 'needs Monitor mode + mdk4'; exit 1; }; "
          "mdk4 \"$mon\" d 2>&1 || echo 'needs mdk4'"}},
        {"Beacon spam (mdk4)", "mdk4 <monitor if> b -s 100",
         {"sh", "-c",
          "mon=; for d in wlan0mon mon0; do [ -e /sys/class/net/$d ] && mon=$d && break; done; "
          "[ -n \"$mon\" ] || { echo 'needs Monitor mode + mdk4'; exit 1; }; "
          "mdk4 \"$mon\" b -s 100 2>&1 || echo 'needs mdk4'"}},
        // hostapd needs driver=nl80211 (it refuses to start without it) and
        // clients need DHCP+DNS to land anywhere: dnsmasq answers every name
        // with the AP. Both run in this script's process group, so the tool
        // Stop button kills them together.
        {"Rogue open AP (hostapd)", "open AP 'Free WiFi' + DHCP/DNS captive on wlan0",
         {"sh", "-c",
          "command -v hostapd >/dev/null 2>&1 || "
          "{ echo 'needs hostapd + dnsmasq (apt install hostapd dnsmasq)'; exit 1; }; "
          "cfg=$(mktemp); printf 'driver=nl80211\\ninterface=wlan0\\nssid=Free WiFi\\nchannel=6\\n"
          "hw_mode=g\\nauth_algs=1\\n' > \"$cfg\"; "
          "ip addr replace 192.168.4.1/24 dev wlan0 2>/dev/null; ip link set wlan0 up; "
          "dns=$(mktemp); printf 'interface=wlan0\\nbind-interfaces\\nexcept-interface=lo\\nno-resolv\\n"
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

    const std::string dir = portal_dir();
    const std::string selected = read_first_line(dir + "/selected");
    std::vector<ToolAction> acts;

    if (selected.empty()) {
        acts.push_back(ToolAction{
            "Evil Portal: Clone an AP  (1. scan first)",
            "Scan nearby APs, then go Back and pick one to clone. Needs root.",
            {"sh", "-c",
             "mkdir -p " + dir + "; rm -f " + dir + "/selected; "
             "ip link set wlan0 up 2>/dev/null; "
             "iw dev wlan0 scan 2>/dev/null | awk "
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

// Mutable backing store so data-driven categories (Nmap) can be rebuilt in
// place; tool_catalog() hands out a const view of it.
std::vector<ToolCategory>& mutable_catalog() {
    static std::vector<ToolCategory> catalog = {
        {
            "Wi-Fi",
            "Scan, recon, 802.11 attacks and Evil Portal (wlan0; root needed)",
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
                {"Scan for BLE devices (15s)", "timeout 15 hcitool lescan",
                 {"timeout", "15", "hcitool", "lescan"}},
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
                {"Enable Bluetooth radio", "rfkill unblock bluetooth", {"rfkill", "unblock", "bluetooth"}},
                {"Disable Bluetooth radio", "rfkill block bluetooth", {"rfkill", "block", "bluetooth"}},
            },
        },
        {
            "RFID",
            "13.56MHz NFC - M5 ST25R3916 CAP (native) or a PN532 reader (via libnfc)",
            icon::kNfc,
            false,
            {
                // --- M5 RF+NFC CAP: ST25R3916 over HAT SPI (native driver) ---
                // The app powers the cap chain first (M5IOE1 PYG13 HAT rail +
                // GPIO26 POWER_EN) and bit-bangs NFC_CS on GPIO22; spidev must
                // exist (sudo raspi-config nonint do_spi 0, once).
                {"ST25R: Read tag (UID/ATQA/SAK)",
                 "Power the field and read a 13.56MHz ISO14443A tag's UID, ATQA and SAK.",
                 {self_exe_path(), "--rfid-read"}},
                {"ST25R: Dump tag (Type-2 / NTAG)",
                 "Read UID then dump the data pages of a Type-2 tag (NTAG/Ultralight).",
                 {self_exe_path(), "--rfid-dump"}},
                {"ST25R: Clone -> emulate tag",
                 "Read a tag's UID, then emulate it as an NFC-A target until you go back.",
                 {self_exe_path(), "--rfid-clone"}},
                {"ST25R: Detect chip",
                 "Probe the ST25R3916: powers the cap up, then reads IC_IDENTITY.",
                 {self_exe_path(), "--st25r-detect"}},

                // --- PN532 reader over I2C/SPI/UART (via libnfc) ---
                {"PN532: Scan via I2C (/dev/i2c-1)",
                 "Test a PN532 wired on I2C bus 1 and read any tag present (nfc-list).",
                 {"sh", "-c",
                  "LIBNFC_DEFAULT_DEVICE=pn532_i2c:/dev/i2c-1 nfc-list -v 2>&1 || "
                  "echo 'No PN532 on I2C bus 1. Check wiring / install libnfc-bin.'"}},
                {"PN532: Scan via UART (/dev/ttyS0)",
                 "Test a PN532 wired on UART and read any tag present (nfc-list).",
                 {"sh", "-c",
                  "LIBNFC_DEFAULT_DEVICE=pn532_uart:/dev/ttyS0 nfc-list -v 2>&1 || "
                  "echo 'No PN532 on ttyS0. Check wiring/baud / install libnfc-bin.'"}},
                {"PN532: Poll for tag (configured)",
                 "nfc-poll using the device in /etc/nfc/libnfc.conf or $LIBNFC_DEFAULT_DEVICE.",
                 {"sh", "-c",
                  "nfc-poll 2>&1 || echo 'No PN532 found. Use a Scan entry to test the bus, "
                  "or set /etc/nfc/libnfc.conf. Needs libnfc-bin.'"}},
                {"PN532: Dump MIFARE Classic",
                 "nfc-mfclassic read (default keys) to /tmp/bruce-mfc.mfd.",
                 {"sh", "-c",
                  "nfc-mfclassic r a /tmp/bruce-mfc.mfd 2>&1 || "
                  "echo 'Needs a PN532 + MIFARE Classic tag (default keys). Install libnfc-bin.'"}},
                {"PN532: Dump Ultralight/NTAG",
                 "nfc-mfultralight read to /tmp/bruce-ul.mfd.",
                 {"sh", "-c",
                  "nfc-mfultralight r /tmp/bruce-ul.mfd 2>&1 || "
                  "echo 'Needs a PN532 + Ultralight/NTAG tag. Install libnfc-bin.'"}},
                {"PN532: Emulate NDEF tag",
                 "Emulate a Type-2 NDEF tag (nfc-emulate-forum-tag2).",
                 {"sh", "-c",
                  "nfc-emulate-forum-tag2 2>&1 || "
                  "echo 'Needs a PN532. nfc-emulate-forum-tag2 is in libnfc-examples.'"}},
                {"PN532: Emulate UID 01:02:03:04",
                 "Emulate a tag advertising a fixed UID (nfc-emulate-uid).",
                 {"sh", "-c",
                  "nfc-emulate-uid 01020304 2>&1 || "
                  "echo 'Needs a PN532. nfc-emulate-uid is in libnfc-examples.'"}},
                {"PN532: Setup / buses help",
                 "How to point libnfc at your PN532, and list available buses.",
                 {"sh", "-c",
                  "echo 'PN532 via libnfc. Set the device persistently in /etc/nfc/libnfc.conf:'; "
                  "echo '  device.connstring = \"pn532_i2c:/dev/i2c-1\"   (or pn532_uart:/dev/ttyS0,'; "
                  "echo '   pn532_spi:/dev/spidev0.0:500000)  and  allow_autoscan = true'; echo; "
                  "echo 'I2C buses:'; ls /dev/i2c-* 2>/dev/null || echo '  (none; enable i2c overlay)'; "
                  "echo 'SPI:'; ls /dev/spidev* 2>/dev/null || echo '  (none)'; "
                  "echo 'Serial:'; ls /dev/ttyS* /dev/ttyUSB* /dev/ttyAMA* 2>/dev/null || echo '  (none)'; "
                  "echo; nfc-list -v 2>&1 | head -4 || echo 'install libnfc-bin'"}},
            },
        },
        {
            "Sub-GHz / RF",
            "M5 CAP CC1101 over SPI, or an SDR dongle (e.g. RTL-SDR)",
            icon::kRf,
            false,
            {
                {"Use CC1101 CAP (detect)",
                 "Select the M5 CAP CC1101: powers the cap up, probes it over "
                 "SPI (CS1/G7) and confirms the chip responds.",
                 {self_exe_path(), "--cc1101-detect"}},
                {"Capture 433MHz (30s, RTL-SDR dongle)", "timeout 30 rtl_433",
                 {"sh", "-c",
                  "command -v rtl_433 >/dev/null 2>&1 || "
                  "{ echo 'rtl_433 is not installed (and it needs an RTL-SDR USB dongle).'; "
                  "echo 'Run: sudo apt install rtl-433'; exit 127; }; timeout 30 rtl_433 2>&1"}},
                {"List SDR devices (RTL-SDR dongle)", "rtl_test -t",
                 {"sh", "-c",
                  "command -v rtl_test >/dev/null 2>&1 || "
                  "{ echo 'rtl_test is not installed (and it needs an RTL-SDR USB dongle).'; "
                  "echo 'Run: sudo apt install rtl-sdr'; exit 127; }; rtl_test -t 2>&1"}},
                {"FM radio RX 100.0MHz (RTL-SDR)", "rtl_fm | aplay",
                 {"sh", "-c",
                  "command -v rtl_fm >/dev/null 2>&1 || "
                  "{ echo 'rtl_fm is not installed. Run: sudo apt install rtl-sdr'; exit 127; }; "
                  "rtl_fm -f 100M -M wbfm -s 200k -r 48k 2>/dev/null | aplay -r 48k -f S16_LE 2>&1 || "
                  "echo 'playback failed - needs an RTL-SDR dongle + alsa-utils (aplay)'"}},
            },
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
                {"Start Responder on wlan0", "responder -I wlan0",
                 {"responder", "-I", "wlan0"}, true},
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
            "Read a GPS on serial/USB via gpsd; feed wardriving",
            icon::kGps,
            false,
            {
                {"GPS fix (gpspipe)", "gpspipe -w (gpsd)",
                 {"sh", "-c", "timeout 20 gpspipe -w 2>&1 || echo 'needs gpsd + a GPS (gpsd-clients). "
                  "Start: sudo gpsd /dev/ttyUSB0 -F /var/run/gpsd.sock'"}},
                {"Raw NMEA (ttyUSB0 @9600)", "stty 9600; cat /dev/ttyUSB0",
                 {"sh", "-c", "stty -F /dev/ttyUSB0 9600 raw -echo 2>/dev/null; timeout 15 cat /dev/ttyUSB0 "
                  "2>&1 || echo 'no GPS on ttyUSB0 (try ttyS0/ttyACM0)'"}},
                {"Wardriving -> WiGLE CSV",
                 "Scan Wi-Fi + GPS fix and log APs in WiGLE format (upload to wigle.net). "
                 "Needs gpsd running and wlan0 up.",
                 {self_exe_path(), "--wardrive", "wlan0"}},
            },
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
            {
                {"Clock", "A full-screen themed clock \xe2\x80\x94 just the time and date",
                 {"sh", "-c", "while true; do printf '\\r%s' \"$(date '+%Y-%m-%d %H:%M:%S')\"; sleep 1; done"},
                 false, /*clock_view=*/true},
                {"Countdown timer (60s)", "sleep 60; beep",
                 {"sh", "-c", "for i in $(seq 60 -1 1); do printf '\\r%2ds remaining ' \"$i\"; sleep 1; done; "
                  "echo; echo 'Time up!'"}},
                {"Uptime + load", "uptime", {"uptime"}},
                // Installs the optional/offensive tooling (Suggests set) with no
                // password prompt: needs_root routes this through sudo -n <self>
                // --root-sh, and the package's sudoers rule makes that passwordless.
                {"Install optional tools (apt)",
                 "apt-get install the Suggests set (aircrack-ng, mdk4, hostapd, "
                 "arp-scan, dnsmasq, ...). Passwordless via the installed sudoers rule.",
                 {"sh", "-c",
                  "export DEBIAN_FRONTEND=noninteractive; "
                  "apt-get update 2>&1; "
                  "apt-get install -y aircrack-ng mdk4 hostapd arp-scan dnsmasq "
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
            },
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

void refresh_dynamic_categories() {
    for (auto& category : mutable_catalog()) {
        const std::string label = category.label;
        if (label == "Nmap") {
            category.actions = build_nmap_actions();
        } else if (label == "Wi-Fi") {
            category.actions = build_wifi_actions();
        }
    }
}

void on_navigation_action(int category_index, int item_index) {
    if (category_index < 0) {
        return;
    }
    const auto& catalog = mutable_catalog();
    if (category_index >= static_cast<int>(catalog.size())) {
        return;
    }
    const std::string label = catalog[static_cast<size_t>(category_index)].label;

    if (label == "Nmap") {
        // Mirror build_nmap_actions()'s row layout to map the tapped row to an action.
        const std::string dir = nmap_dir();
        const std::string selected = read_first_line(dir + "/selected");
        if (selected.empty()) {
            // Row 0 is "Scan" (runnable); rows >= 1 are discovered hosts.
            const auto hosts = read_lines(dir + "/hosts");
            const int host = item_index - 1;
            if (host >= 0 && host < static_cast<int>(hosts.size())) {
                write_file(dir + "/selected", hosts[static_cast<size_t>(host)]);
            }
        } else if (item_index == 0) {
            // Row 0 is "Back to host list".
            std::remove((dir + "/selected").c_str());
        }
        return;
    }

    if (label == "Wi-Fi") {
        // Evil Portal clone rows sit after the fixed prefix; row layout mirrors
        // build_portal_rows(): [scan][no-APs] / [scan][AP...] / [back][clone].
        const int static_count = static_cast<int>(build_wifi_static_actions().size());
        if (item_index < static_count) {
            return;
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
}

namespace {

// Shared sudo escalation for a non-root app: `sudo <flags> <command>`, where
// command is either a native re-exec of this binary (already authorized by the
// sudoers rule) or, for other root-needing rows, this binary's `--root-sh`
// passthrough. `flags` selects the authentication mode: `{"-n"}` for the
// passwordless rule, `{"-k", "-S", "-p", ""}` for the password prompt path.
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

bool action_needs_root(const ToolAction& action) {
    return !action.argv.empty() && geteuid() != 0 &&
           (action.argv[0] == self_exe_path() || action.needs_root);
}

bool sudo_passwordless() {
    if (geteuid() == 0) {
        return true;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::execlp("sudo", "sudo", "-n", "true", static_cast<char*>(nullptr));
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
    return escalated_argv(action, {"-k", "-S", "-p", ""});
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
    out.reserve(7);
    out.emplace_back("sudo");
    if (terminal_auth_mode() == TerminalAuth::Passwordless) {
        out.emplace_back("-n");
    } else {
        out.emplace_back("-k");
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
