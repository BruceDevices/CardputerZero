/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "menu_screen.h"

#include "asset_manager.h"
#include "tool_catalog.h"
#include "ui_const.h"

namespace screen {
namespace {

constexpr int32_t kHeaderHeight = 20;

} // namespace

MenuScreen::MenuScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void MenuScreen::build_content(lv_obj_t* content) {
    category_index_ = view_model().active_category();
    const auto& catalog = model::tool_catalog();

    auto* header = lv_label_create(content);
    lv_obj_set_size(header, LV_PCT(100), kHeaderHeight);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_label_set_text(header,
                      category_index_ == -1 ? "Main Menu" : catalog[static_cast<size_t>(category_index_)].label);

    auto* list = lv_list_create(content);
    lv_obj_set_size(list, LV_PCT(100), view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight - kHeaderHeight);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    const size_t item_count = category_index_ == -1
                                  ? catalog.size()
                                  : catalog[static_cast<size_t>(category_index_)].actions.size();
    rows_.reserve(item_count);
    for (size_t i = 0; i < item_count; ++i) {
        const char* label = category_index_ == -1 ? catalog[i].label
                                                   : catalog[static_cast<size_t>(category_index_)].actions[i].label;
        auto* row = lv_list_add_button(list, nullptr, label);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        rows_.push_back(row);
    }

    apply_selection_highlight();

    nav_bar().set_icon(1, view::NAV_UP, move_up_cb, this);
    nav_bar().set_icon(2, view::NAV_SELECT, select_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, move_down_cb, this);

    if (category_index_ == -1) {
        configure_common_nav(view::NAV_QUIT, quit_cb);
    }
    else {
        configure_common_nav(view::NAV_BACK, back_cb);
    }
}

void MenuScreen::move_selection(int delta) {
    if (rows_.empty()) {
        return;
    }

    const int count = static_cast<int>(rows_.size());
    int next = (static_cast<int>(selected_index_) + delta) % count;
    if (next < 0) {
        next += count;
    }

    selected_index_ = static_cast<size_t>(next);
    apply_selection_highlight();
}

void MenuScreen::apply_selection_highlight() {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (i == selected_index_) {
            lv_obj_add_state(rows_[i], LV_STATE_CHECKED);
            lv_obj_scroll_to_view(rows_[i], LV_ANIM_OFF);
        }
        else {
            lv_obj_remove_state(rows_[i], LV_STATE_CHECKED);
        }
    }
}

void MenuScreen::open_selected() {
    if (selected_index_ >= rows_.size()) {
        return;
    }

    if (category_index_ == -1) {
        const auto& category = model::tool_catalog()[selected_index_];
        if (category.info_only) {
            view_model().show_about();
        }
        else {
            view_model().show_menu(static_cast<int>(selected_index_));
        }
    }
    else {
        view_model().show_tool_run(category_index_, static_cast<int>(selected_index_));
    }
}

void MenuScreen::move_up_cb(lv_event_t* event) {
    auto* self = static_cast<MenuScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->move_selection(-1);
    }
}

void MenuScreen::move_down_cb(lv_event_t* event) {
    auto* self = static_cast<MenuScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->move_selection(1);
    }
}

void MenuScreen::select_cb(lv_event_t* event) {
    auto* self = static_cast<MenuScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->open_selected();
    }
}

void MenuScreen::quit_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->request_quit();
    }
}

void MenuScreen::back_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->go_back();
    }
}

} // namespace screen
