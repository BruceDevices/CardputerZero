/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"
#include "rf_receiver.h"
#include "rf_recording.h"
#include "tool_catalog.h"

#include <array>

namespace screen {

class RfScreen : public BaseScreen {
public:
    RfScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~RfScreen() override;

private:
    static constexpr int kBins = 41;

    void build_content(lv_obj_t* content) override;
    void update_controls();
    void update_frequency();
    void change_frequency(int step);
    void start();
    void stop();
    void tick();
    void process_frame(platform::RfRxFrame frame);
    void save_recording();
    void update_capture_status();
    void clear_plot();
    void draw_sweep();
    void draw_pulses(const std::vector<int>& durations);
    float bin_frequency(int bin) const;
    bool is_plot() const;
    bool is_record() const;

    model::RfTool tool_ = model::RfTool::None;
    hal::RfReceiver receiver_;
    platform::RawRecording recording_;
    int frequency_index_ = 4;
    int scan_index_ = 0;
    uint32_t scan_tuned_at_ = 0;
    uint32_t scan_hold_ms_ = 0;
    uint64_t scan_edges_ = 0;
    float last_signal_mhz_ = 0.0f;
    int bin_ = 0;
    size_t transition_count_ = 0;
    size_t frame_count_ = 0;
    bool confirm_discard_ = false;
    bool fixed_rssi_ = false;
    bool auto_scan_ = false;

    lv_timer_t* timer_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* frequency_ = nullptr;
    lv_obj_t* history_ = nullptr;
    lv_obj_t* axis_left_ = nullptr;
    lv_obj_t* axis_right_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_draw_buf_t* pixels_ = nullptr;
    std::array<int, kBins> rssi_{};
    std::array<int, kBins> peak_{};
    std::array<std::string, 3> recent_{};
};

} // namespace screen
