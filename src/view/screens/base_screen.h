/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "navbar.h"
#include "base_viewmodel.h"
#include "titlebar.h"
#include "lvgl.h"

#include <memory>

namespace app {
class AssetManager;
}

namespace screen {

class BaseScreen {
public:
    BaseScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    virtual ~BaseScreen();

    BaseScreen(const BaseScreen&) = delete;
    BaseScreen& operator=(const BaseScreen&) = delete;

    void init();
    lv_obj_t* root() const;

protected:
    virtual void build_content(lv_obj_t* content) = 0;

    viewmodel::BaseViewModel& view_model();
    app::AssetManager& assets();
    view::widgets::NavBar& nav_bar();

    // Every screen shares the same slot 0 (back/quit) and slot 4 (theme
    // toggle) so the bottom bar reads the same way everywhere; only the
    // back label/action differs between the root menu ("Quit") and
    // everything else ("Back").
    void configure_common_nav(const char* back_text, lv_event_cb_t back_cb);

private:
    static void theme_toggle_cb(lv_event_t* event);
    static void theme_label_observer_cb(lv_observer_t* observer, lv_subject_t* subject);

    viewmodel::BaseViewModel& view_model_;
    app::AssetManager& assets_;
    lv_obj_t* root_{nullptr};
    lv_obj_t* content_{nullptr};
    std::unique_ptr<view::widgets::TitleBar> title_bar_;
    std::unique_ptr<view::widgets::NavBar> nav_bar_;
    lv_observer_t* theme_label_observer_{nullptr};
};

} // namespace screen
