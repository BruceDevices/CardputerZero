/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"
#include "rfid_st25r.h"
#include "tool_catalog.h"

#include <cstdint>
#include <string>
#include <vector>

namespace screen {

// Tag-O-Matic-style live ST25R3916 reader, ported from the Bruce firmware's
// RFID app. Unlike the one-shot --rfid-* tool-run log, this screen drives the
// chip from the LVGL timer and keeps the reader loop running on its own: it
// polls the field continuously with no key press (firmware "Approach the Card
// you want to read"), and once a tag answers it freezes the result and pauses
// the loop for 5 s so the operator can lift the tag and decide what to do,
// then resumes polling automatically.
//
// Three modes (model::RfidTool), chosen by the catalog row that opened it:
//   Read  - UID / ATQA / SAK / type, re-reading on every new tag.
//   Dump  - the above plus the Type-2 (NTAG/Ultralight) data pages.
//   Clone - read one UID, then hand the bus to a `--rfid-emulate <uid>` child
//           so the device advertises that UID until the operator goes Back.
class RfidScreen : public BaseScreen {
public:
    RfidScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~RfidScreen() override;

private:
    enum class Phase { Error, Polling, Hold, Emulating };

    void build_content(lv_obj_t* content) override;

    void start_polling();
    void tick();
    void on_tag(const platform::St25rScan& scan);
    void enter_hold();
    void start_emulation(const platform::St25rScan& scan);
    void build_error_view(const std::string& report);
    void stop_all();
    void handle_back();
    void scroll_info(int dir); // +1 down, -1 up
    void save_tag();
    void set_save_enabled(bool enabled);

    std::string scan_to_text(const platform::St25rScan& scan) const;

    static void back_cb(lv_event_t* event);
    static void up_cb(lv_event_t* event);
    static void down_cb(lv_event_t* event);
    static void save_cb(lv_event_t* event);

    model::RfidTool tool_ = model::RfidTool::None;

    Phase phase_ = Phase::Error;
    bool session_open_ = false;
    uint32_t hold_until_ = 0; // lv_tick deadline while Phase::Hold

    platform::St25rScan last_scan_; // most recent read, for Save
    bool have_scan_ = false;
    bool save_enabled_ = false;

    std::string emulate_uid_hex_;   // UID being emulated (Clone)
    unsigned long activations_ = 0; // reader activations seen while emulating

    lv_timer_t* timer_ = nullptr;
    lv_obj_t* content_root_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* info_box_ = nullptr;
    lv_obj_t* info_ = nullptr;
};

} // namespace screen
