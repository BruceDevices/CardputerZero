/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "wifi_scan_screen.h"

#include "asset_manager.h"
#include "linux_input.h"
#include "theme.h"
#include "ui_const.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace screen {
namespace {

constexpr int32_t kContentH = view::kScreenHeight - view::kTitleBarHeight - view::kNavBarHeight;
constexpr int32_t kListW    = 146; // left list width in the ScanList split view
constexpr int32_t kGap      = 4;

// The scan commands resolve the wireless interface through the shared
// model::wifi_interface() (not a hardcoded "wlan0"), so this screen works on a
// dev laptop too; see run_primary_scan / run_detail_scan.

// Strip leading whitespace (iw indents fields with tabs).
std::string ltrim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// sudo refused to run without a password (`sudo -n`) or rejected the one given.
bool sudo_needs_password(const std::string& out) {
    return out.find("a password is required") != std::string::npos ||
           out.find("a terminal is required") != std::string::npos;
}
bool sudo_password_rejected(const std::string& out) {
    return out.find("incorrect password") != std::string::npos ||
           out.find("Sorry, try again") != std::string::npos;
}

// Concise, tagged diagnostics on stderr (LVGL's own log is WARN+ only).
__attribute__((format(printf, 1, 2))) void scan_log(const char* fmt, ...) {
    std::fputs("[wifi-scan] ", stderr);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

int channel_for_freq(int freq) {
    if (freq <= 0) {
        return 0;
    }
    if (freq == 2484) {
        return 14;
    }
    if (freq >= 2412 && freq <= 2472) {
        return (freq - 2407) / 5;
    }
    if (freq >= 5000 && freq < 6000) {
        return (freq - 5000) / 5;
    }
    if (freq >= 5955) { // 6 GHz
        return (freq - 5950) / 5;
    }
    return 0;
}

} // namespace

WifiScanScreen::WifiScanScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

WifiScanScreen::~WifiScanScreen() {
    runner_.stop();
    if (sudo_group_) {
        lv_group_delete(sudo_group_);
        sudo_group_ = nullptr;
    }
}

void WifiScanScreen::build_content(lv_obj_t* content) {
    content_root_ = content;
    category_index_ = view_model().active_category();
    item_index_     = view_model().active_item();
    const auto& action = model::tool_catalog()[static_cast<size_t>(category_index_)]
                             .actions[static_cast<size_t>(item_index_)];
    mode_ = action.wifi_view;

    // Autonomous: scan on entry, no Run press.
    run_primary_scan();
}

// --- scanning ---------------------------------------------------------------

void WifiScanScreen::run_primary_scan() {
    // Build the scan command here (not the catalog's hardcoded "wlan0") so it
    // targets the real wireless interface and works on a dev laptop too.
    const std::string iface = model::wifi_interface();
    model::ToolAction a;
    a.argv = {"sh", "-c",
              "ip link set " + iface + " up 2>/dev/null; iw dev " + iface + " scan"};
    a.needs_root = true;
    detail_scan_ = false;
    start_scan(a);
}

void WifiScanScreen::run_detail_scan(const Network& net) {
    // Targeted deeper scan of one SSID. The SSID is passed as a distinct argv
    // word (never interpolated into a shell string), so a crafted SSID cannot
    // inject commands - escalated_argv sh-quotes each word.
    const std::string iface = model::wifi_interface();
    model::ToolAction a;
    a.argv = {"iw", "dev", iface, "scan", "ssid", net.ssid};
    a.needs_root = true;
    detail_scan_ = true;
    start_scan(a);
}

void WifiScanScreen::start_scan(const model::ToolAction& action) {
    phase_ = Phase::Scanning;
    show_scanning_banner(model::action_command_text(action));

    // Always keep the `sudo -S` form: if the passwordless launch is refused
    // after all (stale probe), on_scan_exit falls back to the dialog with it.
    const bool needs_root = model::action_needs_root(action);
    pending_argv_ = needs_root ? model::sudo_argv_with_password(action) : action.argv;
    const bool passwordless = needs_root && model::sudo_passwordless();
    scan_log("command: %s", model::action_command_text(action).c_str());
    scan_log("root needed=%s", needs_root ? (passwordless ? "yes, passwordless" : "yes, password") : "no");

    if (needs_root && !passwordless) {
        show_sudo_dialog();
        return;
    }
    used_password_ = false;
    launch_scan(model::launch_argv(action), "");
}

void WifiScanScreen::launch_scan(const std::vector<std::string>& argv,
                                 const std::string& stdin_payload) {
    scan_buffer_.clear();
    // argv[0..1] only ("sudo -n"/"sudo -S"): never the stdin payload.
    scan_log("launch: %s %s", argv.empty() ? "" : argv[0].c_str(),
             argv.size() > 1 ? argv[1].c_str() : "");
    if (!runner_.start(argv, &WifiScanScreen::process_cb, this, stdin_payload)) {
        scan_log("launch failed");
        if (banner_) {
            lv_label_set_text(banner_, "Failed to launch scan.");
        }
    }
}

void WifiScanScreen::process_cb(void* owner, const std::string& text, bool exited) {
    auto* self = static_cast<WifiScanScreen*>(owner);
    if (!self) {
        return;
    }
    self->on_scan_output(text);
    if (exited) {
        self->on_scan_exit();
    }
}

void WifiScanScreen::on_scan_output(const std::string& text) {
    scan_buffer_ += text;
}

void WifiScanScreen::on_scan_exit() {
    scan_log("exited, %zu bytes of output", scan_buffer_.size());

    // sudo refused: offer the password dialog instead of an empty result.
    if (used_password_ ? sudo_password_rejected(scan_buffer_)
                       : sudo_needs_password(scan_buffer_)) {
        scan_log("%s", used_password_ ? "sudo rejected the password" : "sudo -n refused, asking for password");
        if (banner_) {
            lv_label_set_text(banner_, used_password_ ? "Wrong sudo password, try again."
                                                      : "Sudo password needed.");
        }
        show_sudo_dialog();
        return;
    }

    if (detail_scan_) {
        std::vector<Network> found = parse_scan(scan_buffer_);
        std::string title = found.empty() ? "Network" : list_label(found.front());
        std::string body;
        if (!found.empty()) {
            body = detail_text(found.front());
            body += "\n\n--- raw ---\n";
            body += found.front().raw;
        } else {
            body = "No details returned.\n\n";
            body += scan_buffer_;
        }
        build_detail_view(title, body);
        return;
    }

    networks_ = parse_scan(scan_buffer_);
    scan_log("parsed %zu networks", networks_.size());
    // Nothing parsed: keep the command output so the failure is visible.
    scan_error_ = networks_.empty() ? scan_buffer_ : std::string();
    if (!scan_error_.empty()) {
        scan_log("output: %s", scan_error_.substr(0, 400).c_str());
    }
    std::sort(networks_.begin(), networks_.end(),
              [](const Network& a, const Network& b) { return a.signal > b.signal; });
    if (selected_ >= networks_.size()) {
        selected_ = 0;
    }
    if (mode_ == model::WifiView::ScanList) {
        build_list_view();
    } else {
        build_ssid_view();
    }
}

std::vector<WifiScanScreen::Network> WifiScanScreen::parse_scan(const std::string& out) {
    std::vector<Network> nets;
    Network cur;
    bool in_bss = false;
    bool has_rsn = false, has_wpa = false, has_privacy = false, has_sae = false;

    auto finish = [&]() {
        if (!in_bss) {
            return;
        }
        cur.channel = channel_for_freq(cur.freq);
        if (has_sae) {
            cur.security = has_rsn ? "WPA3" : "WPA3?";
        } else if (has_rsn) {
            cur.security = "WPA2";
        } else if (has_wpa) {
            cur.security = "WPA";
        } else if (has_privacy) {
            cur.security = "WEP";
        } else {
            cur.security = "Open";
        }
        nets.push_back(cur);
    };

    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (starts_with(line, "BSS ")) {
            finish();
            cur = Network{};
            has_rsn = has_wpa = has_privacy = has_sae = false;
            in_bss = true;
            // "BSS aa:bb:cc:dd:ee:ff(on wlan0) -- ..."
            std::string rest = line.substr(4);
            size_t paren = rest.find('(');
            cur.bssid = ltrim(paren == std::string::npos ? rest : rest.substr(0, paren));
            cur.raw = line + "\n";
            continue;
        }
        if (!in_bss) {
            continue;
        }
        cur.raw += line;
        cur.raw += "\n";

        const std::string t = ltrim(line);
        if (starts_with(t, "signal:")) {
            cur.signal = static_cast<float>(std::atof(t.c_str() + 7));
        } else if (starts_with(t, "freq:")) {
            cur.freq = std::atoi(t.c_str() + 5);
        } else if (starts_with(t, "SSID:")) {
            cur.ssid = ltrim(t.substr(5));
        } else if (starts_with(t, "RSN:")) {
            has_rsn = true;
        } else if (starts_with(t, "WPA:")) {
            has_wpa = true;
        } else if (t.find("Privacy") != std::string::npos && starts_with(t, "capability:")) {
            has_privacy = true;
        } else if (t.find("SAE") != std::string::npos) {
            has_sae = true;
        }
    }
    finish();
    return nets;
}

// --- views ------------------------------------------------------------------

void WifiScanScreen::clear_content() {
    if (content_root_) {
        lv_obj_clean(content_root_);
    }
    list_ = nullptr;
    detail_box_ = nullptr;
    detail_label_ = nullptr;
    banner_ = nullptr;
    rows_.clear();
}

void WifiScanScreen::show_scanning_banner(const std::string& command) {
    clear_content();
    banner_ = lv_label_create(content_root_);
    lv_obj_set_width(banner_, view::kScreenWidth - 16);
    lv_label_set_long_mode(banner_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(banner_, &lv_font_montserrat_12, 0);
    lv_obj_align(banner_, LV_ALIGN_TOP_LEFT, 8, 8);
    const std::string text = "Running command: \"" + command + "\", wait...";
    lv_label_set_text(banner_, text.c_str());

    // Only Back + Theme are meaningful while a scan is in flight.
    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, "", nullptr, nullptr);
    nav_bar().set_icon(2, "", nullptr, nullptr);
    nav_bar().set_icon(3, "", nullptr, nullptr);
    nav_bar().set_enabled(1, false);
    nav_bar().set_enabled(2, false);
    nav_bar().set_enabled(3, false);
}

void WifiScanScreen::build_list_view() {
    clear_content();
    phase_ = Phase::List;
    const auto pal = view::palette(view_model().is_dark_mode());

    list_ = lv_list_create(content_root_);
    lv_obj_set_size(list_, kListW, kContentH);
    lv_obj_align(list_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_, 0, 0);
    lv_obj_set_style_pad_all(list_, 2, 0);

    detail_box_ = lv_obj_create(content_root_);
    lv_obj_set_size(detail_box_, view::kScreenWidth - kListW - kGap, kContentH);
    lv_obj_align(detail_box_, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_pad_all(detail_box_, 4, 0);
    lv_obj_set_style_border_width(detail_box_, 1, 0);
    lv_obj_set_style_border_color(detail_box_, pal.border, 0);
    lv_obj_set_style_bg_opa(detail_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(detail_box_, LV_DIR_VER);

    detail_label_ = lv_label_create(detail_box_);
    lv_obj_set_width(detail_label_, LV_PCT(100));
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(detail_label_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(detail_label_, pal.text, 0);

    populate_list();
    update_detail_panel();

    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, view::NAV_UP, up_cb, this);
    nav_bar().set_icon(2, "Scan", rescan_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, down_cb, this);
}

void WifiScanScreen::build_ssid_view() {
    clear_content();
    phase_ = Phase::List;

    list_ = lv_list_create(content_root_);
    lv_obj_set_size(list_, view::kScreenWidth - 4, kContentH);
    lv_obj_align(list_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_, 0, 0);

    populate_list();

    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, view::NAV_UP, up_cb, this);
    nav_bar().set_icon(2, view::NAV_SELECT, select_cb, this);
    nav_bar().set_icon(3, view::NAV_DOWN, down_cb, this);
}

void WifiScanScreen::build_detail_view(const std::string& title, const std::string& body) {
    clear_content();
    phase_ = Phase::Detail;
    const auto pal = view::palette(view_model().is_dark_mode());

    detail_box_ = lv_obj_create(content_root_);
    lv_obj_set_size(detail_box_, view::kScreenWidth - 4, kContentH);
    lv_obj_align(detail_box_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(detail_box_, 4, 0);
    lv_obj_set_style_bg_opa(detail_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(detail_box_, 0, 0);
    lv_obj_set_scroll_dir(detail_box_, LV_DIR_VER);

    detail_label_ = lv_label_create(detail_box_);
    lv_obj_set_width(detail_label_, LV_PCT(100));
    lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(detail_label_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(detail_label_, pal.text, 0);
    const std::string text = title + "\n\n" + body;
    lv_label_set_text(detail_label_, text.c_str());

    configure_common_nav(view::NAV_BACK, back_cb, this);
    nav_bar().set_icon(1, view::NAV_UP, up_cb, this);
    nav_bar().set_icon(2, "", nullptr, nullptr);
    nav_bar().set_enabled(2, false);
    nav_bar().set_icon(3, view::NAV_DOWN, down_cb, this);
}

void WifiScanScreen::populate_list() {
    rows_.clear();
    if (networks_.empty()) {
        auto* row = lv_list_add_text(list_, "No networks found");
        LV_UNUSED(row);
        if (!scan_error_.empty() && mode_ == model::WifiView::SsidList) {
            // SsidList has no side panel: show the failure under the row.
            const std::string msg = "Scan failed:\n" + scan_error_.substr(0, 400);
            lv_list_add_text(list_, msg.c_str());
        }
        return;
    }
    lv_obj_set_style_pad_row(list_, 2, 0);
    for (const auto& net : networks_) {
        const std::string label = list_label(net);
        auto* row = lv_list_add_button(list_, nullptr, label.c_str());
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(row, &lv_font_montserrat_12, 0);
        lv_obj_set_style_pad_ver(row, 3, 0); // compact rows so more networks fit
        rows_.push_back(row);
    }
    apply_highlight();
}

void WifiScanScreen::apply_highlight() {
    const auto pal = view::palette(view_model().is_dark_mode());
    for (size_t i = 0; i < rows_.size(); ++i) {
        lv_obj_t* row = rows_[i];
        if (i == selected_) {
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, pal.primary, 0);
            lv_obj_set_style_radius(row, 4, 0);
            lv_obj_set_style_text_color(row, lv_color_white(), 0);
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_remove_local_style_prop(row, LV_STYLE_TEXT_COLOR, 0);
        }
    }
}

void WifiScanScreen::update_detail_panel() {
    if (!detail_label_) {
        return;
    }
    if (networks_.empty()) {
        const std::string msg = scan_error_.empty()
            ? std::string("No networks found.\nPress Scan to retry.")
            : "Scan failed:\n" + scan_error_.substr(0, 600);
        lv_label_set_text(detail_label_, msg.c_str());
        return;
    }
    lv_label_set_text(detail_label_, detail_text(networks_[selected_]).c_str());
}

std::string WifiScanScreen::list_label(const Network& n) const {
    const std::string ssid = n.ssid.empty() ? "<hidden>" : n.ssid;
    if (mode_ == model::WifiView::SsidList) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), " (%d)", static_cast<int>(n.signal));
        return ssid + buf;
    }
    return ssid;
}

std::string WifiScanScreen::detail_text(const Network& n) const {
    std::string out;
    out += "SSID: " + (n.ssid.empty() ? std::string("<hidden>") : n.ssid) + "\n";
    out += "BSSID: " + n.bssid + "\n";
    char line[64];
    std::snprintf(line, sizeof(line), "Signal: %d dBm\n", static_cast<int>(n.signal));
    out += line;
    std::snprintf(line, sizeof(line), "Channel: %d  (%d MHz)\n", n.channel, n.freq);
    out += line;
    out += "Security: " + n.security;
    return out;
}

// --- navigation -------------------------------------------------------------

void WifiScanScreen::move_selection(int delta) {
    if (phase_ == Phase::Detail) {
        if (detail_box_) {
            lv_obj_scroll_by(detail_box_, 0, -delta * 24, LV_ANIM_ON);
        }
        return;
    }
    if (networks_.empty() || rows_.empty()) {
        return;
    }
    int count = static_cast<int>(networks_.size());
    int next = (static_cast<int>(selected_) + delta) % count;
    if (next < 0) {
        next += count;
    }
    selected_ = static_cast<size_t>(next);
    apply_highlight();
    if (mode_ == model::WifiView::ScanList) {
        update_detail_panel();
    }
}

void WifiScanScreen::select_current() {
    if (phase_ != Phase::List || mode_ != model::WifiView::SsidList) {
        return;
    }
    if (networks_.empty() || selected_ >= networks_.size()) {
        return;
    }
    const Network net = networks_[selected_];
    if (net.ssid.empty()) {
        // No SSID to probe - show what the first scan already captured.
        std::string body = detail_text(net) + "\n\n--- raw ---\n" + net.raw;
        build_detail_view(list_label(net), body);
        return;
    }
    run_detail_scan(net);
}

void WifiScanScreen::handle_back() {
    if (phase_ == Phase::Detail) {
        // Return to the cached list without re-scanning the band.
        build_ssid_view();
        return;
    }
    if (phase_ == Phase::Scanning && detail_scan_) {
        // Cancel a targeted detail scan: fall back to the list we already have.
        runner_.stop();
        detail_scan_ = false;
        build_ssid_view();
        return;
    }
    runner_.stop();
    view_model().go_back();
}

// --- sudo password prompt (mirrors ToolRunScreen) ---------------------------

void WifiScanScreen::show_sudo_dialog() {
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
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_set_group(indev, sudo_group_);
        }
    }
}

void WifiScanScreen::close_sudo_dialog() {
    if (!sudo_modal_) {
        return;
    }
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev;
         indev = lv_indev_get_next(indev)) {
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

    // The result views repoint every nav slot; nothing to restore here beyond
    // re-enabling the theme toggle in case the caller stays on the banner.
    nav_bar().set_enabled(4, true);
}

void WifiScanScreen::submit_sudo_dialog() {
    if (!sudo_modal_) {
        return;
    }
    const std::string password = lv_textarea_get_text(sudo_ta_);
    if (password.empty()) {
        return;
    }
    close_sudo_dialog();
    used_password_ = true;
    launch_scan(pending_argv_, password + "\n");
}

void WifiScanScreen::back_cb(lv_event_t* event) {
    auto* self = static_cast<WifiScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->handle_back();
    }
}

void WifiScanScreen::up_cb(lv_event_t* event) {
    auto* self = static_cast<WifiScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->move_selection(-1);
    }
}

void WifiScanScreen::down_cb(lv_event_t* event) {
    auto* self = static_cast<WifiScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->move_selection(1);
    }
}

void WifiScanScreen::select_cb(lv_event_t* event) {
    auto* self = static_cast<WifiScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->select_current();
    }
}

void WifiScanScreen::rescan_cb(lv_event_t* event) {
    auto* self = static_cast<WifiScanScreen*>(lv_event_get_user_data(event));
    if (self && !self->runner_.running()) {
        self->run_primary_scan();
    }
}

void WifiScanScreen::sudo_submit_cb(lv_event_t* event) {
    auto* self = static_cast<WifiScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->submit_sudo_dialog();
    }
}

void WifiScanScreen::sudo_cancel_cb(lv_event_t* event) {
    auto* self = static_cast<WifiScanScreen*>(lv_event_get_user_data(event));
    if (self) {
        self->close_sudo_dialog();
        self->handle_back();
    }
}

} // namespace screen
