/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "base_screen.h"
#include "process_runner.h"
#include "tool_catalog.h"

#include <string>
#include <vector>

namespace screen {

// Firmware-style BLE scan UI (Bruce firmware's ble_scan()): scan for 5s, then
// show a selectable list of nearby devices. Picking one opens a details view
// with the same fields as the firmware's ble_info(): Name / Adresse / Signal.
//
// The list shows only the device name, or the address when the device has no
// name - RSSI is intentionally kept out of the list, exactly like the
// firmware. A trailing "Main Menu" row mirrors addOptionToMainMenu().
class BleScanScreen : public BaseScreen {
public:
    BleScanScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~BleScanScreen() override;

private:
    struct Device {
        std::string name;    // empty => show the address / "<no name>"
        std::string address; // uppercase MAC
        int rssi = 0;        // dBm
    };

    enum class Phase { Scanning, List, Detail };

    void build_content(lv_obj_t* content) override;

    // --- scanning ---
    void run_scan();
    void start_scan(const model::ToolAction& action);
    void launch_scan(const std::vector<std::string>& argv, const std::string& stdin_payload);
    void on_scan_output(const std::string& text);
    void on_scan_exit();
    static void process_cb(void* owner, const std::string& text, bool exited);
    void parse_scan(const std::string& out);

    // --- views ---
    void clear_content();
    void show_scanning_banner();
    void build_list_view();
    void build_detail_view(size_t index);
    void build_empty_view(const std::string& title, const std::string& body);
    std::string detail_text(size_t index) const;
    void populate_list();
    void apply_highlight();
    lv_obj_t* selected_row() const;
    void move_selection(int delta);
    void select_current();
    void handle_back();

    // --- on-device sudo password prompt (root scan) ---
    void show_sudo_dialog();
    void close_sudo_dialog();
    void submit_sudo_dialog();

    // nav callbacks
    static void back_cb(lv_event_t* event);
    static void up_cb(lv_event_t* event);
    static void down_cb(lv_event_t* event);
    static void select_cb(lv_event_t* event);
    static void scan_cb(lv_event_t* event);
    static void sudo_submit_cb(lv_event_t* event);
    static void sudo_cancel_cb(lv_event_t* event);

    int category_index_ = -1;
    int item_index_ = -1;
    Phase phase_ = Phase::Scanning;

    model::ToolAction scan_action_;
    platform::ProcessRunner runner_;
    std::string scan_buffer_;
    std::string scan_error_;
    bool used_password_ = false;
    std::vector<std::string> pending_argv_;

    std::vector<Device> devices_;
    bool truncated_ = false;
    size_t selected_ = 0; // device index, or devices_.size() for "Main Menu"

    lv_obj_t* content_root_ = nullptr;
    lv_obj_t* list_ = nullptr;
    std::vector<lv_obj_t*> rows_;
    lv_obj_t* main_menu_row_ = nullptr;

    lv_obj_t* detail_box_ = nullptr;
    lv_obj_t* detail_title_ = nullptr;
    lv_obj_t* detail_label_ = nullptr;
    lv_obj_t* detail_hint_ = nullptr;

    // sudo modal
    lv_obj_t* sudo_modal_ = nullptr;
    lv_obj_t* sudo_ta_ = nullptr;
    lv_group_t* sudo_group_ = nullptr;
};

} // namespace screen
