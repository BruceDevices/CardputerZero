/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <string>
#include <vector>

namespace model {

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
    // Opens the interactive settings screen instead of a tool list. Trailing so
    // the existing positional initializers (which stop at `actions`) still
    // compile; only the Config category sets it.
    bool is_config = false;
};

// Static catalog of every category/action the menu can show. Defined once in
// tool_catalog.cpp; add new tools there instead of touching any screen code.
const std::vector<ToolCategory>& tool_catalog();

} // namespace model
