/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "menu_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "theme.h"
#include "tool_catalog.h"
#include "ui_const.h"

namespace screen {
namespace {

constexpr int32_t kHeaderHeight = 20;

// The Bruce accent for carousel icons / selection pill. Follows the user's
// chosen theme color (view::palette reads the current accent); same hue in
// light/dark, so the icons only need a rebuild on accent change, not per theme.
inline lv_color_t bruce_accent() {
    return view::palette(true).primary;
}

} // namespace

MenuScreen::MenuScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void MenuScreen::build_content(lv_obj_t* content) {
    category_index_ = view_model().active_category();
    carousel_ = category_index_ == -1;

    if (carousel_) {
        build_carousel(content);
    }
    else {
        build_list(content);
    }
}

void MenuScreen::build_carousel(lv_obj_t* content) {
    auto* icon_big   = assets().load_font("Phosphor-Fill.ttf", 60);
    auto* icon_small = assets().load_font("Phosphor-Fill.ttf", 30);
    auto* name_font  = assets().load_font("inter-bold.ttf", 18);

    // Dim neighbour icons flanking the big centred one - the classic Bruce look.
    icon_prev_ = lv_label_create(content);
    icon_next_ = lv_label_create(content);
    for (auto* side : {icon_prev_, icon_next_}) {
        lv_obj_set_style_text_font(side, icon_small ? icon_small : &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(side, bruce_accent(), 0);
        lv_obj_set_style_text_opa(side, LV_OPA_30, 0);
    }
    lv_obj_align(icon_prev_, LV_ALIGN_LEFT_MID, 6, -8);
    lv_obj_align(icon_next_, LV_ALIGN_RIGHT_MID, -6, -8);

    icon_curr_ = lv_label_create(content);
    lv_obj_set_style_text_font(icon_curr_, icon_big ? icon_big : &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon_curr_, bruce_accent(), 0);
    lv_obj_align(icon_curr_, LV_ALIGN_TOP_MID, 0, 2);

    // Main menu shows just the big icon + category name - no description text,
    // for a clean Bruce-style carousel. Per-tool help still shows on the tool
    // list / run screens via each action's description.
    name_label_ = lv_label_create(content);
    lv_obj_set_style_text_font(name_label_, name_font ? name_font : &lv_font_montserrat_18, 0);
    lv_obj_align(name_label_, LV_ALIGN_TOP_MID, 0, 74);
    reactive::bind_theme(name_label_, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    update_carousel();

    nav_bar().set_icon(1, view::NAV_PREV, move_up_cb, this);
    nav_bar().set_icon(2, view::NAV_SELECT, select_cb, this);
    nav_bar().set_icon(3, view::NAV_NEXT, move_down_cb, this);
    // Root menu: ESC/back is a no-op here (nothing to go back to). Previously it
    // was bound to quit_cb, so pressing back at the top level exited the app and
    // looked like a crash on the framebuffer. Keeps the theme toggle on slot 4.
    configure_common_nav("", nullptr);
}

void MenuScreen::update_carousel() {
    const auto& catalog = model::tool_catalog();
    const int count = static_cast<int>(catalog.size());
    if (count == 0) {
        return;
    }

    const int curr = static_cast<int>(selected_index_);
    const int prev = (curr - 1 + count) % count;
    const int next = (curr + 1) % count;

    lv_label_set_text(icon_curr_, catalog[static_cast<size_t>(curr)].icon);
    lv_label_set_text(icon_prev_, catalog[static_cast<size_t>(prev)].icon);
    lv_label_set_text(icon_next_, catalog[static_cast<size_t>(next)].icon);
    lv_label_set_text(name_label_, catalog[static_cast<size_t>(curr)].label);
}

void MenuScreen::build_list(lv_obj_t* content) {
    // Rebuild data-driven categories (e.g. Nmap's host list) before reading them,
    // so a just-finished discovery / host selection shows up on (re)entry.
    model::refresh_dynamic_categories();
    const auto& catalog = model::tool_catalog();

    auto* header = lv_label_create(content);
    lv_obj_set_size(header, LV_PCT(100), kHeaderHeight);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_label_set_text(header, catalog[static_cast<size_t>(category_index_)].label);

    auto* list = lv_list_create(content);
    lv_obj_set_size(list, LV_PCT(100), view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight - kHeaderHeight);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    const size_t item_count = catalog[static_cast<size_t>(category_index_)].actions.size();
    rows_.reserve(item_count);
    for (size_t i = 0; i < item_count; ++i) {
        const char* label = catalog[static_cast<size_t>(category_index_)].actions[i].label;
        auto* row = lv_list_add_button(list, nullptr, label);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        rows_.push_back(row);
    }

    apply_selection_highlight();

    nav_bar().set_icon(1, view::NAV_UP, move_up_cb, this);
    nav_bar().set_icon(2, view::NAV_SELECT, select_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, move_down_cb, this);
    configure_common_nav(view::NAV_BACK, back_cb);
}

void MenuScreen::move_selection(int delta) {
    const int count = carousel_ ? static_cast<int>(model::tool_catalog().size())
                                : static_cast<int>(rows_.size());
    if (count == 0) {
        return;
    }

    int next = (static_cast<int>(selected_index_) + delta) % count;
    if (next < 0) {
        next += count;
    }

    selected_index_ = static_cast<size_t>(next);
    if (carousel_) {
        update_carousel();
    }
    else {
        apply_selection_highlight();
    }
}

void MenuScreen::apply_selection_highlight() {
    // Explicit styling instead of LV_STATE_CHECKED: the default theme's checked
    // colour barely differs from the row background, so the selected item (and
    // its text) looked like it vanished. Draw a solid purple pill with white
    // text on the selected row; leave the rest flat/transparent.
    for (size_t i = 0; i < rows_.size(); ++i) {
        lv_obj_t* row = rows_[i];
        if (i == selected_index_) {
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, bruce_accent(), 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_style_text_color(row, lv_color_white(), 0);
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        }
        else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            // Fall back to the theme's text colour for unselected rows.
            lv_obj_remove_local_style_prop(row, LV_STYLE_TEXT_COLOR, 0);
        }
    }
}

void MenuScreen::open_selected() {
    if (carousel_) {
        const auto& category = model::tool_catalog()[selected_index_];
        if (category.info_only) {
            view_model().show_about();
        }
        else {
            view_model().show_menu(static_cast<int>(selected_index_));
        }
        return;
    }

    if (selected_index_ >= rows_.size()) {
        return;
    }

    // A row with no argv is a navigation row (not a launchable tool).
    const auto& category = model::tool_catalog()[static_cast<size_t>(category_index_)];
    if (category.actions[selected_index_].argv.empty()) {
        if (std::string(category.label) == "Theme") {
            // Apply the picked accent color and re-theme + rebuild in place.
            view::set_accent(static_cast<int>(selected_index_));
            view_model().refresh_theme();
        } else {
            // e.g. Nmap's "pick host" / "back": mutate state, re-enter category.
            model::on_navigation_action(category_index_, static_cast<int>(selected_index_));
            view_model().show_menu(category_index_);
        }
        return;
    }

    view_model().show_tool_run(category_index_, static_cast<int>(selected_index_));
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
