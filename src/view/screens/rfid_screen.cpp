/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "rfid_screen.h"

#include "theme.h"
#include "ui_const.h"

#include <algorithm>
#include <cctype>
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
        case model::RfidTool::Load:  return "Load Tag";
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
    nav_bar().set_icon(3, view::NAV_DOWN, down_cb, this);
    if (tool_ == model::RfidTool::Load) {
        lv_obj_set_width(title_, view::kScreenWidth - 16);
        lv_label_set_long_mode(title_, LV_LABEL_LONG_DOT);
        nav_bar().set_icon(2, view::NAV_SELECT, select_cb, this);
        list_ = lv_list_create(content_root_);
        lv_obj_set_size(list_, LV_PCT(100), kContentH - 40);
        lv_obj_align(list_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(list_, 0, 0);
        lv_obj_set_style_pad_row(list_, 2, 0);
        std::error_code ec;
        directory_ = std::filesystem::absolute(model::rfid_recordings_dir(), ec);
        if (ec) directory_ = model::rfid_recordings_dir();
        show_file_picker();
        return;
    }
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
    start_timer();
}

bool RfidScreen::start_timer() {
    if (!timer_) {
        timer_ = lv_timer_create([](lv_timer_t* timer) {
            static_cast<RfidScreen*>(lv_timer_get_user_data(timer))->tick();
        }, kPollPeriodMs, this);
    }
    if (!timer_) {
        lv_label_set_text(status_, "Could not start reader timer");
        phase_ = Phase::Error;
    }
    return timer_ != nullptr;
}

void RfidScreen::tick() {
    if (phase_ == Phase::Writing) {
        std::string report;
        const auto result = platform::st25r_session_write_tag(loaded_scan_, report);
        if (result == platform::St25rWriteResult::WaitingForTag) return;
        stop_all();
        phase_ = Phase::WriteResult;
        lv_label_set_text(status_, result == platform::St25rWriteResult::Written ? "Write complete" : "Write failed");
        lv_label_set_text(info_, report.c_str());
        return;
    }
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
            lv_label_set_text_fmt(status_, "Emulating %s  -  selections: %lu  (Back to stop)",
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
    // The listener owns the open session. The UI timer only reports selections.
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

void RfidScreen::add_menu_row(const std::string& label, bool available) {
    auto* row = lv_list_add_button(list_, nullptr, label.c_str());
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_ver(row, 3, 0);
    lv_obj_set_style_text_font(row, &lv_font_montserrat_14, 0);
    if (!available) lv_obj_set_style_text_opa(row, LV_OPA_40, 0);
    rows_.push_back(row);
}

void RfidScreen::highlight_selection() {
    for (size_t i = 0; i < rows_.size(); ++i) {
        auto* row = rows_[i];
        lv_obj_set_style_bg_opa(row, i == selected_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (i == selected_) {
            lv_obj_set_style_bg_color(row, view::palette(true).primary, 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_style_text_color(row, lv_color_white(), 0);
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        } else {
            lv_obj_remove_local_style_prop(row, LV_STYLE_TEXT_COLOR, 0);
        }
    }
}

void RfidScreen::move_selection(int delta) {
    if (phase_ != Phase::FilePicker && phase_ != Phase::TagMenu) {
        scroll_info(delta);
        return;
    }
    if (rows_.empty()) return;
    const int count = static_cast<int>(rows_.size());
    selected_ = (static_cast<int>(selected_) + delta + count) % count;
    highlight_selection();
}

void RfidScreen::show_file_picker() {
    phase_ = Phase::FilePicker;
    lv_obj_add_flag(info_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(list_);
    rows_.clear();
    files_.clear();
    nav_bar().set_enabled(2, true);
    lv_label_set_text(title_, "Load Tag");

    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry_error;
        const auto& path = it->path();
        const std::string name = path.filename().string();
        if (name.empty() || name.front() == '.') continue;
        if (it->is_directory(entry_error)) {
            files_.push_back({path, name + "/", true});
        } else if (it->is_regular_file(entry_error)) {
            auto extension = path.extension().string();
            for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (extension == ".nfc") files_.push_back({path, name, false});
        }
    }
    std::sort(files_.begin(), files_.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.directory != b.directory) return a.directory;
        return a.label < b.label;
    });
    const auto parent = directory_.parent_path();
    if (!parent.empty() && parent != directory_) files_.insert(files_.begin(), {parent, "../", true});
    std::string status = "Choose a .nfc file";
    if (ec) status = "Folder unavailable";
    else if (std::all_of(files_.begin(), files_.end(), [](const FileEntry& entry) { return entry.directory; })) {
        status = "No .nfc files in this folder";
    }
    lv_label_set_text(status_, status.c_str());
    selected_ = files_.size() > 1 && files_.front().label == "../" ? 1 : 0;
    for (const auto& entry : files_) {
        if (entry.path == loaded_path_) selected_ = rows_.size();
        add_menu_row(entry.label);
    }
    nav_bar().set_enabled(2, !rows_.empty());
    lv_obj_update_layout(list_);
    highlight_selection();
}

void RfidScreen::show_tag_menu() {
    phase_ = Phase::TagMenu;
    lv_obj_add_flag(info_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(list_);
    rows_.clear();
    nav_bar().set_enabled(2, true);
    lv_label_set_text(title_, loaded_path_.filename().string().c_str());
    const auto uid = platform::to_hex(loaded_scan_.uid.data(), loaded_scan_.uid.size(), true);
    lv_label_set_text_fmt(status_, "UID: %s", uid.c_str());
    const bool can_emulate = loaded_scan_.uid.size() != 10;
    add_menu_row("Write Tag", loaded_scan_.t2t && loaded_scan_.page_count > 4);
    add_menu_row("Emulate Tag", can_emulate && loaded_scan_.t2t && loaded_scan_.page_count > 0);
    add_menu_row("Emulate UID", can_emulate);
    selected_ = loaded_scan_.page_count > 0 ? 0 : 2;
    lv_obj_update_layout(list_);
    highlight_selection();
}

void RfidScreen::select_row() {
    if (phase_ == Phase::FilePicker) {
        if (selected_ >= files_.size()) return;
        const auto entry = files_[selected_];
        if (entry.directory) {
            directory_ = entry.path;
            show_file_picker();
            return;
        }
        std::string error;
        if (!platform::st25r_load_nfc(entry.path.string(), loaded_scan_, error)) {
            lv_label_set_text(status_, error.c_str());
            return;
        }
        loaded_path_ = entry.path;
        show_tag_menu();
        return;
    }
    if (phase_ != Phase::TagMenu) return;

    const bool writing = selected_ == 0;
    if ((writing && (!loaded_scan_.t2t || loaded_scan_.page_count <= 4)) ||
        (selected_ == 1 && (!loaded_scan_.t2t || loaded_scan_.page_count == 0))) {
        lv_label_set_text(status_, "Select a Type-2 dump with data pages");
        return;
    }
    if (!writing && loaded_scan_.uid.size() == 10) {
        lv_label_set_text(status_, "Emulation supports 4- or 7-byte UIDs");
        return;
    }
    lv_obj_add_flag(list_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(info_box_, LV_OBJ_FLAG_HIDDEN);
    nav_bar().set_enabled(2, false);
    std::string report;
    session_open_ = platform::st25r_session_open(&report);
    if (!session_open_) {
        build_error_view(report);
        return;
    }
    if (!start_timer()) {
        stop_all();
        return;
    }
    if (writing) {
        phase_ = Phase::Writing;
        lv_label_set_text(status_, "Approach the destination tag...");
        lv_label_set_text(info_, "Hold a writable Type-2 tag in place until writing finishes.");
        return;
    }
    auto scan = loaded_scan_;
    if (selected_ == 2) {
        scan.pages.clear();
        scan.page_count = 0;
    }
    start_emulation(scan);
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
    if (tool_ == model::RfidTool::Load) {
        if (phase_ == Phase::TagMenu) {
            show_file_picker();
            return;
        }
        if (phase_ != Phase::FilePicker) {
            show_tag_menu();
            return;
        }
    }
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
        self->move_selection(-1);
    }
}

void RfidScreen::down_cb(lv_event_t* event) {
    auto* self = static_cast<RfidScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->move_selection(1);
    }
}

void RfidScreen::select_cb(lv_event_t* event) {
    auto* self = static_cast<RfidScreen*>(lv_event_get_user_data(event));
    if (self) self->select_row();
}

void RfidScreen::back_cb(lv_event_t* event) {
    auto* self = static_cast<RfidScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->handle_back();
    }
}

} // namespace screen
