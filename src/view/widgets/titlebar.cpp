/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "titlebar.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>

#include <unistd.h>

#include "bindings.h"
#include "device_status.h"
#include "gps_session.h"
#include "theme.h"

// Injected by CMake (target_compile_definitions) from the project() version.
#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace view::widgets {
namespace {

// Sysfs polling is cheap but pointless at 1 Hz; one status refresh every 5th tick.
constexpr uint32_t kDeviceRefreshTicks = 5;

// Status cluster width; gps/wifi/battery pin to the right edge inside it.
constexpr int32_t kStatusClusterWidth = 140;
constexpr int32_t kClusterEdgeGap = 5;

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

// Pick up a timezone change made at runtime (the "Set timezone" picker runs
// `timedatectl set-timezone`, which rewrites /etc/localtime). glibc caches the
// zone, so we must re-read the current zone name and re-apply it via TZ+tzset;
// the name only changes when the user actually switches zones, so the file read
// each tick is cheap and tzset only re-parses on a real change.
std::string current_zone_name() {
    // /etc/localtime is authoritative: timedatectl always repoints this symlink,
    // whereas /etc/timezone is frequently left stale on systemd/Debian (that
    // staleness showed the wrong UTC offset). Resolve it (realpath follows
    // relative / multi-hop links) and take the part after ".../zoneinfo/".
    if (char* resolved = ::realpath("/etc/localtime", nullptr)) {
        const std::string path(resolved);
        std::free(resolved);
        const std::string marker = "/zoneinfo/";
        const auto pos = path.find(marker);
        if (pos != std::string::npos) {
            return path.substr(pos + marker.size());
        }
    }
    // Fallback: /etc/localtime is a plain copy, not a symlink -> the name file.
    std::ifstream tz("/etc/timezone");
    std::string name;
    if (tz && std::getline(tz, name)) {
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
            name.pop_back();
        }
        if (!name.empty()) {
            return name;
        }
    }
    return {};
}

void sync_timezone() {
    static std::string applied;
    const std::string zone = current_zone_name();
    if (zone.empty() || zone == applied) {
        return;
    }
    applied = zone;
    ::setenv("TZ", zone.c_str(), 1);
    ::tzset();
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

    // Clock on the left. When the system clock is unset (no RTC/NTP) it falls
    // back to the app version string. The app name used to scroll in the centre
    // as a marquee, which read poorly, so the bar is now clock | status cluster.
    clock_label_ = lv_label_create(core_obj_);
    lv_obj_set_style_text_font(clock_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(clock_label_, LV_ALIGN_LEFT_MID, kClusterEdgeGap + 3, 0);
    reactive::bind_theme(clock_label_, view_model_.dark_mode_subject(), reactive::ThemeRole::Text);
    update_clock();

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
    // Bruce's status bar draws the GPS icon left of Wi-Fi, only while a GPS
    // feature runs and has received data (gpsConnected).
    gps_label_ = lv_label_create(status);
    configure_label(gps_label_, dark_mode_subject, 16);
    lv_label_set_text(gps_label_, LV_SYMBOL_GPS);
    refresh_gps();
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

void TitleBar::update_clock() {
    if (!clock_label_) {
        return;
    }
    sync_timezone(); // reflect a runtime timezone switch before converting
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    // A pre-2024 system time means the clock was never set (devices without an
    // RTC/NTP boot near the epoch); show the app version instead of a bogus time.
    if (lt.tm_year + 1900 >= 2024) {
        char buf[8];
        std::strftime(buf, sizeof(buf), "%H:%M", &lt);
        lv_label_set_text(clock_label_, buf);
    } else {
        lv_label_set_text(clock_label_, APP_VERSION);
    }
}

void TitleBar::refresh_gps() {
    if (gps_label_) {
        set_visible(gps_label_, platform::gps::connected());
    }
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
    title_bar->update_clock(); // cheap; keep the minute digit fresh every tick
    title_bar->refresh_gps();  // an atomic load; no extra timer or wake-up
    if (++title_bar->refresh_tick_ >= kDeviceRefreshTicks) {
        title_bar->refresh_tick_ = 0;
        title_bar->refresh_device_status();
    }
}

} // namespace view::widgets
