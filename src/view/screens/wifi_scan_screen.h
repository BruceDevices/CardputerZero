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

// Structured Wi-Fi scan UI behind the "Scan nearby networks" and
// "List SSIDs + signal" rows. Both scan `iw dev wlan0 scan` on entry (no Run
// press) and show a signal-sorted, selectable list instead of the raw log.
//
//  - ScanList: list on the left, the selected network's details on the right.
//  - SsidList: an "SSID (RSSI)" list; selecting one runs a targeted deeper scan
//              and shows it on a detail view. ESC returns to the cached list
//              (no re-scan of the whole band).
class WifiScanScreen : public BaseScreen {
public:
    WifiScanScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~WifiScanScreen() override;

private:
    struct Network {
        std::string ssid;   // empty => hidden
        std::string bssid;
        float signal = -200.0f; // dBm
        int freq = 0;           // MHz
        int channel = 0;
        std::string security;   // Open / WEP / WPA / WPA2 / WPA3
        std::string raw;        // full BSS block, for the detail view
    };

    enum class Phase { Scanning, List, Detail };

    void build_content(lv_obj_t* content) override;

    // --- scanning ---
    void start_scan(const model::ToolAction& action);
    void run_primary_scan();                   // populate the network list
    void run_detail_scan(const Network& net);  // SsidList: targeted deeper scan
    void on_scan_output(const std::string& text);
    void on_scan_exit();
    static void process_cb(void* owner, const std::string& text, bool exited);
    static std::vector<Network> parse_scan(const std::string& out);

    // --- views ---
    void clear_content();
    void show_scanning_banner(const std::string& command);
    void build_list_view();   // ScanList: left list + right detail panel
    void build_ssid_view();   // SsidList: full-width SSID (RSSI) list
    void build_detail_view(const std::string& title, const std::string& body);
    void populate_list();
    void apply_highlight();
    void update_detail_panel();          // ScanList live right panel
    std::string detail_text(const Network& n) const;
    std::string list_label(const Network& n) const;

    void move_selection(int delta);
    void select_current();
    void handle_back();

    // --- on-device sudo password prompt (root scans) ---
    void show_sudo_dialog();
    void close_sudo_dialog();
    void submit_sudo_dialog();
    void launch_scan(const std::vector<std::string>& argv, const std::string& stdin_payload);

    // nav callbacks
    static void back_cb(lv_event_t* event);
    static void up_cb(lv_event_t* event);
    static void down_cb(lv_event_t* event);
    static void select_cb(lv_event_t* event);
    static void rescan_cb(lv_event_t* event);
    static void sudo_submit_cb(lv_event_t* event);
    static void sudo_cancel_cb(lv_event_t* event);

    model::WifiView mode_ = model::WifiView::None;
    int category_index_ = -1;
    int item_index_ = -1;
    Phase phase_ = Phase::Scanning;

    platform::ProcessRunner runner_;
    std::string scan_buffer_;
    bool detail_scan_ = false;   // the in-flight scan feeds the detail view
    bool root_needed_ = false;   // the scan command needs sudo
    std::vector<std::string> pending_argv_; // held while the sudo dialog is up
    bool used_password_ = false; // the in-flight scan was started with `sudo -S`
    std::string scan_error_;     // raw output of a scan that parsed no networks

    std::vector<Network> networks_;
    size_t selected_ = 0;

    lv_obj_t* content_root_ = nullptr; // the screen's content area (parent of views)
    lv_obj_t* list_ = nullptr;        // lv_list of rows
    std::vector<lv_obj_t*> rows_;
    lv_obj_t* detail_box_ = nullptr;  // scrollable container (right panel / detail view)
    lv_obj_t* detail_label_ = nullptr;
    lv_obj_t* banner_ = nullptr;

    // sudo modal
    lv_obj_t* sudo_modal_ = nullptr;
    lv_obj_t* sudo_ta_ = nullptr;
    lv_group_t* sudo_group_ = nullptr;
};

} // namespace screen
