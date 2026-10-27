/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"

namespace screen {

// Full-screen themed clock face shown by the System/Clock -> "Clock" row
// (ToolAction::clock_view). Big HH:MM in the accent color with the weekday and
// date beneath it, refreshed once a second by an lv_timer. No tool log.
class ClockScreen : public BaseScreen {
public:
    ClockScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~ClockScreen() override;

private:
    void build_content(lv_obj_t* content) override;
    void update();

    static void tick_cb(lv_timer_t* timer);
    static void back_cb(lv_event_t* event);

    lv_obj_t* time_label_{nullptr};
    lv_obj_t* date_label_{nullptr};
    lv_timer_t* timer_{nullptr};
};

} // namespace screen
