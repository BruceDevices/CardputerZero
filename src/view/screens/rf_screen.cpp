/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "rf_screen.h"

#include "bindings.h"
#include "rf_bridge.h"
#include "theme.h"
#include "ui_const.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace screen {
namespace {

constexpr int kPlotWidth = 280;
constexpr int kPlotHeight = 56;
constexpr int kPlotLeft = 33;
constexpr int kPlotTop = 37;
constexpr int kFloorDbm = -105;
constexpr int kCeilingDbm = -35;
constexpr int kPulseWindowUs = 16000;
constexpr size_t kRecordingLimit = 131072;
constexpr uint32_t kSweepTickMs = 8;
constexpr uint32_t kFixedTickMs = 100;
constexpr uint32_t kCaptureTickMs = 15;
constexpr uint32_t kScanDwellMs = 300;
constexpr uint32_t kScanSignalHoldMs = 1300;

constexpr std::array kFrequencies = {
    303.875f, 315.00f, 390.00f, 418.00f, 433.92f,
    434.42f, 868.35f, 868.95f, 915.00f, 923.00f,
};

lv_obj_t* label(lv_obj_t* parent, lv_subject_t* theme, int x, int y, const char* text) {
    auto* result = lv_label_create(parent);
    lv_obj_set_pos(result, x, y);
    lv_obj_set_style_text_font(result, &lv_font_montserrat_12, 0);
    lv_label_set_text(result, text);
    reactive::bind_theme(result, theme, reactive::ThemeRole::Text);
    return result;
}

uint16_t* plot_row(lv_draw_buf_t* pixels, int y) {
    return static_cast<uint16_t*>(lv_draw_buf_goto_xy(pixels, 0, y));
}

int rssi_height(int dbm) {
    const int clamped = std::clamp(dbm, kFloorDbm, kCeilingDbm);
    return (clamped - kFloorDbm) * (kPlotHeight - 1) / (kCeilingDbm - kFloorDbm);
}

uint16_t heat_color(int dbm) {
    const lv_color_t colors[] = {
        lv_color_hex(0x100d22), lv_color_hex(0x392889), lv_color_hex(0x267bba),
        lv_color_hex(0x3cc68c), lv_color_hex(0xffdf55),
    };
    const int scale = rssi_height(dbm) * 4 * 255 / (kPlotHeight - 1);
    const int index = std::min(scale / 255, 3);
    return lv_color_to_u16(lv_color_mix(colors[index + 1], colors[index], scale - index * 255));
}

} // namespace

RfScreen::RfScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

RfScreen::~RfScreen() {
    if (timer_) lv_timer_delete(timer_);
    receiver_.stop();
    if (canvas_) lv_obj_delete(canvas_);
    if (pixels_) lv_draw_buf_destroy(pixels_);
}

bool RfScreen::is_plot() const {
    return tool_ == model::RfTool::Spectrum || tool_ == model::RfTool::Spectrogram;
}

bool RfScreen::is_record() const {
    return tool_ == model::RfTool::Record;
}

void RfScreen::build_content(lv_obj_t* content) {
    const auto& category = model::tool_catalog().at(view_model().active_category());
    const auto& action = category.actions.at(view_model().active_item());
    tool_ = action.rf_tool;
    auto* theme = view_model().dark_mode_subject();
    label(content, theme, 8, 3, action.label);

    frequency_ = label(content, theme, 176, 3, "");
    lv_obj_set_width(frequency_, 136);
    lv_obj_set_style_text_align(frequency_, LV_TEXT_ALIGN_RIGHT, 0);
    status_ = label(content, theme, 8, 20, "Press Start");
    lv_obj_set_width(status_, 304);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);

    if (tool_ == model::RfTool::ProtocolMonitor) {
        history_ = label(content, theme, 8, 42, "");
        lv_obj_set_width(history_, 304);
        lv_obj_set_style_text_line_space(history_, 5, 0);
    } else {
        pixels_ = lv_draw_buf_create(kPlotWidth, kPlotHeight, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        if (pixels_) {
            canvas_ = lv_canvas_create(content);
            lv_obj_set_pos(canvas_, kPlotLeft, kPlotTop);
            lv_canvas_set_draw_buf(canvas_, pixels_);
        }
        axis_left_ = label(content, theme, kPlotLeft, 95, "");
        axis_right_ = label(content, theme, 220, 95, "");
        lv_obj_set_width(axis_right_, 92);
        lv_obj_set_style_text_align(axis_right_, LV_TEXT_ALIGN_RIGHT, 0);
        if (tool_ == model::RfTool::Spectrum) {
            label(content, theme, 6, 35, "-35");
            label(content, theme, 2, 79, "-105");
        } else if (tool_ == model::RfTool::Spectrogram) {
            label(content, theme, 2, 35, "Now");
            label(content, theme, 2, 79, "Past");
        }
    }

    configure_common_nav(view::NAV_BACK, [](lv_event_t* event) {
        auto& self = *static_cast<RfScreen*>(lv_event_get_user_data(event));
        self.stop();
        if (!self.recording_.empty() && !self.confirm_discard_) {
            self.confirm_discard_ = true;
            lv_label_set_text(self.status_, "Save or press Back again to discard");
            return;
        }
        self.view_model().go_back();
    }, this);
    nav_bar().set_icon(1, "Freq −", [](lv_event_t* event) {
        static_cast<RfScreen*>(lv_event_get_user_data(event))->change_frequency(-1);
    }, this);
    nav_bar().set_icon(2, "Start", [](lv_event_t* event) {
        auto& self = *static_cast<RfScreen*>(lv_event_get_user_data(event));
        if (self.receiver_.active()) self.stop();
        else self.start();
    }, this);
    nav_bar().set_icon(3, "Freq +", [](lv_event_t* event) {
        static_cast<RfScreen*>(lv_event_get_user_data(event))->change_frequency(1);
    }, this);
    if (is_record()) {
        nav_bar().set_icon(4, "Save", [](lv_event_t* event) {
            static_cast<RfScreen*>(lv_event_get_user_data(event))->save_recording();
        }, this);
    } else if (tool_ == model::RfTool::Spectrum) {
        nav_bar().set_icon(4, "Fixed", [](lv_event_t* event) {
            auto& self = *static_cast<RfScreen*>(lv_event_get_user_data(event));
            const bool was_active = self.receiver_.active();
            self.stop();
            self.fixed_rssi_ = !self.fixed_rssi_;
            self.nav_bar().set_text(4, self.fixed_rssi_ ? "Sweep" : "Fixed");
            self.update_frequency();
            self.clear_plot();
            lv_label_set_text(self.status_, "Press Start");
            if (was_active) self.start();
        }, this);
    } else if (tool_ == model::RfTool::Scan) {
        nav_bar().set_icon(4, "Auto", [](lv_event_t* event) {
            auto& self = *static_cast<RfScreen*>(lv_event_get_user_data(event));
            const bool was_active = self.receiver_.active();
            self.stop();
            self.auto_scan_ = !self.auto_scan_;
            self.nav_bar().set_text(4, self.auto_scan_ ? "Fixed" : "Auto");
            lv_label_set_text(self.status_, self.auto_scan_ ? "Auto scan ready" : "Press Start");
            if (was_active) self.start();
        }, this);
    }

    update_frequency();
    clear_plot();
    update_controls();
    if (!pixels_ && !history_) {
        nav_bar().set_enabled(2, false);
        lv_label_set_text(status_, "Could not allocate plot");
    }
}

void RfScreen::update_controls() {
    const bool active = receiver_.active();
    const bool can_retune = !active && recording_.empty();
    nav_bar().set_enabled(1, can_retune);
    nav_bar().set_enabled(3, can_retune);
    nav_bar().set_text(2, active ? "Stop" : "Start");
    if (is_record()) nav_bar().set_enabled(4, !active && !recording_.empty());
}

float RfScreen::bin_frequency(int bin) const {
    return kFrequencies[frequency_index_] + 0.25f * (bin - kBins / 2);
}

void RfScreen::update_frequency() {
    const int channel = auto_scan_ && receiver_.active() ? scan_index_ : frequency_index_;
    lv_label_set_text_fmt(frequency_, "%.3f MHz", static_cast<double>(kFrequencies[channel]));
    if (fixed_rssi_) {
        lv_label_set_text(axis_left_, "−4 s");
        lv_label_set_text(axis_right_, "Now");
    } else if (is_plot()) {
        lv_label_set_text_fmt(axis_left_, "%.2f", static_cast<double>(bin_frequency(0)));
        lv_label_set_text_fmt(axis_right_, "%.2f MHz", static_cast<double>(bin_frequency(kBins - 1)));
    } else if (axis_left_) {
        lv_label_set_text(axis_left_, "0");
        lv_label_set_text(axis_right_, "16 ms");
    }
}

void RfScreen::change_frequency(int step) {
    if (receiver_.active() || !recording_.empty()) return;
    const int count = static_cast<int>(kFrequencies.size());
    frequency_index_ = (frequency_index_ + step + count) % count;
    frame_count_ = 0;
    transition_count_ = 0;
    recent_ = {};
    if (history_) lv_label_set_text(history_, "");
    update_frequency();
    clear_plot();
    lv_label_set_text(status_, "Press Start");
}

void RfScreen::start() {
    confirm_discard_ = false;
    if (recording_.total_durations() >= kRecordingLimit) {
        lv_label_set_text(status_, "Recording full. Save to continue.");
        return;
    }
    const float mhz = is_plot() && !fixed_rssi_ ? bin_frequency(0) : kFrequencies[frequency_index_];
    if (const char* error = receiver_.start(mhz, !is_plot())) {
        lv_label_set_text(status_, error);
        return;
    }
    if (recording_.empty()) {
        frame_count_ = 0;
        transition_count_ = 0;
        recent_ = {};
        if (history_) lv_label_set_text(history_, "");
    }
    recording_.frequency_hz = std::round(static_cast<double>(mhz) * 1000.0) * 1000.0;
    bin_ = 0;
    scan_index_ = frequency_index_;
    scan_tuned_at_ = lv_tick_get();
    scan_hold_ms_ = kScanDwellMs;
    scan_edges_ = 0;
    last_signal_mhz_ = 0.0f;
    clear_plot();
    lv_label_set_text(status_, (is_plot() && !fixed_rssi_) || auto_scan_ ? "Sweeping…" : "Listening…");
    const auto period = fixed_rssi_ ? kFixedTickMs : is_plot() ? kSweepTickMs : kCaptureTickMs;
    timer_ = lv_timer_create([](lv_timer_t* timer) {
        static_cast<RfScreen*>(lv_timer_get_user_data(timer))->tick();
    }, period, this);
    if (!timer_) {
        receiver_.stop();
        lv_label_set_text(status_, "Could not start sampling timer");
    }
    update_controls();
}

void RfScreen::stop() {
    if (!receiver_.active()) return;
    lv_timer_delete(timer_);
    timer_ = nullptr;
    std::deque<platform::RfRxFrame> pending;
    receiver_.stop(is_plot() ? nullptr : &pending);
    for (auto& frame : pending) process_frame(std::move(frame));
    if (is_plot()) {
        lv_label_set_text(status_, "Sampling stopped");
    } else if (frame_count_ == 0) {
        lv_label_set_text(status_, "No pulses captured");
    } else {
        update_capture_status();
    }
    if (auto_scan_) update_frequency();
    update_controls();
}

void RfScreen::tick() {
    if (fixed_rssi_) {
        std::move(rssi_.begin() + 1, rssi_.end(), rssi_.begin());
        rssi_.back() = receiver_.rssi_dbm();
        draw_sweep();
        lv_label_set_text_fmt(status_, "RSSI %d dBm", rssi_.back());
        return;
    }
    if (is_plot()) {
        rssi_[bin_] = receiver_.rssi_dbm();
        peak_[bin_] = std::max(peak_[bin_], rssi_[bin_]);
        ++bin_;
        if (bin_ == kBins) {
            draw_sweep();
            const auto peak = std::max_element(rssi_.begin(), rssi_.end());
            const auto mhz = bin_frequency(static_cast<int>(peak - rssi_.begin()));
            lv_label_set_text_fmt(status_, "Peak %.2f MHz    %d dBm", static_cast<double>(mhz), *peak);
            bin_ = 0;
        }
        receiver_.tune(bin_frequency(bin_));
        return;
    }
    platform::RfRxFrame frame;
    if (receiver_.poll(frame)) {
        process_frame(std::move(frame));
        scan_tuned_at_ = lv_tick_get();
        scan_hold_ms_ = kScanDwellMs;
        scan_edges_ = receiver_.edges();
        if (is_record() && transition_count_ >= kRecordingLimit) stop();
    } else if (auto_scan_ && lv_tick_elaps(scan_tuned_at_) >= scan_hold_ms_) {
        if (scan_hold_ms_ == kScanDwellMs && receiver_.edges() - scan_edges_ >= 16) {
            scan_hold_ms_ = kScanSignalHoldMs;
            return;
        }
        scan_index_ = (scan_index_ + 1) % kFrequencies.size();
        if (!receiver_.tune(kFrequencies[scan_index_])) {
            stop();
            lv_label_set_text(status_, "Could not retune GDO0 capture");
            return;
        }
        scan_tuned_at_ = lv_tick_get();
        scan_hold_ms_ = kScanDwellMs;
        scan_edges_ = 0;
        update_frequency();
    }
}

void RfScreen::update_capture_status() {
    if (auto_scan_) {
        lv_label_set_text_fmt(status_, "Last RX %.3f MHz", static_cast<double>(last_signal_mhz_));
        return;
    }
    lv_label_set_text_fmt(status_, "%s%u %s, %u transitions",
                          receiver_.active() ? "RX   " : "",
                          static_cast<unsigned>(frame_count_), frame_count_ == 1 ? "burst" : "bursts",
                          static_cast<unsigned>(transition_count_));
}

void RfScreen::process_frame(platform::RfRxFrame frame) {
    auto& durations = frame.durations;
    if (is_record()) {
        const size_t available = kRecordingLimit - transition_count_;
        if (!available) return;
        if (durations.size() > available) durations.resize(available);
    }

    ++frame_count_;
    transition_count_ += durations.size();
    if (auto_scan_) last_signal_mhz_ = kFrequencies[scan_index_];
    update_capture_status();

    if (tool_ == model::RfTool::ProtocolMonitor) {
        const auto decoded = rfproto::decode(durations);
        char summary[96];
        if (decoded.ok) {
            std::snprintf(summary, sizeof(summary), "%s, %d bits, TE %d us",
                          decoded.protocol.c_str(), decoded.bits, decoded.te);
        } else {
            std::snprintf(summary, sizeof(summary), "Unknown OOK, %u transitions",
                          static_cast<unsigned>(durations.size()));
        }
        recent_[2] = std::move(recent_[1]);
        recent_[1] = std::move(recent_[0]);
        recent_[0] = summary;
        lv_label_set_text_fmt(history_, "%s\n%s\n%s", recent_[0].c_str(), recent_[1].c_str(), recent_[2].c_str());
    } else {
        draw_pulses(durations);
    }

    if (is_record()) recording_.add_frame(std::move(durations), frame.start_ms);
}

void RfScreen::save_recording() {
    if (receiver_.active() || recording_.empty()) return;
    const auto file = platform::rf_save_sub(recording_, model::rf_recordings_dir());
    confirm_discard_ = false;
    if (file.empty()) {
        lv_label_set_text(status_, "Could not save .sub file");
        return;
    }
    lv_label_set_text_fmt(status_, "Saved %s", file.substr(file.find_last_of('/') + 1).c_str());
    recording_ = {};
    update_controls();
}

void RfScreen::clear_plot() {
    rssi_.fill(kFloorDbm);
    peak_.fill(kFloorDbm);
    if (!pixels_) return;
    lv_canvas_fill_bg(canvas_, lv_color_hex(0x151021), LV_OPA_COVER);
}

void RfScreen::draw_sweep() {
    if (tool_ == model::RfTool::Spectrogram) {
        const auto stride = pixels_->header.stride;
        std::memmove(pixels_->data + stride, pixels_->data, stride * (kPlotHeight - 1));
        auto* row = plot_row(pixels_, 0);
        for (int x = 0; x < kPlotWidth; ++x) row[x] = heat_color(rssi_[x * kBins / kPlotWidth]);
    } else {
        const auto palette = view::palette(view_model().is_dark_mode());
        const uint16_t fill = lv_color_to_u16(palette.primary);
        const uint16_t hold = lv_color_to_u16(palette.info);
        const uint16_t grid = lv_color_to_u16(lv_color_hex(0x372a4d));
        const uint16_t bg = lv_color_to_u16(lv_color_hex(0x151021));
        for (int y = 0; y < kPlotHeight; ++y) {
            const int height = kPlotHeight - 1 - y;
            auto* row = plot_row(pixels_, y);
            for (int x = 0; x < kPlotWidth; ++x) {
                const int bin = x * kBins / kPlotWidth;
                row[x] = height <= rssi_height(rssi_[bin]) ? fill
                         : (height % 18 == 0 || x % 70 == 0) ? grid : bg;
                if (!fixed_rssi_ && height == rssi_height(peak_[bin])) row[x] = hold;
            }
        }
    }
    lv_obj_invalidate(canvas_);
}

void RfScreen::draw_pulses(const std::vector<int>& durations) {
    lv_canvas_fill_bg(canvas_, lv_color_hex(0x151021), LV_OPA_COVER);
    const auto line = lv_color_to_u16(view::palette(view_model().is_dark_mode()).info);
    constexpr int high_y = 11;
    constexpr int low_y = kPlotHeight - 12;
    int elapsed = 0;
    int previous_y = -1;
    for (const int duration : durations) {
        const int start = elapsed * kPlotWidth / kPulseWindowUs;
        elapsed += std::min(std::abs(duration), kPulseWindowUs - elapsed);
        const int end = elapsed * kPlotWidth / kPulseWindowUs;
        const int y = duration > 0 ? high_y : low_y;
        if (previous_y >= 0 && previous_y != y) {
            for (int row = high_y; row <= low_y; ++row) plot_row(pixels_, row)[start] = line;
        }
        auto* row = plot_row(pixels_, y);
        std::fill(row + start, row + std::max(end, start + 1), line);
        previous_y = y;
        if (elapsed == kPulseWindowUs) break;
    }
    lv_obj_invalidate(canvas_);
}

} // namespace screen
