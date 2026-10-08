/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "theme.h"

#include "tool_catalog.h"
#include "ui_const.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace view {
namespace {

int g_accent = 0; // index into model::accent_presets()

int accent_count() {
    return static_cast<int>(model::accent_presets().size());
}

std::string accent_conf_path() {
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::string(home) + "/.bruce/ui.conf";
    }
    return "/root/.bruce/ui.conf";
}

// The accent only recolors `primary`/`info`; body/card/text stay per dark/light
// mode so the Bruce black-canvas look is preserved whatever accent is picked.
void accent_colors(uint32_t& primary, uint32_t& info) {
    const auto& presets = model::accent_presets();
    const int idx = (g_accent >= 0 && g_accent < static_cast<int>(presets.size())) ? g_accent : 0;
    primary = presets[static_cast<size_t>(idx)].primary;
    info = presets[static_cast<size_t>(idx)].info;
}

} // namespace

ThemePalette palette(bool dark_mode) {
    uint32_t primary = 0;
    uint32_t info = 0;
    accent_colors(primary, info);

    if (dark_mode) {
        return {
            lv_color_hex(color::dark::kBody),
            lv_color_hex(color::dark::kCard),
            lv_color_hex(color::dark::kAction),
            lv_color_hex(color::dark::kButton),
            lv_color_hex(color::dark::kBorder),
            lv_color_hex(color::dark::kTextPrimary),
            lv_color_hex(color::dark::kTextDisabled),
            lv_color_hex(primary),
            lv_color_hex(info),
        };
    }

    return {
        lv_color_hex(color::light::kBody),
        lv_color_hex(color::light::kCard),
        lv_color_hex(color::light::kAction),
        lv_color_hex(color::light::kButton),
        lv_color_hex(color::light::kBorder),
        lv_color_hex(color::light::kTextPrimary),
        lv_color_hex(color::light::kTextDisabled),
        lv_color_hex(primary),
        lv_color_hex(info),
    };
}

int accent_index() { return g_accent; }

void set_accent(int index) {
    if (index < 0 || index >= accent_count()) {
        return;
    }
    g_accent = index;
    // Best-effort persist: ensure ~/.bruce exists, then write the index. Ignore
    // failures - the accent still applies this session, it just won't survive a
    // restart.
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        mkdir((std::string(home) + "/.bruce").c_str(), 0755);
    }
    std::ofstream(accent_conf_path()) << g_accent << "\n";
}

void load_accent() {
    std::ifstream in(accent_conf_path());
    int idx = 0;
    if (in >> idx && idx >= 0 && idx < accent_count()) {
        g_accent = idx;
    }
}

void apply_lvgl_theme(lv_display_t* display, bool dark_mode) {
    if (!display) {
        return;
    }

    const auto colors = palette(dark_mode);
    lv_theme_t* theme = lv_theme_default_init(display, colors.primary, colors.info, dark_mode, LV_FONT_DEFAULT);
    lv_display_set_theme(display, theme);
}

} // namespace view
