/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */



#pragma once

#include <cstdint>

namespace view {

// Phosphor icon glyphs (verified codepoints only - the bundled Phosphor-Fill.ttf
// exposes glyph names as generic uniXXXX, so only reuse codepoints already
// confirmed by the upstream template rather than guessing new ones).
constexpr const char* ICON_SIGN_OUT = "\uE42A";
constexpr const char* ICON_MOON     = "\uE330";
constexpr const char* ICON_SUN      = "\uE474";
constexpr const char* ICON_INFO     = "\uE2CE";

// Nav bar action labels. Plain text in the Inter UI font reads reliably at
// this resolution and needs no icon-font codepoint lookup, keeping the
// 5-button bottom bar self-explanatory without a legend.
constexpr const char* NAV_BACK   = "Back";
constexpr const char* NAV_QUIT   = "Quit";
constexpr const char* NAV_UP     = "Up";
constexpr const char* NAV_DOWN   = "Down";
constexpr const char* NAV_SELECT = "Select";
constexpr const char* NAV_RUN    = "Run";
constexpr const char* NAV_STOP   = "Stop";
constexpr const char* NAV_CLEAR  = "Clear";
constexpr const char* NAV_LIGHT  = "Light";
constexpr const char* NAV_DARK   = "Dark";

namespace color {

// Bruce-style purple/black UI, flattened to solid RGB values for LVGL.
namespace light {
constexpr uint32_t kPrimary      = 0x8a2be2;
constexpr uint32_t kInfo         = 0xb026ff;
constexpr uint32_t kBody         = 0xf5f2fb;
constexpr uint32_t kCard         = 0xffffff;
constexpr uint32_t kAction       = 0xece4fb;
constexpr uint32_t kButton       = 0xf0eaf9;
constexpr uint32_t kBorder       = 0xd8c7f2;
constexpr uint32_t kTextPrimary  = 0x1c0f2b;
constexpr uint32_t kTextDisabled = 0xb3a6c9;
} // namespace light

namespace dark {
constexpr uint32_t kPrimary      = 0xb026ff;
constexpr uint32_t kInfo         = 0xd48bff;
constexpr uint32_t kBody         = 0x0a0612;
constexpr uint32_t kCard         = 0x150c24;
constexpr uint32_t kAction       = 0x0d0818;
constexpr uint32_t kButton       = 0x1c1030;
constexpr uint32_t kBorder       = 0x3a2a52;
constexpr uint32_t kTextPrimary  = 0xe9defc;
constexpr uint32_t kTextDisabled = 0x5c4e73;
} // namespace dark

} // namespace color

} // namespace view
