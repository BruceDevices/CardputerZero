/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"

#include <vector>

namespace screen {

// One reusable screen drives both the root menu (categories) and every
// category's tool list - the data comes from model::tool_catalog(), so
// adding a tool never needs a new screen class.
class MenuScreen : public BaseScreen {
public:
    MenuScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);

private:
    void build_content(lv_obj_t* content) override;
    void build_carousel(lv_obj_t* content); // root menu: Bruce-style icon carousel
    void build_list(lv_obj_t* content);      // category submenu: vertical action list
    void move_selection(int delta);
    void apply_selection_highlight();
    void update_carousel();
    void open_selected();

    static void move_up_cb(lv_event_t* event);
    static void move_down_cb(lv_event_t* event);
    static void select_cb(lv_event_t* event);
    static void quit_cb(lv_event_t* event);
    static void back_cb(lv_event_t* event);

    int category_index_{-1};
    size_t selected_index_{0};
    std::vector<lv_obj_t*> rows_;

    // Carousel widgets (root menu only).
    bool carousel_{false};
    lv_obj_t* icon_prev_{nullptr};
    lv_obj_t* icon_curr_{nullptr};
    lv_obj_t* icon_next_{nullptr};
    lv_obj_t* name_label_{nullptr};
};

} // namespace screen
