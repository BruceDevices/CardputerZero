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
namespace {
constexpr int32_t kButtonWidth     = 56;
constexpr int32_t kKeyPitch        = 57;
constexpr int32_t kFirstKeyCenterX = 46;
} // namespace

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
    reactive::bind_theme(core_obj_, view_model_.dark_mode_subject(), reactive::ThemeRole::Bar);

    const auto light_color = view::palette(false).primary;
    const auto dark_color  = view::palette(true).primary;
    auto* label_font       = assets_.load_font("inter-semibold.ttf", 12);

    for (size_t i = 0; i < icon_buttons_.size(); ++i) {
        icon_buttons_[i] = std::make_unique<IconButton>(core_obj_,
                                                        view_model_,
                                                        kButtonWidth,
                                                        24,
                                                        "",
                                                        label_font ? label_font : &lv_font_montserrat_12,
                                                        light_color,
                                                        dark_color,
                                                        nullptr,
                                                        nullptr,
                                                        true);
        icon_buttons_[i]->build();
        // Absolute x, not flex/padding: ThemeRole::Bar resets the bar's padding
        // on every theme apply. Each label is centered over its physical key
        // (4 5 6 7 8), which sit ~57 px apart at x = 46..274 on the panel.
        lv_obj_align(icon_buttons_[i]->root(), LV_ALIGN_LEFT_MID,
                     kFirstKeyCenterX + static_cast<int32_t>(i) * kKeyPitch - kButtonWidth / 2, 0);
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
