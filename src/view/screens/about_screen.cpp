/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "about_screen.h"

#include "asset_manager.h"
#include "ui_const.h"

namespace screen {

AboutScreen::AboutScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void AboutScreen::build_content(lv_obj_t* content) {
    auto* label = lv_label_create(content);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_all(label, 10, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label,
                      "Bruce CardputerZero\n"
                      "Version 0.1.0\n\n"
                      "A security-testing toolkit UI that launches curl, "
                      "netcat, iw, bluetoothctl, smbclient and Responder "
                      "already installed on this device.\n\n"
                      "For authorized security testing, CTF and research "
                      "use only.");

    nav_bar().set_icon(1, nullptr, nullptr, nullptr);
    nav_bar().set_icon(2, nullptr, nullptr, nullptr);
    nav_bar().set_icon(3, nullptr, nullptr, nullptr);
    configure_common_nav(view::NAV_BACK, back_cb);
}

void AboutScreen::back_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->go_back();
    }
}

} // namespace screen
