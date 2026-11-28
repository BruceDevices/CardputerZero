/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "gps_app_screen.h"

#include "gps_config.h"
#include "gps_port.h"
#include "linux_input.h"
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
    runner_.stop(); // Wardriving child: SIGTERM, wait for it to exit
    if (sudo_group_) {
        lv_group_delete(sudo_group_);
        sudo_group_ = nullptr;
    }
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
    const int cat = view_model().active_category();
    const int item = view_model().active_item();
    const auto& catalog = model::tool_catalog();
    if (cat >= 0 && static_cast<size_t>(cat) < catalog.size() && item >= 0 &&
        static_cast<size_t>(item) < catalog[static_cast<size_t>(cat)].actions.size()) {
        const auto& action = catalog[static_cast<size_t>(cat)].actions[static_cast<size_t>(item)];
        if (action.gps_app == model::GpsApp::Wardriving) {
            start_wardrive(action);
            return;
        }
    }
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
    if (tracker_) {
        render_frame(tracker_->frame());
    }
}

void GpsAppScreen::render_frame(const gps::ScreenFrame& frame) {
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
    if (wardriving_) {
        close_sudo_dialog();
        // First Back on a running screen: the run's result has not been
        // handled yet - whether the child still runs, or already exited and
        // its last output waits for the next poll. OK / Back on an error the
        // screen already shows (phase Done) just leaves.
        const bool undelivered = phase_ == Phase::Running;
        phase_ = Phase::Done;
        closed_by_back_ = true;
        // ESC: SIGTERM to the child (sudo relays it). The child stops within
        // ~100 ms, also during a scan or an Auto probe (at most ~2.5 s more
        // when storage is still syncing); stop() waits for it and removes the
        // poll timer, so the rest of its output is drained here.
        runner_.stop();
        gps::set_connected(false);
        if (undelivered && platform::wardrive::storage_error_after_back(decoder_, runner_.take_pending_output())) {
            LOG_INFO("gps: Wardriving ended: storage (after Back)");
            render_frame(decoder_.frame());
            nav_bar().set_icon(2, "OK", back_cb, this);
            return;
        }
        view_model().go_back();
        return;
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

// --- Wardriving (child process) ---------------------------------------------------

void GpsAppScreen::start_wardrive(const model::ToolAction& action) {
    wardriving_ = true;
    phase_ = Phase::Running;
    local_frame_ = gps::ScreenFrame{};
    local_frame_.title = "Wardriving";
    local_frame_.version = 1u << 30; // distinct from the child's frame versions
    render_frame(local_frame_);

    const bool needs_root = model::action_needs_root(action);
    pending_argv_ = needs_root ? model::sudo_argv_with_password(action) : action.argv;
    if (needs_root && !model::sudo_passwordless()) {
        show_sudo_dialog();
        return;
    }
    launch_wardrive(model::launch_argv(action), "");
}

void GpsAppScreen::launch_wardrive(const std::vector<std::string>& argv, const std::string& stdin_payload) {
    child_output_.clear();
    decoder_ = platform::wardrive::FrameDecoder{};
    phase_ = Phase::Running;
    std::string mode;
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == "--wardrive") {
            mode = argv[i + 1];
        }
    }
    LOG_INFO("gps: Wardriving ({}){}", mode, stdin_payload.empty() ? "" : " with the sudo password");
    if (!runner_.start(argv, &GpsAppScreen::process_cb, this, stdin_payload)) {
        wardrive_failed("Failed to start Wardriving");
    }
}

void GpsAppScreen::process_cb(void* owner, const std::string& text, bool exited) {
    auto* self = static_cast<GpsAppScreen*>(owner);
    if (!self) {
        return;
    }
    if (!text.empty()) {
        self->on_wardrive_output(text);
    }
    if (exited) {
        self->on_wardrive_exit();
    }
}

void GpsAppScreen::on_wardrive_output(const std::string& text) {
    if (child_output_.size() < 16384) {
        child_output_ += text;
    }
    if (decoder_.feed(text)) {
        gps::set_connected(decoder_.gps_connected()); // drawGpsSmall() while gpsConnected
        if (decoder_.any_frame()) {
            render_frame(decoder_.frame());
        }
    }
}

void GpsAppScreen::on_wardrive_exit() {
    gps::set_connected(false);
    if (closed_by_back_ || phase_ == Phase::Done) {
        return;
    }
    for (const auto& l : decoder_.log()) {
        LOG_INFO("gps: wardrive: {}", l);
    }
    // sudo refused (no passwordless rule) or rejected the password: the same
    // on-device prompt as the other root screens.
    if (!decoder_.any_frame()) {
        const bool refused = child_output_.find("a password is required") != std::string::npos ||
                             child_output_.find("a terminal is required") != std::string::npos;
        const bool rejected = used_password_ && (child_output_.find("incorrect password") != std::string::npos ||
                                                 child_output_.find("Sorry, try again") != std::string::npos);
        if (rejected || (!used_password_ && refused)) {
            show_sudo_dialog();
            return;
        }
    }
    phase_ = Phase::Done;
    const std::string& reason = decoder_.end_reason();
    LOG_INFO("gps: Wardriving ended: {}", reason.empty() ? "no end marker" : reason);
    if (reason == "stopped") {
        view_model().go_back();
        return;
    }
    if (reason == "notfound") {
        // Firmware: displayError("GPS not Found!") then straight back to the menu.
        leave_timer_ = lv_timer_create(leave_cb, kNotFoundHoldMs, this);
        lv_timer_set_repeat_count(leave_timer_, 1);
        return;
    }
    if (reason.empty()) {
        // The child never finished (crash, sudo or exec error): show its last line.
        // (Bruce's own Serial lines - "ERR: ...", "MESSAGE: ..." - are not it.)
        std::string last;
        for (const auto& l : decoder_.log()) {
            if (l.rfind("ERR: ", 0) != 0 && l.rfind("MESSAGE: ", 0) != 0) {
                last = l;
            }
        }
        wardrive_failed(last.empty() ? "Wardriving stopped unexpectedly" : last);
        return;
    }
    // Open / storage errors: displayError(msg, true) waits for a key.
    nav_bar().set_icon(2, "OK", back_cb, this);
}

void GpsAppScreen::wardrive_failed(const std::string& message) {
    phase_ = Phase::Done;
    local_frame_ = decoder_.any_frame() ? decoder_.frame() : local_frame_;
    local_frame_.popup = gps::PopupKind::Error;
    local_frame_.popup_text = message;
    local_frame_.version = (1u << 30) + 1 + decoder_.frame().version;
    render_frame(local_frame_);
    nav_bar().set_icon(2, "OK", back_cb, this);
}

// --- sudo password prompt (mirrors BleScanScreen / ToolRunScreen) ------------------

void GpsAppScreen::show_sudo_dialog() {
    if (sudo_modal_) {
        return;
    }
    platform::set_text_input_mode(true);

    sudo_modal_ = lv_obj_create(root());
    lv_obj_set_size(sudo_modal_, view::kScreenWidth, view::kScreenHeight);
    lv_obj_set_pos(sudo_modal_, 0, 0);
    lv_obj_set_style_bg_color(sudo_modal_, lv_obj_get_style_bg_color(root(), LV_PART_MAIN), 0);
    lv_obj_set_style_bg_opa(sudo_modal_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sudo_modal_, 0, 0);
    lv_obj_set_style_pad_all(sudo_modal_, 0, 0);
    lv_obj_clear_flag(sudo_modal_, LV_OBJ_FLAG_SCROLLABLE);

    auto* title = lv_label_create(sudo_modal_);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "Sudo password needed");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    sudo_ta_ = lv_textarea_create(sudo_modal_);
    lv_obj_set_size(sudo_ta_, LV_PCT(88), 34);
    lv_obj_align(sudo_ta_, LV_ALIGN_TOP_MID, 0, 34);
    lv_textarea_set_one_line(sudo_ta_, true);
    lv_textarea_set_password_mode(sudo_ta_, true);
    lv_obj_set_style_text_font(sudo_ta_, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(sudo_ta_, sudo_submit_cb, LV_EVENT_READY, this);

    auto* hint = lv_label_create(sudo_modal_);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_label_set_text(hint, "device default is often 'pi'");
    lv_obj_align_to(hint, sudo_ta_, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    auto* keys = lv_label_create(sudo_modal_);
    lv_obj_set_style_text_font(keys, &lv_font_montserrat_12, 0);
    lv_label_set_text(keys, "ESC cancel - Enter OK");
    lv_obj_align_to(keys, hint, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    nav_bar().set_icon(0, view::NAV_BACK, sudo_cancel_cb, this);
    nav_bar().set_icon(2, view::NAV_RUN, sudo_submit_cb, this);
    nav_bar().set_enabled(1, false);
    nav_bar().set_enabled(3, false);
    nav_bar().set_enabled(4, false);

    sudo_group_ = lv_group_create();
    lv_group_add_obj(sudo_group_, sudo_ta_);
    lv_group_focus_obj(sudo_ta_);
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, sudo_group_);
        }
    }
}

void GpsAppScreen::close_sudo_dialog() {
    if (!sudo_modal_) {
        return;
    }
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, nullptr);
        }
    }
    lv_group_delete(sudo_group_);
    sudo_group_ = nullptr;
    lv_obj_delete(sudo_modal_);
    sudo_modal_ = nullptr;
    sudo_ta_ = nullptr;
    platform::set_text_input_mode(false);
    nav_bar().set_enabled(4, true);
    // Back to the run screen's nav: Back only.
    configure_common_nav(view::NAV_BACK, back_cb, this);
    for (size_t slot = 1; slot <= 3; ++slot) {
        nav_bar().set_icon(slot, "", nullptr, nullptr);
    }
}

void GpsAppScreen::submit_sudo_dialog() {
    if (!sudo_modal_) {
        return;
    }
    const std::string password = lv_textarea_get_text(sudo_ta_);
    if (password.empty()) {
        return;
    }
    close_sudo_dialog();
    used_password_ = true;
    launch_wardrive(pending_argv_, password + "\n");
}

void GpsAppScreen::sudo_submit_cb(lv_event_t* event) {
    auto* self = static_cast<GpsAppScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->submit_sudo_dialog();
    }
}

void GpsAppScreen::sudo_cancel_cb(lv_event_t* event) {
    auto* self = static_cast<GpsAppScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->leave();
    }
}

} // namespace screen
