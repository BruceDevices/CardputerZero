/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "menu_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "bruce_module.h"
#include "gps_port.h"
#include "rfid_mfrc522.h"
#include "rfid_pn532.h"
#include "rfid_st25r.h"
#include "theme.h"
#include "tool_catalog.h"
#include "ui_const.h"

#include <string>
#include <unordered_map>

namespace screen {
namespace {

constexpr int32_t kHeaderHeight = 20;

// The Bruce accent for carousel icons / selection pill. Follows the user's
// chosen theme color (view::palette reads the current accent); same hue in
// light/dark, so the icons only need a rebuild on accent change, not per theme.
inline lv_color_t bruce_accent() {
    return view::palette(true).primary;
}

// Last highlighted row per menu level (-1 = root carousel, otherwise the
// category index). The MenuScreen is rebuilt on every navigation, so without
// this, Back from a category or tool always landed on the first item.
std::unordered_map<int, size_t>& remembered_selection() {
    static std::unordered_map<int, size_t> sel;
    return sel;
}

// Error from a navigation row (GPS: Config save failed), shown as a popup
// over the rebuilt list - Bruce's displayError() - then cleared.
std::string& pending_error() {
    static std::string text;
    return text;
}

// Red popup centred over the list, removed after a few seconds (it goes away
// with the screen if that is rebuilt first).
void show_error_popup(lv_obj_t* parent, const std::string& text) {
    auto* popup = lv_obj_create(parent);
    lv_obj_remove_style_all(popup);
    lv_obj_set_width(popup, view::kScreenWidth - 2 * 10);
    lv_obj_set_height(popup, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(popup, 7, 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(popup, lv_color_hex(0xd32f2f), 0); // TFT_RED stripe
    lv_obj_set_style_pad_ver(popup, 6, 0);
    lv_obj_set_style_pad_hor(popup, 8, 0);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    auto* label = lv_label_create(popup);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, text.size() <= 32 ? &lv_font_montserrat_14 : &lv_font_montserrat_12, 0);
    lv_label_set_text(label, text.c_str());
    lv_obj_move_foreground(popup);
    lv_obj_delete_delayed(popup, 3000);
}

size_t restore_selection(int level, size_t count) {
    const auto& sel = remembered_selection();
    const auto it = sel.find(level);
    if (it == sel.end() || count == 0) {
        return 0;
    }
    return it->second < count ? it->second : count - 1; // dynamic lists may shrink
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
        selected_index_ = restore_selection(-1, model::tool_catalog().size());
    }

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
    model::refresh_dynamic_categories(category_index_);
    const auto& pre_refresh_catalog = model::tool_catalog();
    if (category_index_ >= 0 &&
        category_index_ < static_cast<int>(pre_refresh_catalog.size()) &&
        std::string(pre_refresh_catalog[static_cast<size_t>(category_index_)].label) ==
            "Sub-GHz / RF") {
        hal::redetect();
        const auto& pins = hal::modules().cc1101;
        model::refresh_rf_category(hal::has_cc1101(), pins.cs, pins.gdo0);
    }
    if (category_index_ >= 0 &&
        category_index_ < static_cast<int>(pre_refresh_catalog.size()) &&
        std::string(pre_refresh_catalog[static_cast<size_t>(category_index_)].label) == "GPS") {
        // Config > GPS Port's picker lists the serial nodes present right now
        // (a readdir of /dev; no port is opened here).
        model::refresh_gps_category(platform::gps::port_candidates());
    }
    bool rfid_category = false;
    bool rfid_st25r = false, rfid_rfid2 = false, rfid_pn532 = false;
    if (category_index_ >= 0 &&
        category_index_ < static_cast<int>(pre_refresh_catalog.size()) &&
        std::string(pre_refresh_catalog[static_cast<size_t>(category_index_)].label) == "RFID") {
        // Show each module's tag actions only when it is actually present. The
        // CAP's ST25R (SPI) comes from the same HAL probe the RF menu uses; an
        // I2C ST25R unit, the RFID2 (MFRC522) and a PN532 are probed natively.
        rfid_category = true;
        hal::redetect();
        rfid_st25r = hal::has_st25r() || platform::st25r_present_i2c();
        rfid_rfid2 = platform::rfid2_present();
        rfid_pn532 = platform::pn532_present();
        model::refresh_rfid_category(rfid_st25r, rfid_rfid2, rfid_pn532);
    }
    const auto& catalog = model::tool_catalog();

    auto* header = lv_label_create(content);
    lv_obj_set_size(header, LV_PCT(100), kHeaderHeight);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_left(header, 8, 0);
    const auto& category = catalog[static_cast<size_t>(category_index_)];
    if (std::string(category.label) == "Sub-GHz / RF" && !hal::has_cc1101()) {
        lv_label_set_text(header, "Sub-GHz / RF (CC1101 not detected)");
    } else if (rfid_category) {
        std::string tags;
        if (rfid_st25r) tags += tags.empty() ? "ST25R" : ", ST25R";
        if (rfid_rfid2) tags += tags.empty() ? "RFID2" : ", RFID2";
        if (rfid_pn532) tags += tags.empty() ? "PN532" : ", PN532";
        lv_label_set_text(header, tags.empty() ? "RFID (no module detected)"
                                               : ("RFID (" + tags + ")").c_str());
    } else {
        lv_label_set_text(header, model::category_header(category_index_).c_str());
    }

    auto* list = lv_list_create(content);
    lv_obj_set_size(list, LV_PCT(100), view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight - kHeaderHeight);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0); // tighter gap so ~4 rows fit, not 2

    const size_t item_count = catalog[static_cast<size_t>(category_index_)].actions.size();
    selected_index_ = restore_selection(category_index_, item_count);
    if (remembered_selection().count(category_index_) == 0) {
        // Start on the current value (GPS Baudrate / port picker), like
        // Bruce's loopOptions(options, index).
        const int preferred = model::preferred_selection(category_index_);
        if (preferred >= 0 && static_cast<size_t>(preferred) < item_count) {
            selected_index_ = static_cast<size_t>(preferred);
        }
    }
    rows_.reserve(item_count);
    for (size_t i = 0; i < item_count; ++i) {
        const char* label = catalog[static_cast<size_t>(category_index_)].actions[i].label;
        auto* row = lv_list_add_button(list, nullptr, label);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        // Compact rows: the default list button is tall enough that only ~2 fit
        // in the content area; trim the vertical padding so ~4 are visible.
        lv_obj_set_style_pad_ver(row, 3, 0);
        lv_obj_set_style_text_font(row, &lv_font_montserrat_14, 0);
        if (catalog[static_cast<size_t>(category_index_)].actions[i].dimmed) {
            lv_obj_set_style_text_opa(row, LV_OPA_40, 0); // disabled option
        }
        rows_.push_back(row);
    }

    // Lay the rows out now so scroll_to_view can reach a remembered row
    // further down the list.
    lv_obj_update_layout(list);
    apply_selection_highlight();

    if (!pending_error().empty()) {
        show_error_popup(content, pending_error());
        pending_error().clear();
    }

    nav_bar().set_icon(1, view::NAV_UP, move_up_cb, this);
    nav_bar().set_icon(2, view::NAV_SELECT, select_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, move_down_cb, this);
    // Back first undoes a category's in-place drill-down (Nmap host view,
    // Evil Portal clone view, timezone picker) - same as its "< Back" row -
    // and only leaves the category when there is nothing left to undo.
    configure_common_nav(
        view::NAV_BACK,
        [](lv_event_t* event) {
            auto* self = static_cast<MenuScreen*>(lv_event_get_user_data(event));
            if (!self) {
                return;
            }
            if (model::on_category_back(self->category_index_)) {
                remembered_selection().erase(self->category_index_); // list contents changed
                self->view_model().show_menu(self->category_index_);
            } else {
                self->view_model().go_back();
            }
        },
        this);
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
    remembered_selection()[category_index_] = selected_index_;
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
    const auto& action = category.actions[selected_index_];
    if (action.dimmed) {
        return; // a disabled option does nothing (GPS Port > "Save Config")
    }
    if (action.rf_tool != model::RfTool::None) {
        view_model().show_rf(category_index_, static_cast<int>(selected_index_));
        return;
    }
    if (action.ble_scan_view) {
        view_model().show_ble_scan(category_index_, static_cast<int>(selected_index_));
        return;
    }
    if (action.gps_app != model::GpsApp::None) {
        view_model().show_gps_app(category_index_, static_cast<int>(selected_index_));
        return;
    }
    if (action.wifi_view != model::WifiView::None) {
        view_model().show_wifi_scan(category_index_, static_cast<int>(selected_index_));
        return;
    }
    if (action.argv.empty()) {
        if (std::string(category.label) == "Theme") {
            // Apply the picked accent color and re-theme + rebuild in place.
            view::set_accent(static_cast<int>(selected_index_));
            view_model().refresh_theme();
        } else {
            // e.g. Nmap's "pick host" / "back": mutate state, re-enter category
            // - or leave it for the root menu (GPS "Main Menu").
            const bool leave = model::on_navigation_action(category_index_, static_cast<int>(selected_index_));
            pending_error() = model::take_navigation_error(category_index_);
            if (!pending_error().empty()) {
                // Same list again (edited value kept), the row still selected
                // for a retry, with the error on top.
                remembered_selection()[category_index_] = selected_index_;
                view_model().show_menu(category_index_);
                return;
            }
            remembered_selection().erase(category_index_); // list contents changed
            if (leave) {
                view_model().show_menu(-1);
            } else {
                view_model().show_menu(category_index_);
            }
        }
        return;
    }

    if (category.actions[selected_index_].clock_view) {
        view_model().show_clock(category_index_, static_cast<int>(selected_index_));
        return;
    }

    if (category.actions[selected_index_].terminal_view) {
        view_model().show_terminal(category_index_, static_cast<int>(selected_index_));
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
