/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "about_screen.h"

#include "asset_manager.h"
#include "ui_const.h"

// Injected by CMake from project() version (same define the title bar uses).
#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace screen {

AboutScreen::AboutScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void AboutScreen::build_content(lv_obj_t* content) {
    // Scrollable box so the text is reachable on the short panel; Up/Down (nav
    // slots 1/3) page through it, matching the tool-run and terminal logs.
    scroll_ = lv_obj_create(content);
    lv_obj_set_size(scroll_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(scroll_, 8, 0);
    lv_obj_set_style_border_width(scroll_, 0, 0);
    lv_obj_add_flag(scroll_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_, LV_SCROLLBAR_MODE_AUTO);

    auto* label = lv_label_create(scroll_);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label,
                      "Bruce CardputerZero\n"
                      "Version " APP_VERSION "\n\n"
                      "A security-testing toolkit UI that launches curl, "
                      "netcat, iw, bluetoothctl, smbclient and Responder "
                      "already installed on this device.\n\n"
                      "For authorized security testing, CTF and research "
                      "use only.");

    nav_bar().set_icon(1, view::NAV_UP, scroll_up_cb, this);
    nav_bar().set_icon(2, nullptr, nullptr, nullptr);
    nav_bar().set_icon(3, view::NAV_DOWN, scroll_down_cb, this);
    configure_common_nav(view::NAV_BACK, back_cb);
}

void AboutScreen::scroll(int32_t delta) {
    if (scroll_) {
        lv_obj_scroll_by(scroll_, 0, delta, LV_ANIM_ON);
    }
}

void AboutScreen::scroll_up_cb(lv_event_t* event) {
    auto* self = static_cast<AboutScreen*>(lv_event_get_user_data(event));
    if (self && self->scroll_) {
        self->scroll(lv_obj_get_height(self->scroll_) * 3 / 4);
    }
}

void AboutScreen::scroll_down_cb(lv_event_t* event) {
    auto* self = static_cast<AboutScreen*>(lv_event_get_user_data(event));
    if (self && self->scroll_) {
        self->scroll(-lv_obj_get_height(self->scroll_) * 3 / 4);
    }
}

void AboutScreen::back_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->go_back();
    }
}

} // namespace screen
