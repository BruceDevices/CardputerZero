/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "gps_app_screen.h"

#include "gps_config.h"
#include "gps_port.h"
#include "logger.h"
#include "theme.h"
#include "ui_const.h"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <unistd.h>

namespace screen {
namespace {

namespace gps = platform::gps;

constexpr int32_t kContentH = view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight;
// Montserrat 14 title row + 6 Montserrat 12 body lines fill the 110 px area
// (the firmware's 6x8 font fits ~13; fit_lines() adapts its text).
constexpr int32_t kTitleH = 18;
constexpr int32_t kLineH = 15;
constexpr size_t kBodyLines = static_cast<size_t>((kContentH - kTitleH) / kLineH);
// Bruce's BORDER_PAD_X steps: padx 1 / padx 2.
constexpr int32_t kPad1 = 8;
constexpr int32_t kPad2 = 20;
// Poll the reader 4x/s while the screen is up; the firmware's own cadence
// (1 s waiting counter, 5 s pass) is timed by the Tracker from the clock.
constexpr uint32_t kTickMs = 250;
// displayError("GPS not Found!") is followed by an immediate return to the
// menu on the firmware; hold it long enough to read.
constexpr uint32_t kNotFoundHoldMs = 1500;
constexpr uint32_t kErrorRed = 0xd32f2f; // displayError(): TFT_RED stripe

uint64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// Auto: each candidate x baud gets this long to show one valid sentence (the
// receivers send a burst at least once a second).
constexpr int kAutoProbeMs = 1500;

} // namespace

GpsAppScreen::GpsAppScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

GpsAppScreen::~GpsAppScreen() {
    cancel_probe();
    if (leave_timer_) {
        lv_timer_delete(leave_timer_);
        leave_timer_ = nullptr;
    }
    shutdown();
}

void GpsAppScreen::build_content(lv_obj_t* content) {
    content_root_ = content;
    build_widgets();

    configure_common_nav(view::NAV_BACK, back_cb, this);
    for (size_t slot = 1; slot <= 3; ++slot) {
        nav_bar().set_icon(slot, "", nullptr, nullptr);
    }

    start_feature();
}

void GpsAppScreen::build_widgets() {
    const auto pal = view::palette(view_model().is_dark_mode());

    title_ = lv_label_create(content_root_);
    lv_obj_set_style_text_font(title_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_, pal.primary, 0);
    lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 1);
    lv_label_set_text(title_, "");

    for (size_t i = 0; i < kBodyLines; ++i) {
        auto* label = lv_label_create(content_root_);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, pal.primary, 0); // padprintln(): priColor text
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(label, kPad1, kTitleH + static_cast<int32_t>(i) * kLineH);
        lv_obj_set_width(label, view::kScreenWidth - kPad1 - 4);
        lv_label_set_text(label, "");
        line_labels_.push_back(label);
    }

    // displayRedStripe(): a rounded box across the screen, centred.
    popup_ = lv_obj_create(content_root_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_width(popup_, view::kScreenWidth - 2 * 10);
    lv_obj_set_height(popup_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(popup_, 7, 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(popup_, 6, 0);
    lv_obj_set_style_pad_hor(popup_, 8, 0);
    lv_obj_align(popup_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(popup_, LV_OBJ_FLAG_SCROLLABLE);
    popup_label_ = lv_label_create(popup_);
    lv_obj_set_width(popup_label_, LV_PCT(100));
    lv_label_set_long_mode(popup_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(popup_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(popup_label_, lv_color_white(), 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_HIDDEN);
}

void GpsAppScreen::start_feature() {
    // GPS > Config (~/.bruce/gps.conf, else the board default), with
    // $GPS_UART / $GPS_BAUD on top.
    const auto eff = model::gps_config::resolve(model::gps_config::load());
    port_ = eff.port;
    baud_ = eff.baud;
    tracker_ = std::make_unique<gps::Tracker>(gps::tracker_options_from_env());

    if (model::gps_config::is_auto(port_) && ::geteuid() != 0) {
        start_probe();
        return;
    }
    open_port();
}

// Auto: probe the candidate ports on a worker thread (each one is opened only
// for its probe window) while the screen shows "Searching GPS..."; the UI
// thread only polls the job's done flag from the tick timer.
void GpsAppScreen::start_probe() {
    phase_ = Phase::Probing;
    probe_ = std::make_shared<ProbeJob>();
    const auto ports = gps::auto_order(model::gps_config::current_board_default().port, gps::port_candidates());
    LOG_INFO("gps: Auto port: probing {} candidate(s) at {} bps first", ports.size(), baud_);
    auto job = probe_;
    const int baud = baud_;
    std::thread([job, ports, baud]() {
        job->result = gps::auto_detect(ports, baud, model::gps_config::baud_rates(), kAutoProbeMs, &job->cancel);
        job->done.store(true);
    }).detach(); // holds its own reference: Back never waits for a probe window

    lv_label_set_text(title_, "GPS TRACKER");
    lv_obj_set_style_bg_color(popup_, view::palette(view_model().is_dark_mode()).primary, 0);
    lv_obj_set_style_text_font(popup_label_, &lv_font_montserrat_14, 0);
    lv_label_set_text(popup_label_, "Searching GPS...");
    lv_obj_remove_flag(popup_, LV_OBJ_FLAG_HIDDEN);
    tick_timer_ = lv_timer_create(tick_cb, kTickMs, this);
}

void GpsAppScreen::cancel_probe() {
    if (probe_) {
        probe_->cancel.store(true);
        probe_.reset();
    }
}

void GpsAppScreen::finish_probe() {
    const gps::ProbeResult result = probe_->result;
    probe_.reset();
    if (tick_timer_) {
        lv_timer_delete(tick_timer_);
        tick_timer_ = nullptr;
    }
    phase_ = Phase::Running;
    if (result.kind == gps::ProbeResult::Kind::Found) {
        LOG_INFO("gps: Auto port: {}", result.message);
        port_ = result.port;
        baud_ = result.baud;
        open_port();
        return;
    }
    LOG_INFO("gps: Auto port: {}", result.message);
    tracker_->fail_open(result.message, /*not_found=*/false); // waits for OK: the text is long
    render();
    begin_closing(/*by_back=*/false);
}

void GpsAppScreen::open_port() {
    // Shared entry with --gps-tracker: refuses root before the UART is opened.
    if (!gps::begin_tracker(session_, *tracker_, port_, baud_, now_ms())) {
        LOG_INFO("gps: GPS Tracker not started on {} @ {}: {}", port_, baud_, tracker_->frame().popup_text);
        render();
        begin_closing(/*by_back=*/false);
        return;
    }
    LOG_INFO("gps: GPS Tracker on {} @ {} bps", port_, baud_);
    render();
    tick_timer_ = lv_timer_create(tick_cb, kTickMs, this);
    tick();
}

void GpsAppScreen::tick() {
    if (!tracker_) {
        return;
    }
    if (phase_ == Phase::Probing) {
        if (probe_ && probe_->done.load()) {
            finish_probe();
        }
        return;
    }
    if (phase_ == Phase::Running) {
        // One synchronized snapshot (bytes + hang-up + parser) and only the
        // TinyGPS++-style reads under the lock.
        session_.snapshot([&](size_t bytes, bool hung_up, gps::Parser& parser) {
            tracker_->tick(now_ms(), bytes, hung_up, parser);
        });
        // Hand the GPX write to the storage worker / apply its results: never
        // blocks this (LVGL) thread on storage.
        tracker_->flush_io();
        gps::set_connected(tracker_->gps_connected());
        render();
        if (tracker_->state() == gps::Tracker::State::Ended) {
            begin_closing(/*by_back=*/false);
        }
        return;
    }
    if (phase_ == Phase::Closing) {
        const bool idle = tracker_->io_idle(); // applies late results (errors)
        render();
        if (idle) {
            conclude();
        }
    }
}

void GpsAppScreen::render() {
    if (!tracker_) {
        return;
    }
    const gps::ScreenFrame& frame = tracker_->frame();
    if (frame.version == rendered_version_ && rendered_version_ != 0) {
        return;
    }
    rendered_version_ = frame.version;

    lv_label_set_text(title_, upper(frame.title).c_str()); // printTitle() uppercases

    const auto lines = gps::fit_lines(frame.lines, line_labels_.size());
    for (size_t i = 0; i < line_labels_.size(); ++i) {
        lv_obj_t* label = line_labels_[i];
        if (i < lines.size()) {
            const int32_t x = lines[i].padx >= 2 ? kPad2 : kPad1;
            lv_obj_set_x(label, x);
            lv_obj_set_width(label, view::kScreenWidth - x - 4);
            lv_label_set_text(label, lines[i].text.c_str());
        } else {
            lv_label_set_text(label, "");
        }
    }

    if (frame.popup == gps::PopupKind::None) {
        lv_obj_add_flag(popup_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const auto pal = view::palette(view_model().is_dark_mode());
    lv_obj_set_style_bg_color(popup_,
                              frame.popup == gps::PopupKind::Error ? lv_color_hex(kErrorRed) : pal.primary, 0);
    // displayRedStripe(): FM (big) when the text fits on one line, else FP.
    lv_obj_set_style_text_font(popup_label_,
                               frame.popup_text.size() <= 32 ? &lv_font_montserrat_14 : &lv_font_montserrat_12, 0);
    lv_label_set_text(popup_label_, frame.popup_text.c_str());
    lv_obj_remove_flag(popup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(popup_);
}

// The feature ended (Back, GPS not Found!, storage/open error). The UART is
// released right away; the storage worker may still be closing the track, so
// the screen keeps ticking (without blocking) until it reports back.
void GpsAppScreen::begin_closing(bool by_back) {
    if (phase_ != Phase::Running) {
        return;
    }
    phase_ = Phase::Closing;
    closed_by_back_ = by_back;
    session_.close();
    gps::set_connected(false);
    if (tracker_) {
        tracker_->stop(); // queues the footer on the storage worker
    }
    if (!tick_timer_) {
        tick_timer_ = lv_timer_create(tick_cb, kTickMs, this);
    }
    tick(); // finish at once when no file work is pending
}

void GpsAppScreen::conclude() {
    if (phase_ == Phase::Done) {
        return;
    }
    phase_ = Phase::Done;
    if (tick_timer_) {
        lv_timer_delete(tick_timer_);
        tick_timer_ = nullptr;
    }
    render();
    if (tracker_ && !tracker_->finalize_error().empty()) {
        // Closing the GPX failed: say so and wait for OK instead of leaving.
        nav_bar().set_icon(2, "OK", back_cb, this);
        return;
    }
    if (closed_by_back_) {
        view_model().go_back();
        return;
    }
    if (tracker_ && tracker_->end_reason() == gps::Tracker::EndReason::NotFound) {
        // Firmware: displayError() then straight back to the menu.
        leave_timer_ = lv_timer_create(leave_cb, kNotFoundHoldMs, this);
        lv_timer_set_repeat_count(leave_timer_, 1);
        return;
    }
    // Errors wait for a key, like displayError(msg, true).
    nav_bar().set_icon(2, "OK", back_cb, this);
}

void GpsAppScreen::shutdown() {
    if (tick_timer_) {
        lv_timer_delete(tick_timer_);
        tick_timer_ = nullptr;
    }
    if (tracker_) {
        tracker_->stop(); // non-blocking: the worker writes the footer
    }
    session_.close();
    gps::set_connected(false);
}

void GpsAppScreen::leave() {
    if (leave_timer_) {
        lv_timer_delete(leave_timer_);
        leave_timer_ = nullptr;
    }
    switch (phase_) {
        case Phase::Probing:
            // Nothing opened yet: stop the probe (it exits within ~100 ms on
            // its own thread) and leave at once.
            cancel_probe();
            if (tick_timer_) {
                lv_timer_delete(tick_timer_);
                tick_timer_ = nullptr;
            }
            phase_ = Phase::Done;
            view_model().go_back();
            return;
        case Phase::Running:
            begin_closing(/*by_back=*/true); // ESC: end() + ~GPSTracker()
            return;
        case Phase::Closing:
            // A second Back while storage is still busy: leave now; the worker
            // finishes the track on its own.
            shutdown();
            view_model().go_back();
            return;
        case Phase::Done:
            view_model().go_back();
            return;
    }
}

void GpsAppScreen::tick_cb(lv_timer_t* timer) {
    auto* self = static_cast<GpsAppScreen*>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void GpsAppScreen::leave_cb(lv_timer_t* timer) {
    auto* self = static_cast<GpsAppScreen*>(lv_timer_get_user_data(timer));
    if (self) {
        self->leave_timer_ = nullptr; // one-shot: LVGL deletes it after this call
        self->leave();
    }
}

void GpsAppScreen::back_cb(lv_event_t* event) {
    auto* self = static_cast<GpsAppScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->leave();
    }
}

} // namespace screen
