/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace model {

enum class RfTool { None, Scan, ProtocolMonitor, Record, Spectrum, Spectrogram };

// Which structured Wi-Fi scan UI a row opens instead of the raw tool-run log.
// ScanList  -> "Scan nearby networks": a signal-sorted, selectable network list
//              on the left with the selected network's details on the right.
// SsidList  -> "List SSIDs + signal": a selectable "SSID (RSSI)" list; picking
//              one runs a targeted deeper scan and shows it on a detail screen.
enum class WifiView { None, ScanList, SsidList };

// Which Bruce-firmware GPS feature a row opens on the shared GpsAppScreen
// (in-app serial NMEA reader, no gpsd). Tracker = Bruce's "GPS Tracker" (GPX).
enum class GpsApp { None, Tracker };

// A selectable UI accent color. Pure data (no LVGL) so it is the single source
// of truth shared by the "Theme" menu (model) and the palette (view): `primary`
// is the main accent, `info` a lighter companion used for secondary styling.
struct AccentPreset {
    const char* name;
    uint32_t primary;
    uint32_t info;
};

// The accent presets, in menu order. Index into this list is what the Theme
// category selects and what view::set_accent() persists.
const std::vector<AccentPreset>& accent_presets();

// One launchable action inside a category (e.g. "Scan nearby networks").
// `argv` is executed directly with execvp, never through a shell string built
// from user input, so there is no command-injection surface even though the
// commands themselves are security-testing tools.
struct ToolAction {
    const char* label;
    const char* description;
    std::vector<std::string> argv;
    // The row must run as root (hostapd, iw scan, responder, ...). Screens
    // spawn `model::launch_argv(action)` which escalates such rows; see the
    // launch_argv notes in tool_catalog.cpp.
    bool needs_root = false;
    // Open a dedicated full-screen widget instead of the tool-run log. The only
    // one today is the themed clock face (ClockScreen); `argv` is still kept so
    // the row degrades to a normal `date` run if the widget is ever dropped.
    bool clock_view = false;
    // Open the interactive TerminalScreen instead of the tool-run log. Both
    // "Terminal" rows open the same screen; root is chosen per typed command
    // (leading "sudo "), not per row. `argv` is a harmless fallback print used
    // only if the screen is ever dropped.
    bool terminal_view = false;
    // Run the command automatically on screen entry instead of waiting for the
    // operator to press Run. Used for the read-only Wi-Fi recon rows so they
    // behave autonomously; left false for attacks and mode toggles.
    bool auto_run = false;
    RfTool rf_tool = RfTool::None;
    // Non-None opens the dedicated WifiScanScreen (structured list/detail view)
    // instead of the raw tool-run log. `argv` is still the scan command it runs.
    WifiView wifi_view = WifiView::None;
    // Open the dedicated BleScanScreen (firmware-style device list + information
    // view) instead of the raw tool-run log. `argv` is the fallback scan command.
    bool ble_scan_view = false;
    // Non-None opens the shared GpsAppScreen running that Bruce GPS feature
    // instead of the tool-run log. `argv` is only a harmless fallback print.
    GpsApp gps_app = GpsApp::None;
};

// A menu category shown on the root menu (e.g. "Wi-Fi", "Responder").
// `info_only` categories (About) have no actions and open a static screen
// instead of a tool-run screen.
struct ToolCategory {
    const char* label;
    const char* description;
    const char* icon; // Phosphor-Fill glyph shown in the main-menu carousel (see ui_const.h)
    bool info_only;
    std::vector<ToolAction> actions;
};

// Static catalog of every category/action the menu can show. Defined once in
// tool_catalog.cpp; add new tools there instead of touching any screen code.
const std::vector<ToolCategory>& tool_catalog();

// Recompute the action list of any data-driven category (currently just "Nmap",
// whose rows depend on discovered hosts). Call before building a category's menu
// so freshly discovered hosts / the current selection show up. Cheap; a no-op
// for static categories.
// `category_index` lets hardware-backed categories refresh only when entered;
// pass -1 to refresh all purely data-driven categories without probing hardware.
void refresh_dynamic_categories(int category_index = -1);

// Per-user state dir of the Nmap guided flow (/tmp/bruce-nmap-<uid>), holding
// `hosts` (discovered IPs) and `selected` (the picked host).
std::string nmap_state_dir();

// Rebuild the native CC1101 rows plus the saved-RAW replay list. The view owns
// hardware probing and passes the resulting availability/pins into the model.
void refresh_rf_category(bool cc1101_present, int cs_gpio, int gdo0_gpio);
std::string rf_recordings_dir();

// A ToolAction with an empty argv is a navigation row (not a launchable tool):
// selecting it mutates dynamic state instead of running a process. Screens route
// such a row here, then re-enter the same category to show the updated list.
// Safe no-op for categories/items that are not navigation rows.
void on_navigation_action(int category_index, int item_index);

// Back key (nav slot 0 / key 4 / ESC) pressed on a category's tool list.
// Data-driven categories with an in-place drill-down undo one level here,
// like Bruce's ESC in a sub-menu: Nmap host view -> host list, Wi-Fi Evil
// Portal clone view -> AP list, System / Clock timezone picker -> utilities.
// Returns true if it changed state (the screen should rebuild the same
// category), false if there is nothing to undo (leave the category as usual).
bool on_category_back(int category_index);

// The command a screen should actually spawn for `action`. Normally this is
// just `action.argv`, but root-needing rows must run as root while the app is
// launched non-root by APPLaunch. Those are wrapped in `sudo -n` re-invoking
// this sudoers-authorized binary: native re-execs (`--evil-portal`, ...) run
// directly, everything else (shell scripts, responder) goes through the
// binary's `--root-sh` passthrough - one mechanism, one authorized binary, no
// broadened sudoers rule. External tools and already-root runs are returned
// unchanged.
std::vector<std::string> launch_argv(const ToolAction& action);

// A human-readable one-line form of the command a row runs, for the
// "Running command: "<...>", wait..." status line. For `sh -c "<script>"` rows
// it returns the script; otherwise the argv joined by spaces.
std::string action_command_text(const ToolAction& action);

// The active wireless interface name. "wlan0" only exists on the CardputerZero;
// on other hosts the Wi-Fi NIC is named differently (e.g. "wlp0s20f3"), so the
// Wi-Fi tools must not hardcode it. A netdev is wireless iff it has a phy80211
// link under /sys/class/net. Prefers "wlan0" when present (the device), else the
// first wireless interface, else falls back to "wlan0". This is the single
// source of truth every Wi-Fi command resolves its interface through.
std::string wifi_interface();

// True when launching `action` needs root at all (i.e. launch_argv would wrap
// it with sudo for a non-root app). Screens use it to pick between the
// passwordless fast path and the on-device sudo password prompt.
bool action_needs_root(const ToolAction& action);

// True if passwordless sudo works right now (`sudo -n true` exits 0: the
// sudoers.d rule is installed for this user). Uncached - a fork per root-tool
// launch is cheap and the rule can appear at any time.
bool sudo_passwordless();

// Like launch_argv, but for the password-prompt path: `sudo -S -p ''` with
// the same command tail. `-S` reads the password from stdin (ProcessRunner
// feeds it through a pipe; it never travels in argv or the log), `-p ''` keeps
// sudo's prompt out of the tool log. No `-k`: a successful run leaves sudo's
// timestamp cached, so the next sudo_passwordless() probe passes and later
// launches take the `sudo -n` path without asking again.
std::vector<std::string> sudo_argv_with_password(const ToolAction& action);

// App-wide sudo password cache. Needed because sudo's own credential cache
// does not carry over between our tty-less launches (every child is setsid()ed,
// and sudo keys tickets per tty/session), so without this every root command
// asked again. Entered once, reused by every screen; it expires after
// kSudoCacheIdleSeconds without use (sudo's default timeout) and is wiped
// as soon as sudo rejects it. Memory only, never written anywhere.
constexpr int kSudoCacheIdleSeconds = 15 * 60;
// The cached password, or "" when none/expired. A hit counts as a use and
// restarts the idle timer.
std::string sudo_cached_password();
void sudo_remember_password(const std::string& password);
void sudo_forget_password();
// sudo -S output that means the password was wrong.
bool sudo_output_rejected_password(const std::string& output);

// --- Terminal ("Terminal" category) ------------------------------------------------

// How a root terminal command must be launched right now. NotNeeded: the app
// itself is root (euid == 0), so commands run directly. Passwordless: the
// /etc/sudoers.d rule covers this binary, `sudo -n <self> --root-sh` works.
// Password: no rule - the command needs `sudo -S` plus the user's password
// on stdin (TerminalScreen caches it for the session, ToolRunScreen asks once
// per run - same split as the catalog rows).
enum class TerminalAuth { NotNeeded, Passwordless, Password };

TerminalAuth terminal_auth_mode();

// Wrap a terminal command line for execution. User commands (and root commands
// when the app is already root) run as `sh -c <command>`; root commands run
// through the same sudoers-authorized binary and its `--root-sh` passthrough
// as every root catalog row - one mechanism, no broadened rule. Note the rule
// authorizes the binary with ANY argv, so user-typed terminal commands ride it
// without widening the attack surface beyond what the rule already grants.
std::vector<std::string> terminal_command_argv(const std::string& command, bool as_root);

} // namespace model
