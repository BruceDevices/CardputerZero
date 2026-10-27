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
void refresh_dynamic_categories();

// A ToolAction with an empty argv is a navigation row (not a launchable tool):
// selecting it mutates dynamic state instead of running a process. Screens route
// such a row here, then re-enter the same category to show the updated list.
// Safe no-op for categories/items that are not navigation rows.
void on_navigation_action(int category_index, int item_index);

// The command a screen should actually spawn for `action`. Normally this is
// just `action.argv`, but root-needing rows must run as root while the app is
// launched non-root by APPLaunch. Those are wrapped in `sudo -n` re-invoking
// this sudoers-authorized binary: native re-execs (`--evil-portal`, ...) run
// directly, everything else (shell scripts, responder) goes through the
// binary's `--root-sh` passthrough - one mechanism, one authorized binary, no
// broadened sudoers rule. External tools and already-root runs are returned
// unchanged.
std::vector<std::string> launch_argv(const ToolAction& action);

// True when launching `action` needs root at all (i.e. launch_argv would wrap
// it with sudo for a non-root app). Screens use it to pick between the
// passwordless fast path and the on-device sudo password prompt.
bool action_needs_root(const ToolAction& action);

// True if passwordless sudo works right now (`sudo -n true` exits 0: the
// sudoers.d rule is installed for this user). Uncached - a fork per root-tool
// launch is cheap and the rule can appear at any time.
bool sudo_passwordless();

// Like launch_argv, but for the password-prompt path: `sudo -k -S -p ''` with
// the same command tail. `-S` reads the password from stdin (ProcessRunner
// feeds it through a pipe; it never travels in argv or the log), `-k` forces a
// fresh authentication so the password line is always consumed predictably,
// `-p ''` keeps sudo's prompt out of the tool log.
std::vector<std::string> sudo_argv_with_password(const ToolAction& action);

} // namespace model
