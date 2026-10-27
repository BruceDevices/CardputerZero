/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "clock_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "theme.h"
#include "ui_const.h"

#include <ctime>

namespace screen {

ClockScreen::ClockScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

ClockScreen::~ClockScreen() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void ClockScreen::build_content(lv_obj_t* content) {
    auto* big_font = assets().load_font("inter-bold.ttf", 56);
    auto* date_font = assets().load_font("inter-medium.ttf", 14);

    // Big HH:MM in the accent hue (same in light/dark, like the menu carousel
    // icons), centered just above the middle of the content area.
    time_label_ = lv_label_create(content);
    lv_obj_set_style_text_font(time_label_, big_font ? big_font : &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(time_label_, view::palette(view_model().is_dark_mode()).primary, 0);
    lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, -12);

    // Weekday + date underneath; follows the text theme so it flips with dark mode.
    date_label_ = lv_label_create(content);
    lv_obj_set_style_text_font(date_label_, date_font ? date_font : &lv_font_montserrat_14, 0);
    lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, 28);
    reactive::bind_theme(date_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    update();
    timer_ = lv_timer_create(tick_cb, 1000, this);

    nav_bar().set_icon(1, nullptr, nullptr, nullptr);
    nav_bar().set_icon(2, nullptr, nullptr, nullptr);
    nav_bar().set_icon(3, nullptr, nullptr, nullptr);
    configure_common_nav(view::NAV_BACK, back_cb);
}

void ClockScreen::update() {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_r(&t, &lt);

    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &lt);
    lv_label_set_text(time_label_, buf);

    char date[64];
    std::strftime(date, sizeof(date), "%A, %d %B %Y", &lt);
    lv_label_set_text(date_label_, date);
}

void ClockScreen::tick_cb(lv_timer_t* timer) {
    auto* self = static_cast<ClockScreen*>(lv_timer_get_user_data(timer));
    if (self) {
        self->update();
    }
}

void ClockScreen::back_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->go_back();
    }
}

} // namespace screen
