/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "titlebar.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "bindings.h"
#include "device_status.h"
#include "theme.h"

namespace view::widgets {
namespace {

// Sysfs polling is cheap but pointless at 1 Hz; one status refresh every 5th tick.
constexpr uint32_t kDeviceRefreshTicks = 5;

// Fixed cluster width caps how far a long title can run before it scrolls.
constexpr int32_t kStatusClusterWidth = 120;
constexpr int32_t kClusterEdgeGap = 5;
// A centered title clears the cluster box only if it fits between two mirrored
// cluster gaps, so cap it to the middle span instead of eyeballing a margin.
constexpr int32_t kTitleMaxWidth = kScreenWidth - 2 * (kStatusClusterWidth + kClusterEdgeGap);

// LVGL ships one wifi glyph; strength is conveyed by the percent label instead.
constexpr const char* kWifiIcon = LV_SYMBOL_WIFI;

// Template's battery bucketing, mapped onto the five built-in LVGL glyphs.
const char* battery_icon(int32_t capacity_percent) {
    if (capacity_percent >= 95) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (capacity_percent >= 70) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (capacity_percent >= 35) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (capacity_percent >= 10) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

void configure_label(lv_obj_t* label, lv_subject_t* dark_mode_subject, int32_t width) {
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    reactive::bind_theme(label, dark_mode_subject, reactive::ThemeRole::Text);
}

void set_visible(lv_obj_t* obj, bool visible) {
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace

TitleBar::TitleBar(lv_obj_t* parent, viewmodel::BaseViewModel& view_model)
    : BaseWidgets(parent), view_model_(view_model) {}

TitleBar::~TitleBar() {
    if (refresh_timer_) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }
}

void TitleBar::build() {
    if (core_obj_) {
        return;
    }

    core_obj_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(core_obj_);
    lv_obj_set_size(core_obj_, LV_PCT(100), kTitleBarHeight);
    lv_obj_align(core_obj_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(core_obj_, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(core_obj_, view_model_.dark_mode_subject(), reactive::ThemeRole::Bar);

    auto* title = lv_label_create(core_obj_);
    lv_label_bind_text(title, view_model_.title_subject(), nullptr);
    lv_obj_set_width(title, kTitleMaxWidth);
    // Fixed width keeps a long title clear of the status cluster; it scrolls instead of clipping.
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    reactive::bind_theme(title, view_model_.dark_mode_subject(), reactive::ThemeRole::Text);

    auto* status = lv_obj_create(core_obj_);
    lv_obj_remove_style_all(status);
    lv_obj_set_size(status, kStatusClusterWidth, LV_PCT(100));
    lv_obj_align(status, LV_ALIGN_RIGHT_MID, -kClusterEdgeGap, 0);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    // END alignment keeps visible items pinned to the right edge as pieces hide.
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, 4, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);

    auto* dark_mode_subject = view_model_.dark_mode_subject();
    wifi_label_ = lv_label_create(status);
    configure_label(wifi_label_, dark_mode_subject, 16);
    lv_label_set_text(wifi_label_, kWifiIcon);
    wifi_percent_label_ = lv_label_create(status);
    configure_label(wifi_percent_label_, dark_mode_subject, 30);
    battery_icon_label_ = lv_label_create(status);
    configure_label(battery_icon_label_, dark_mode_subject, 32);
    battery_percent_label_ = lv_label_create(status);
    configure_label(battery_percent_label_, dark_mode_subject, 30);

    refresh_device_status();
    refresh_timer_ = lv_timer_create(refresh_timer_cb, 1000, this);
}

void TitleBar::refresh_device_status() {
    const auto status = platform::read_device_status();

    set_visible(wifi_label_, status.wifi_connected);
    set_visible(wifi_percent_label_, status.wifi_connected && status.wifi_strength_percent >= 0);
    if (status.wifi_connected) {
        std::array<char, 5> wifi_percent{};
        std::snprintf(wifi_percent.data(), wifi_percent.size(), "%d%%",
                      std::clamp(status.wifi_strength_percent, 0, 100));
        lv_label_set_text(wifi_percent_label_, wifi_percent.data());
    }

    set_visible(battery_icon_label_, status.battery_present);
    set_visible(battery_percent_label_, status.battery_present);
    if (status.battery_present) {
        std::array<char, 10> battery_text{};
        std::snprintf(battery_text.data(), battery_text.size(), "%s%s",
                      battery_icon(status.battery_percent),
                      status.battery_charging ? LV_SYMBOL_CHARGE : "");
        lv_label_set_text(battery_icon_label_, battery_text.data());
        std::array<char, 5> battery_percent{};
        std::snprintf(battery_percent.data(), battery_percent.size(), "%d%%",
                      std::clamp(status.battery_percent, 0, 100));
        lv_label_set_text(battery_percent_label_, battery_percent.data());
    }
}

void TitleBar::refresh_timer_cb(lv_timer_t* timer) {
    auto* title_bar = static_cast<TitleBar*>(lv_timer_get_user_data(timer));
    if (!title_bar) {
        return;
    }
    if (++title_bar->refresh_tick_ >= kDeviceRefreshTicks) {
        title_bar->refresh_tick_ = 0;
        title_bar->refresh_device_status();
    }
}

} // namespace view::widgets
