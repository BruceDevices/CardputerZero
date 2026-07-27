/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "navbar.h"

#include "asset_manager.h"
#include "bindings.h"
#include "linux_input.h"
#include "theme.h"

namespace view::widgets {

NavBar::NavBar(lv_obj_t* parent, viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseWidgets(parent), view_model_(view_model), assets_(assets) {}

NavBar::~NavBar() {
    for (size_t i = 0; i < icon_buttons_.size(); ++i) {
        if (icon_buttons_[i]) {
            platform::unregister_nav_button(i, icon_buttons_[i]->root());
        }
    }
}

void NavBar::build() {
    if (core_obj_) {
        return;
    }

    core_obj_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(core_obj_);
    lv_obj_set_size(core_obj_, LV_PCT(100), kNavBarHeight);
    lv_obj_align(core_obj_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(core_obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(core_obj_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(core_obj_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(core_obj_, 8, 0);
    lv_obj_set_style_pad_right(core_obj_, 8, 0);
    reactive::bind_theme(core_obj_, view_model_.dark_mode_subject(), reactive::ThemeRole::Bar);

    const auto light_color = view::palette(false).primary;
    const auto dark_color  = view::palette(true).primary;
    auto* label_font       = assets_.load_font("inter-semibold.ttf", 12);

    for (size_t i = 0; i < icon_buttons_.size(); ++i) {
        icon_buttons_[i] = std::make_unique<IconButton>(core_obj_,
                                                        view_model_,
                                                        56,
                                                        24,
                                                        "",
                                                        label_font ? label_font : &lv_font_montserrat_12,
                                                        light_color,
                                                        dark_color,
                                                        nullptr,
                                                        nullptr,
                                                        true);
        icon_buttons_[i]->build();
        icon_buttons_[i]->set_enabled(false);
        platform::register_nav_button(i, icon_buttons_[i]->root());
    }
}

void NavBar::set_icon(size_t index, const char* text, lv_event_cb_t click_cb, void* user_data) {
    if (index >= icon_buttons_.size() || !icon_buttons_[index]) {
        return;
    }

    auto& button = icon_buttons_[index];
    button->set_text(text ? text : "");
    button->set_click_handler(click_cb, user_data);
    button->set_enabled(click_cb != nullptr);
}

void NavBar::set_text(size_t index, const char* text) {
    if (index >= icon_buttons_.size() || !icon_buttons_[index]) {
        return;
    }

    icon_buttons_[index]->set_text(text ? text : "");
}

void NavBar::set_enabled(size_t index, bool enabled) {
    if (index >= icon_buttons_.size() || !icon_buttons_[index]) {
        return;
    }

    icon_buttons_[index]->set_enabled(enabled);
}

} // namespace view::widgets
