/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "titlebar.h"

#include "bindings.h"
#include "theme.h"

namespace view::widgets {

TitleBar::TitleBar(lv_obj_t* parent, viewmodel::BaseViewModel& view_model)
    : BaseWidgets(parent), view_model_(view_model) {}

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
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    reactive::bind_theme(title, view_model_.dark_mode_subject(), reactive::ThemeRole::Text);
}

} // namespace view::widgets
