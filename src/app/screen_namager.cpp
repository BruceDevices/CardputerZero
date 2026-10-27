/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "screen_manager.h"

#include "about_screen.h"
#include "clock_screen.h"
#include "menu_screen.h"
#include "tool_run_screen.h"

namespace app {
namespace {

void flush_requested_refresh_async(void* user_data) {
    auto* manager = static_cast<ScreenManager*>(user_data);
    if (manager) {
        manager->flush_requested_refresh();
    }
}

void current_page_observer(lv_observer_t* observer, lv_subject_t* subject) {
    LV_UNUSED(subject);

    auto* manager = static_cast<ScreenManager*>(lv_observer_get_target(observer));
    if (manager) {
        manager->request_refresh();
    }
}

} // namespace

ScreenManager::ScreenManager(viewmodel::BaseViewModel& view_model, AssetManager& assets)
    : view_model_(view_model), assets_(assets) {}

ScreenManager::~ScreenManager() {
    lv_async_call_cancel(flush_requested_refresh_async, this);

    if (page_observer_) {
        lv_observer_remove(page_observer_);
        page_observer_ = nullptr;
    }
}

void ScreenManager::start() {
    if (page_observer_) {
        return;
    }

    page_observer_ = lv_subject_add_observer_with_target(view_model_.nav_token_subject(),
                                                         current_page_observer,
                                                         this,
                                                         nullptr);
    flush_requested_refresh();
}

void ScreenManager::request_refresh() {
    if (refresh_scheduled_) {
        return;
    }

    refresh_scheduled_ = lv_async_call(flush_requested_refresh_async, this) == LV_RESULT_OK;
}

void ScreenManager::flush_requested_refresh() {
    refresh_scheduled_ = false;

    switch (view_model_.current_page()) {
        case model::AppPage::ToolRun:
            load_screen(std::make_unique<screen::ToolRunScreen>(view_model_, assets_));
            break;
        case model::AppPage::About:
            load_screen(std::make_unique<screen::AboutScreen>(view_model_, assets_));
            break;
        case model::AppPage::Clock:
            load_screen(std::make_unique<screen::ClockScreen>(view_model_, assets_));
            break;
        case model::AppPage::Menu:
        default:
            load_screen(std::make_unique<screen::MenuScreen>(view_model_, assets_));
            break;
    }
}

lv_obj_t* ScreenManager::current_screen() const {
    return current_screen_ ? current_screen_->root() : nullptr;
}

void ScreenManager::load_screen(std::unique_ptr<screen::BaseScreen> next_screen) {
    if (!next_screen || !next_screen->root()) {
        return;
    }

    lv_screen_load(next_screen->root());
    current_screen_ = std::move(next_screen);
}

} // namespace app
