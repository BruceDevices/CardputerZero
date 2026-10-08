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

} // namespace model
