/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"
#include "gps_session.h"
#include "gps_tracker.h"

#include <memory>
#include <string>
#include <vector>

namespace screen {

// Shared screen for Bruce firmware's GPS features (GPS Tracker today). It owns
// the GPS serial Session for as long as it is shown - the port is opened on
// entry and released on Back / when the feature ends - and renders the
// feature's ScreenFrame the way the firmware draws it: the uppercased title,
// padprintln() lines (adapted to ~6 body lines by platform::gps::fit_lines)
// and the displayTextLine() / displayError() popups. Back stops the feature
// like ESC on the Cardputer.
class GpsAppScreen : public BaseScreen {
public:
    GpsAppScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~GpsAppScreen() override;

private:
    void build_content(lv_obj_t* content) override;
    void build_widgets();
    void start_feature();
    enum class Phase { Running, Closing, Done };

    void tick();
    void render();
    void begin_closing(bool by_back);
    void conclude();
    void shutdown();
    void leave();

    static void tick_cb(lv_timer_t* timer);
    static void leave_cb(lv_timer_t* timer);
    static void back_cb(lv_event_t* event);

    lv_obj_t* content_root_ = nullptr;
    lv_obj_t* title_ = nullptr;
    std::vector<lv_obj_t*> line_labels_;
    lv_obj_t* popup_ = nullptr;
    lv_obj_t* popup_label_ = nullptr;

    lv_timer_t* tick_timer_ = nullptr;
    lv_timer_t* leave_timer_ = nullptr;
    uint32_t rendered_version_ = 0;
    Phase phase_ = Phase::Running;
    bool closed_by_back_ = false;

    std::string port_;
    int baud_ = 0;
    platform::gps::Session session_;
    std::unique_ptr<platform::gps::Tracker> tracker_;
};

} // namespace screen
