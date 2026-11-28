/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "rfid_screen.h"

#include "theme.h"
#include "ui_const.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace screen {
namespace {

constexpr int32_t kContentH = view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight;
constexpr uint32_t kPollPeriodMs = 250; // reader loop cadence
constexpr uint32_t kHoldMs = 5000;      // pause after a read so the tag can be lifted

const char* mode_title(model::RfidTool tool) {
    switch (tool) {
        case model::RfidTool::Read:  return "Read tag";
        case model::RfidTool::Dump:  return "Dump tag";
        case model::RfidTool::Clone: return "Clone tag";
        default:                     return "RFID";
    }
}

} // namespace

RfidScreen::RfidScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

RfidScreen::~RfidScreen() {
    stop_all();
}

void RfidScreen::build_content(lv_obj_t* content) {
    content_root_ = content;
    const auto& category = model::tool_catalog().at(view_model().active_category());
    const auto& action = category.actions.at(view_model().active_item());
    tool_ = action.rfid_tool;

    const auto pal = view::palette(view_model().is_dark_mode());

    title_ = lv_label_create(content_root_);
    lv_obj_set_style_text_font(title_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_, pal.primary, 0);
    lv_label_set_text(title_, mode_title(tool_));
    lv_obj_align(title_, LV_ALIGN_TOP_LEFT, 8, 2);

    status_ = lv_label_create(content_root_);
    lv_obj_set_width(status_, view::kScreenWidth - 16);
    lv_obj_set_style_text_font(status_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_, pal.text, 0);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
    lv_label_set_text(status_, "Starting reader...");
    lv_obj_align(status_, LV_ALIGN_TOP_LEFT, 8, 20);

    info_box_ = lv_obj_create(content_root_);
    lv_obj_set_size(info_box_, LV_PCT(100), kContentH - 40);
    lv_obj_align(info_box_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(info_box_, 4, 0);
    lv_obj_set_style_bg_opa(info_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_box_, 0, 0);
    lv_obj_set_scroll_dir(info_box_, LV_DIR_VER);

    info_ = lv_label_create(info_box_);
    lv_obj_set_width(info_, LV_PCT(100));
    lv_label_set_long_mode(info_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(info_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(info_, pal.text, 0);
    lv_label_set_text(info_, "");

    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, view::NAV_UP, up_cb, this);
    nav_bar().set_icon(2, "", nullptr, nullptr);
    nav_bar().set_icon(3, view::NAV_DOWN, down_cb, this);
    nav_bar().set_icon(4, "Save", save_cb, this);
    nav_bar().set_enabled(4, false); // enabled once a tag has been read

    std::string report;
    session_open_ = platform::st25r_session_open(&report);
    if (!session_open_) {
        build_error_view(report);
        return;
    }
    start_polling();
}

void RfidScreen::start_polling() {
    phase_ = Phase::Polling;
    lv_label_set_text(status_, "Approach a tag...");
    if (!timer_) {
        timer_ = lv_timer_create([](lv_timer_t* timer) {
            static_cast<RfidScreen*>(lv_timer_get_user_data(timer))->tick();
        }, kPollPeriodMs, this);
    }
    if (!timer_) {
        lv_label_set_text(status_, "Could not start reader timer");
        phase_ = Phase::Error;
    }
}

void RfidScreen::tick() {
    if (phase_ == Phase::Hold) {
        const int32_t remaining = static_cast<int32_t>(hold_until_ - lv_tick_get());
        if (remaining <= 0) {
            start_polling();
        } else {
            lv_label_set_text_fmt(status_, "Tag read - lift it (%ds)", (remaining + 999) / 1000);
        }
        return;
    }
    if (phase_ == Phase::Emulating) {
        if (platform::st25r_emulate_step()) {
            ++activations_;
            lv_label_set_text_fmt(status_, "Emulating %s  -  reads: %lu  (Back to stop)",
                                  emulate_uid_hex_.c_str(), activations_);
        }
        return;
    }
    if (phase_ != Phase::Polling) {
        return;
    }
    platform::St25rScan scan;
    // Clone also dumps the pages so the emulator can serve them to the reader.
    const bool want_pages = tool_ == model::RfidTool::Dump || tool_ == model::RfidTool::Clone;
    if (platform::st25r_session_poll(scan, want_pages)) {
        on_tag(scan);
    }
}

void RfidScreen::on_tag(const platform::St25rScan& scan) {
    last_scan_ = scan;
    have_scan_ = true;
    lv_label_set_text(info_, scan_to_text(scan).c_str());
    lv_obj_scroll_to_y(info_box_, 0, LV_ANIM_OFF);
    if (tool_ == model::RfidTool::Clone) {
        start_emulation(scan);
    } else {
        set_save_enabled(true);
        enter_hold();
    }
}

void RfidScreen::enter_hold() {
    phase_ = Phase::Hold;
    hold_until_ = lv_tick_get() + kHoldMs;
    lv_label_set_text(status_, "Tag read - lift it (5s)");
}

void RfidScreen::start_emulation(const platform::St25rScan& scan) {
    // Emulate in-process on the already-open session: no second process, so no
    // SPI/GPIO hand-off race (the subprocess approach failed to start because
    // the child could not take the bus). The reader timer keeps running and
    // drives st25r_emulate_step() while in Phase::Emulating.
    emulate_uid_hex_ = platform::to_hex(scan.uid.data(), scan.uid.size());
    activations_ = 0;

    if (!session_open_ || !platform::st25r_emulate_begin(scan)) {
        lv_label_set_text(status_, "Could not start emulation");
        phase_ = Phase::Error;
        return;
    }
    phase_ = Phase::Emulating;
    lv_label_set_text(info_, scan_to_text(scan).c_str());
    lv_label_set_text_fmt(status_, "Emulating %s  (Back to stop)", emulate_uid_hex_.c_str());
}

std::string RfidScreen::scan_to_text(const platform::St25rScan& scan) const {
    std::string out = "UID:  " + platform::to_hex(scan.uid.data(), scan.uid.size(), true) + "\n";
    char line[80];
    std::snprintf(line, sizeof(line), "ATQA: %02X %02X\n", scan.atqa[0], scan.atqa[1]);
    out += line;
    std::snprintf(line, sizeof(line), "SAK:  %02X  ", scan.sak);
    out += line;
    out += scan.sak_text;
    if (tool_ == model::RfidTool::Dump) {
        if (scan.page_count > 0) {
            out += "\n\nPages:";
            for (unsigned p = 0; p < scan.page_count; ++p) {
                std::snprintf(line, sizeof(line), "\npg %02u: %s", p,
                              platform::to_hex(&scan.pages[p * 4], 4, true).c_str());
                out += line;
            }
        } else if (scan.t2t) {
            out += "\n\n(no pages read)";
        } else {
            out += "\n\nDump needs a Type-2 tag (SAK 0x00: NTAG/Ultralight).";
        }
    }
    return out;
}

void RfidScreen::build_error_view(const std::string& report) {
    phase_ = Phase::Error;
    lv_label_set_text(status_, "No ST25R reader detected");
    std::string body = report.empty() ? "Check the CAP/module seating, power and wiring,\n"
                                        "then re-open this screen."
                                      : report;
    lv_label_set_text(info_, body.c_str());
}

void RfidScreen::stop_all() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (session_open_) {
        platform::st25r_session_close(); // also ends emulation (field off)
        session_open_ = false;
    }
}

void RfidScreen::handle_back() {
    stop_all();
    view_model().go_back();
}

void RfidScreen::scroll_info(int dir) {
    if (info_box_) {
        lv_obj_scroll_by(info_box_, 0, -dir * 28, LV_ANIM_ON);
    }
}

void RfidScreen::set_save_enabled(bool enabled) {
    save_enabled_ = enabled;
    nav_bar().set_enabled(4, enabled);
}

void RfidScreen::save_tag() {
    if (!have_scan_ || !save_enabled_) {
        return;
    }
    const std::string path = platform::st25r_save_nfc(last_scan_, model::rfid_recordings_dir());
    if (path.empty()) {
        lv_label_set_text(status_, "Save failed (check storage)");
        return;
    }
    // While showing the saved path, keep the hold paused so the message stays up.
    phase_ = Phase::Hold;
    hold_until_ = lv_tick_get() + kHoldMs;
    lv_label_set_text_fmt(status_, "Saved %s", path.substr(path.find_last_of('/') + 1).c_str());
}

void RfidScreen::save_cb(lv_event_t* event) {
    auto* self = static_cast<RfidScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->save_tag();
    }
}

void RfidScreen::up_cb(lv_event_t* event) {
    auto* self = static_cast<RfidScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->scroll_info(-1);
    }
}

void RfidScreen::down_cb(lv_event_t* event) {
    auto* self = static_cast<RfidScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->scroll_info(1);
    }
}

void RfidScreen::back_cb(lv_event_t* event) {
    auto* self = static_cast<RfidScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->handle_back();
    }
}

} // namespace screen
