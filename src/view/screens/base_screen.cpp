/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "base_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "theme.h"
#include "ui_const.h"

namespace screen {

BaseScreen::BaseScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : view_model_(view_model), assets_(assets) {}

BaseScreen::~BaseScreen() {
    if (theme_label_observer_) {
        lv_observer_remove(theme_label_observer_);
        theme_label_observer_ = nullptr;
    }

    title_bar_.reset();
    nav_bar_.reset();

    if (root_ && lv_obj_is_valid(root_)) {
        lv_obj_delete(root_);
    }
}

void BaseScreen::init() {
    if (root_) {
        return;
    }

    root_ = lv_obj_create(nullptr);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, view::kScreenWidth, view::kScreenHeight);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(root_, view_model_.dark_mode_subject(), reactive::ThemeRole::Screen);

    title_bar_ = std::make_unique<view::widgets::TitleBar>(root_, view_model_);
    title_bar_->build();

    nav_bar_ = std::make_unique<view::widgets::NavBar>(root_, view_model_, assets_);
    nav_bar_->build();

    content_ = lv_obj_create(root_);
    lv_obj_remove_style_all(content_);
    lv_obj_set_size(content_, LV_PCT(100), view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight);
    lv_obj_align(content_, LV_ALIGN_TOP_MID, 0, view::kTitleBarHeight);
    lv_obj_clear_flag(content_, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(content_, view_model_.dark_mode_subject(), reactive::ThemeRole::Surface);

    build_content(content_);
}

lv_obj_t* BaseScreen::root() const {
    return root_;
}

viewmodel::BaseViewModel& BaseScreen::view_model() {
    return view_model_;
}

app::AssetManager& BaseScreen::assets() {
    return assets_;
}

view::widgets::NavBar& BaseScreen::nav_bar() {
    return *nav_bar_;
}

void BaseScreen::configure_common_nav(const char* back_text, lv_event_cb_t back_cb, void* back_user_data) {
    nav_bar_->set_icon(0, back_text, back_cb, back_user_data ? back_user_data : &view_model_);
    nav_bar_->set_icon(4,
                       view_model_.is_dark_mode() ? view::NAV_LIGHT : view::NAV_DARK,
                       theme_toggle_cb,
                       &view_model_);

    if (!theme_label_observer_) {
        theme_label_observer_ =
            reactive::observe_obj(root_, view_model_.dark_mode_subject(), theme_label_observer_cb, this);
    }
}

void BaseScreen::theme_toggle_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->toggle_dark_mode();
    }
}

void BaseScreen::theme_label_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* self = static_cast<BaseScreen*>(lv_observer_get_user_data(observer));
    if (!self || !self->nav_bar_) {
        return;
    }

    self->nav_bar_->set_text(4, lv_subject_get_int(subject) ? view::NAV_LIGHT : view::NAV_DARK);
}

} // namespace screen
