/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"
#include "gps_port.h"
#include "gps_session.h"
#include "gps_tracker.h"
#include "process_runner.h"
#include "tool_catalog.h"
#include "wardrive.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace screen {

// Shared screen for Bruce firmware's GPS features. It renders the feature's
// ScreenFrame the way the firmware draws it: the uppercased title,
// padprintln() lines (adapted to ~6 body lines by platform::gps::fit_lines)
// and the displayTextLine() / displayError() popups. Back stops the feature
// like ESC on the Cardputer.
//  - GPS Tracker runs in-app: the screen owns the GPS serial Session for as
//    long as it is shown (opened on entry, released on Back / at the end).
//  - Wardriving needs root (iw scan, BLE scan): it runs as the child
//    `<self> --wardrive <mode> --ui ...` (through sudo, with the same password
//    prompt as the other root screens), which owns the port and streams its
//    frames (platform::wardrive::FrameDecoder). Back sends it SIGTERM.
class GpsAppScreen : public BaseScreen {
public:
    GpsAppScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~GpsAppScreen() override;

private:
    void build_content(lv_obj_t* content) override;
    void build_widgets();
    void start_feature();
    void start_probe();
    void cancel_probe();
    void finish_probe();
    void open_port();
    // Wardriving (child process)
    void start_wardrive(const model::ToolAction& action);
    void launch_wardrive(const std::vector<std::string>& argv, const std::string& stdin_payload);
    void on_wardrive_output(const std::string& text);
    void on_wardrive_exit();
    void wardrive_failed(const std::string& message);
    void show_sudo_dialog();
    void close_sudo_dialog();
    void submit_sudo_dialog();
    static void process_cb(void* owner, const std::string& text, bool exited);
    static void sudo_submit_cb(lv_event_t* event);
    static void sudo_cancel_cb(lv_event_t* event);
    // Probing: GPS Port = Auto, the candidate ports are being probed.
    enum class Phase { Probing, Running, Closing, Done };

    // Shared with the detached probe thread, which keeps it alive.
    struct ProbeJob {
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        platform::gps::ProbeResult result; // written before `done`
    };

    void tick();
    void render();
    void render_frame(const platform::gps::ScreenFrame& frame);
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
    std::shared_ptr<ProbeJob> probe_;

    bool wardriving_ = false;
    platform::ProcessRunner runner_;
    platform::wardrive::FrameDecoder decoder_;
    platform::gps::ScreenFrame local_frame_; // screen-made frames (launch errors)
    std::string child_output_;               // raw output, for sudo's messages
    std::vector<std::string> pending_argv_;
    bool used_password_ = false;
    lv_obj_t* sudo_modal_ = nullptr;
    lv_obj_t* sudo_ta_ = nullptr;
    lv_group_t* sudo_group_ = nullptr;
};

} // namespace screen
