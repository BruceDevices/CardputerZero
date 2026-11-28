/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Self-check of the Bruce Wardriving port (platform/wardrive.*) and the shared
 * bluetoothctl parser (platform/ble_scan_parser.*): parsers against captured
 * iw / bluetoothctl output (tests/fixtures), the WiGLE 1.6 rows, the screen
 * protocol, and Bruce's loop driven by a virtual clock and a scripted GPS.
 * Synthetic coordinates only. Not part of the CMake build. Run from the repo
 * root:
 *
 *   g++ -std=c++17 -pthread -Isrc/platform tests/wardrive_selfcheck.cpp \
 *       src/platform/wardrive.cpp src/platform/ble_scan_parser.cpp src/platform/gps_session.cpp \
 *       src/platform/gps_parser.cpp src/platform/gps_port.cpp src/platform/gps_tracker.cpp \
 *       src/platform/gpx_writer.cpp src/platform/uart_speed.cpp \
 *       -o /tmp/wardrive_check && /tmp/wardrive_check
 */

#include "ble_scan_parser.h"
#include "wardrive.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <deque>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace platform;
namespace wd = platform::wardrive;
namespace gps = platform::gps;

namespace {

int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                              \
        }                                                                              \
    } while (0)

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// NMEA with a valid checksum.
std::string nmea(const std::string& body) {
    unsigned char sum = 0;
    for (char c : body) sum ^= static_cast<unsigned char>(c);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "*%02X\r\n", sum);
    return "$" + body + buf;
}

// Synthetic positions near 0 N / 0 E (open sea), ddmm.mmmmm.
std::string fix_at(const char* hhmmss, const char* lat, const char* lon, const char* date = "221126") {
    return nmea(std::string("GNGGA,") + hhmmss + ".00," + lat + ",N," + lon + ",E,1,07,1.20,12.3,M,0.0,M,,") +
           nmea(std::string("GNRMC,") + hhmmss + ".00,A," + lat + ",N," + lon + ",E,0.50,10.0," + date + ",,,A,V");
}
std::string nofix_at(const char* hhmmss, const char* date = "221126") {
    return nmea(std::string("GNGGA,") + hhmmss + ".00,,,,,0,00,25.50,,,,,,") +
           nmea(std::string("GNRMC,") + hhmmss + ".00,V,,,,,,," + date + ",,,N,V");
}

std::string tmpdir(const char* tag) {
    std::string t = "/tmp/wardrive_check_" + std::string(tag) + "_" + std::to_string(::getpid());
    std::string cmd = "rm -rf '" + t + "' && mkdir -p '" + t + "'";
    CHECK(std::system(cmd.c_str()) == 0);
    return t;
}

bool has_line(const gps::ScreenFrame& f, const std::string& text) {
    for (const auto& l : f.lines) {
        if (l.text == text) return true;
    }
    return false;
}

// --- scripted environment for Wardriver ---------------------------------------
struct Sim {
    uint64_t now = 0;
    std::deque<std::pair<uint64_t, std::string>> feed; // (time ms, bytes)
    gps::Parser parser;
    bool hang_up_when_drained = false;
    uint64_t stop_at = UINT64_MAX;
    bool open_ok = true;
    bool open_not_found = false;
    std::string open_message;
    std::vector<std::vector<ScanAp>> wifi; // per scan call (last one repeats)
    int wifi_calls = 0;
    bool wifi_stops = false;
    bool ble_ok = true;
    ble::ScanResult ble_result;
    int ble_calls = 0;
    std::vector<gps::ScreenFrame> frames;
    std::vector<std::string> popups; // every popup text drawn
    std::vector<std::string> serial;
    std::vector<std::string> rssi_queries;
    // RSSI the hook learns for a device the scan reported without one
    // (bluetoothctl info during discovery); 0 = none learnt.
    int lookup_rssi = -42;
    int lookup_after_calls = 0; // the lookups learn nothing in the first N passes
    wd::Storage* storage = nullptr; // when set, delay() waits for its worker

    bool esc() const { return now >= stop_at; }

    wd::Wardriver::Hooks hooks() {
        wd::Wardriver::Hooks h;
        h.millis = [this]() { return now; };
        // Virtual time only moves once the real storage worker has finished
        // what was queued, so "the next tick" is deterministic.
        h.delay = [this](uint32_t ms) {
            if (storage) storage->wait_idle(10000);
            now += ms;
        };
        h.esc = [this]() { return esc(); };
        h.open_gps = [this](std::string& m, bool& nf) {
            m = open_message;
            nf = open_not_found;
            return open_ok;
        };
        h.gps = [this](const std::function<void(size_t, bool, gps::Parser&)>& fn) {
            size_t bytes = 0;
            while (!feed.empty() && feed.front().first <= now) {
                parser.encode(feed.front().second.data(), feed.front().second.size());
                bytes += feed.front().second.size();
                feed.pop_front();
            }
            fn(bytes, hang_up_when_drained && feed.empty(), parser);
        };
        h.scan_wifi = [this](bool& ok) {
            ok = true;
            now += 3000; // WiFi.scanNetworks() takes a few seconds
            if (wifi_stops) stop_at = now;
            if (wifi.empty()) return std::vector<ScanAp>{};
            const size_t i = std::min<size_t>(static_cast<size_t>(wifi_calls++), wifi.size() - 1);
            return wifi[i];
        };
        h.scan_ble = [this](int seconds, const std::function<bool(const std::string&)>& need_rssi,
                            ble::ScanResult& out) {
            now += static_cast<uint64_t>(seconds) * 1000;
            ++ble_calls;
            out = ble_result;
            for (auto& d : out.devices) {
                if (d.rssi == 0 && need_rssi(d.mac)) {
                    rssi_queries.push_back(d.mac);
                    d.rssi = ble_calls > lookup_after_calls ? lookup_rssi : 0;
                }
            }
            return ble_ok;
        };
        h.draw = [this](const gps::ScreenFrame& f, bool, bool) {
            frames.push_back(f);
            if (f.popup != gps::PopupKind::None &&
                (popups.empty() || popups.back() != f.popup_text)) {
                popups.push_back(f.popup_text);
            }
        };
        h.serial = [this](const std::string& s) { serial.push_back(s); };
        return h;
    }
};

ScanAp ap(const char* mac, const char* ssid, const char* auth = "WPA2_PSK", int ch = 6, int freq = 2437,
          int rssi = -50) {
    ScanAp a;
    a.bssid = mac;
    a.ssid = ssid;
    a.auth = auth;
    a.channel = ch;
    a.freq = freq;
    a.signal = rssi;
    return a;
}

wd::Wardriver::Options wifi_opts() {
    wd::Wardriver::Options o;
    o.scan_wifi = true;
    o.scan_ble = false;
    o.current_year = 2026;
    o.app_version = "9.9.9";
    return o;
}

bool popup_seen(const Sim& s, const std::string& text) {
    for (const auto& p : s.popups) {
        if (p == text) return true;
    }
    return false;
}

bool line_seen(const Sim& s, const std::string& text) {
    for (const auto& f : s.frames) {
        if (has_line(f, text)) return true;
    }
    return false;
}

} // namespace

int main() {
    // --- freq -> channel ---
    CHECK(freq_to_channel(2412) == 1);
    CHECK(freq_to_channel(2484) == 14);
    CHECK(freq_to_channel(5180) == 36);
    CHECK(freq_to_channel(5745) == 149);
    CHECK(freq_to_channel(5955) == 1);
    CHECK(freq_to_channel(1234) == 0);

    // --- Bruce AuthMode names from iw's RSN/WPA suites ---
    CHECK(bruce_auth_mode(false, false, "", false, "") == "OPEN");
    CHECK(bruce_auth_mode(true, false, "", false, "") == "WEP");
    CHECK(bruce_auth_mode(true, false, "", true, " PSK") == "WPA_PSK");
    CHECK(bruce_auth_mode(true, true, " PSK", false, "") == "WPA2_PSK");
    CHECK(bruce_auth_mode(true, true, " PSK/SHA-256", false, "") == "WPA2_PSK");
    CHECK(bruce_auth_mode(true, true, " FT/PSK", false, "") == "WPA2_PSK");
    CHECK(bruce_auth_mode(true, true, " PSK", true, " PSK") == "WPA_WPA2_PSK");
    CHECK(bruce_auth_mode(true, true, " SAE", false, "") == "WPA3_PSK");
    CHECK(bruce_auth_mode(true, true, " PSK SAE", false, "") == "WPA2_WPA3_PSK");
    CHECK(bruce_auth_mode(true, true, " IEEE 802.1X", false, "") == "WPA2_ENTERPRISE");
    CHECK(bruce_auth_mode(true, false, "", true, " IEEE 802.1X") == "WPA2_ENTERPRISE");
    CHECK(bruce_auth_mode(true, true, " FILS/SHA-256", false, "") == "WPA2_ENTERPRISE");
    CHECK(bruce_auth_mode(true, true, " OWE", false, "") == "UNKNOWN"); // no Bruce name
    CHECK(bruce_auth_mode(true, true, "", false, "") == "UNKNOWN");

    // --- iw SSID escapes ---
    CHECK(decode_iw_ssid("Home") == "Home");
    CHECK(decode_iw_ssid("\\x20lead") == " lead");
    CHECK(decode_iw_ssid("\\xe3\\x83\\x86") == "\xe3\x83\x86");
    CHECK(decode_iw_ssid("a\\x0ab") == "a\\x0ab"); // control bytes stay escaped
    CHECK(decode_iw_ssid("\\x00\\x00") == "");     // hidden
    CHECK(decode_iw_ssid("") == "");
    CHECK(decode_iw_ssid("back\\x5cslash") == "back\\slash");

    // --- iw scan fixture (one real iw 6.17 block + iw-format blocks) ---
    const std::string iw = slurp("tests/fixtures/iw_scan_6.17.txt");
    CHECK(!iw.empty());
    const auto aps = parse_iw_scan(iw);
    CHECK(aps.size() == 10);
    CHECK(aps[0].bssid == "02:00:5e:20:00:01" && aps[0].ssid == "TestNet-1");
    CHECK(aps[0].freq == 2437 && aps[0].channel == 6 && aps[0].signal == -36);
    CHECK(aps[0].auth == "WPA_WPA2_PSK"); // real block: RSN PSK + WPA PSK
    CHECK(aps[1].ssid == "Cafe, \"Free\"" && aps[1].auth == "WPA3_PSK" && aps[1].freq == 5180 && aps[1].channel == 36);
    CHECK(aps[2].auth == "WPA2_WPA3_PSK" && aps[2].channel == 149);
    CHECK(aps[3].auth == "WPA2_ENTERPRISE" && aps[3].channel == 1);
    CHECK(aps[4].auth == "WEP");
    CHECK(aps[5].auth == "OPEN" && aps[5].ssid == "Open " && aps[5].channel == 14 && aps[5].freq == 2484);
    CHECK(aps[6].auth == "UNKNOWN" && aps[6].freq == 5955);
    CHECK(aps[7].auth == "WPA_PSK" && aps[7].ssid == "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"); // WPS suites ignored
    CHECK(aps[8].ssid.empty() && aps[8].auth == "WPA2_PSK");
    CHECK(aps[9].ssid == "line\\x0abreak back\\slash" && aps[9].auth == "OPEN");

    // --- WiGLE 1.6 rows (Bruce's format strings) ---
    wd::Fix fix;
    fix.lat = 0.012345;
    fix.lng = 0.06789;
    fix.alt = 12.3;
    fix.hdop = 1.2;
    fix.year = 2026, fix.month = 11, fix.day = 22, fix.hour = 15, fix.minute = 20, fix.second = 2;
    CHECK(wd::wifi_row(aps[0], fix) ==
          "02:00:5E:20:00:01,\"TestNet-1\",[WPA_WPA2_PSK],2026-11-22 15:20:02,6,2437,-36,0.012345,0.067890,"
          "12.300000,1.200000,,,WIFI\n");
    CHECK(wd::wifi_row(aps[1], fix) ==
          "02:00:5E:20:00:02,\"Cafe, \"\"Free\"\"\",[WPA3_PSK],2026-11-22 15:20:02,36,5180,-61,0.012345,0.067890,"
          "12.300000,1.200000,,,WIFI\n");
    CHECK(wd::csv_quoted("a\"b\nc") == "a\"\"b c");
    ble::Device bd;
    bd.mac = "02:00:5E:10:00:13";
    bd.name = "Tag \"1\"";
    bd.rssi = -70;
    bd.mfgr_id = 0x004c;
    CHECK(wd::ble_row(bd, fix) ==
          "02:00:5e:10:00:13,\"Tag \"\"1\"\"\",Misc [BLE],2026-11-22 15:20:02,0,,-70,0.012345,0.067890,"
          "12.300000,1.200000,,004C,BLE\n");
    bd.mfgr_id = 0;
    bd.name.clear();
    CHECK(wd::ble_row(bd, fix) ==
          "02:00:5e:10:00:13,\"\",Misc [BLE],2026-11-22 15:20:02,0,,-70,0.012345,0.067890,12.300000,1.200000,,,BLE\n");
    CHECK(wd::wigle_header("1.2.3") ==
          "WigleWifi-1.6,appRelease=v1.2.3,model=CardputerZero,release=v1.2.3,device=CardputerZero,"
          "display=ST7789 LCD,board=CardputerZero (CM0),brand=Bruce,star=Sol,body=4,subBody=1\r\n"
          "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,"
          "AccuracyMeters,RCOIs,MfgrId,Type\r\n");

    // --- filename, MAC keys, alerts ---
    CHECK(wd::wardriving_filename(2026, 11, 22, 15, 20, 2) == "261122_152002_wardriving.csv");
    CHECK(wd::wardriving_filename(2000, 1, 2, 3, 4, 5) == "000102_030405_wardriving.csv");
    uint64_t key = 0;
    CHECK(wd::parse_mac_u64("02:00:5E:10:00:13", key) && key == 0x02005e100013ull);
    CHECK(wd::parse_mac_u64("02-00-5e-10-00-13", key) && key == 0x02005e100013ull);
    CHECK(!wd::parse_mac_u64("02:00:5E:10:00", key));
    CHECK(!wd::parse_mac_u64("02:00:5E:10:00:13:14", key));
    CHECK(!wd::parse_mac_u64("zz:00:5E:10:00:13", key));
    const auto alerts = wd::parse_alert_macs(wd::alert_sample() + "  02:00:5E:20:00:02 \r\n\n#x\n02:00:5e:10:00:13\n");
    CHECK(alerts.size() == 2 && alerts.count("02:00:5e:20:00:02") && alerts.count("02:00:5e:10:00:13"));
    CHECK(wd::parse_alert_macs(wd::alert_sample()).empty());
    CHECK(wd::alert_sample() ==
          "# Alert MAC addresses - one per line\r\n# Lines starting with # are comments\r\n# Example:\r\n"
          "# aa:bb:cc:dd:ee:ff\r\n");

    // --- bluetoothctl 5.85 fixture ---
    const std::string bt = slurp("tests/fixtures/bluetoothctl_scan_le_5.85.txt");
    CHECK(!bt.empty());
    {
        const auto r = ble::parse_scan(bt);
        CHECK(r.error.empty() && !r.truncated);
        CHECK(r.devices.size() == 15); // 16 MACs in the log, one is the controller
        CHECK(r.devices[0].mac == "02:00:5E:10:00:02" && r.devices[0].name.empty());
        bool named = false, mfgr = false, apple = false;
        for (const auto& d : r.devices) {
            if (d.mac == "02:00:5E:10:00:05") {
                named = d.name == "Name-05";
                mfgr = d.mfgr_id == 0x0600 && d.rssi != 0;
            }
            if (d.mac == "02:00:5E:10:00:0B") apple = d.mfgr_id == 0x0969;
        }
        CHECK(named && mfgr && apple);
        const auto capped = ble::parse_scan(bt, {5, false});
        CHECK(capped.devices.size() == 5 && capped.truncated);
    }
    {
        // Cached devices printed before discovery starts are not "seen".
        const std::string log = "[NEW] Device 02:00:5E:30:00:01 Cached\n"
                                "[NEW] Device 02:00:5E:30:00:02 CachedAndSeen\n"
                                "Discovery started\n"
                                "[CHG] Controller 02:00:5E:30:00:FF Discovering: yes\n"
                                "[CHG] Device 02:00:5E:30:00:02 RSSI: 0xffffffb4 (-76)\n"
                                "[CHG] Device 02:00:5E:30:00:02 ManufacturerData Key: 0x0075\n"
                                "[NEW] Device 02:00:5E:30:00:03 02-00-5E-30-00-03\n";
        ble::ParseOptions po;
        po.only_seen_in_scan = true;
        const auto r = ble::parse_scan(log, po);
        CHECK(r.devices.size() == 2);
        CHECK(r.devices[0].mac == "02:00:5E:30:00:02" && r.devices[0].rssi == -76 && r.devices[0].mfgr_id == 0x75);
        CHECK(r.devices[1].name.empty());
        CHECK(ble::parse_scan(log).devices.size() == 3); // the BLE Scan screen keeps all
        const auto err = ble::parse_scan("No default controller available\n");
        CHECK(err.devices.empty() && err.error == "No default controller available\n");
    }

    // --- screen protocol round trip (any chunking) ---
    {
        gps::ScreenFrame f;
        f.title = "Wardriving";
        f.lines = {{1, ""}, {1, "Found WiFi: 1"}, {2, "Date: 2026-11-22"}};
        f.popup = gps::PopupKind::Error;
        f.popup_text = "ALERT: WiFi found! Name: a\nb MAC: 02:00:5E:20:00:01";
        const std::string wire = wd::encode_frame(f, false, true) + "log line\n" + wd::encode_frame(f, true, false) +
                                 "@X storage\n";
        wd::FrameDecoder d;
        for (char c : wire) d.feed(std::string(1, c));
        CHECK(d.any_frame() && d.frame().title == "Wardriving" && d.frame().lines.size() == 3);
        CHECK(d.frame().lines[2].padx == 2 && d.frame().lines[2].text == "Date: 2026-11-22");
        CHECK(d.frame().lines[0].text.empty());
        CHECK(d.frame().popup == gps::PopupKind::Error && d.popup_waits());
        CHECK(d.frame().popup_text == "ALERT: WiFi found! Name: a b MAC: 02:00:5E:20:00:01");
        CHECK(!d.gps_connected() && d.ended() && d.end_reason() == "storage");
        CHECK(d.log().size() == 1 && d.log()[0] == "log line");
        wd::FrameDecoder partial;
        partial.feed("@F Wardriving\n@1 x\n");
        CHECK(!partial.any_frame()); // applied only at "@."
    }

    // --- line fitting for the 6-line body ---
    {
        std::vector<gps::ScreenLine> pass = {{1, ""}, {1, "File: 261122_152002_wardriving"},
                                             {1, "Found WiFi: 42 BLE: 17 Alert: 1"},
                                             {1, "Distance: 1.37km  ET: 00:12:09"}, {1, "GPS location updated"},
                                             {1, "Coord: 0.012345, 0.067890"}, {1, "Start Scanning..."},
                                             {1, "Scan done. WiFi: 9 BLE: 4"}};
        auto out = gps::fit_lines(pass, 6);
        CHECK(out.size() == 6 && out[3].text == "GPS location updated" && out[5].text == "Scan done. WiFi: 9 BLE: 4");
        pass.insert(pass.begin() + 7, gps::ScreenLine{1, "MAC cache cleared to prevent heap overflow"});
        out = gps::fit_lines(pass, 6);
        CHECK(out.size() == 6 && out[3].text == "Coord: 0.012345, 0.067890" && out[5].text == "Scan done. WiFi: 9 BLE: 4");
        // while scanning, "Start Scanning..." stays
        CHECK(out[4].text == "MAC cache cleared to prevent heap overflow");
        std::vector<gps::ScreenLine> scanning(pass.begin(), pass.begin() + 7);
        out = gps::fit_lines(scanning, 6);
        CHECK(out.back().text == "Start Scanning...");
        std::vector<gps::ScreenLine> nofix = {{1, ""}, {1, "File: 261122_152002_wardriving"}, {1, "Found WiFi: 0"},
                                              {1, "Distance: 0.00km  ET: 00:00:31"}, {1, "GPS location not updated"},
                                              {2, "Date: 2026-11-22"}, {2, "Time: 15:20:02"}, {2, "Sat:  0"},
                                              {2, "HDOP: 25.50"}};
        out = gps::fit_lines(nofix, 6);
        CHECK(out.size() == 6 && out[4].text == "Date: 2026-11-22   Time: 15:20:02" &&
              out[5].text == "Sat:  0   HDOP: 25.50");
    }

    // === Wardriver loop (virtual clock) ========================================

    // 1. No GPS data: "Waiting GPS: Ns" once a second until ESC.
    {
        const std::string home = tmpdir("wait");
        wd::Storage st(home);
        Sim s;
        s.stop_at = 10500;
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.end_reason() == wd::Wardriver::EndReason::Stopped);
        // displayTextLine() adds 200 ms to each 1 s tick (core/display.cpp:387-395).
        CHECK(popup_seen(s, "Waiting GPS: 0s") && popup_seen(s, "Waiting GPS: 8s"));
        CHECK(!popup_seen(s, "Waiting GPS: 9s"));
        CHECK(!s.serial.empty() && s.serial[0] == "MESSAGE: Waiting GPS: 0s");
        const auto& f0 = s.frames.front();
        CHECK(f0.title == "Wardriving" && f0.lines.size() >= 4);
        CHECK(f0.lines[0].text.empty() && f0.lines[1].text == "Found WiFi: 0");
        CHECK(f0.lines[2].text == "Distance: 0.00km  ET: 00:00:00" && f0.lines[3].text == "Initializing...");
        CHECK(line_seen(s, "Waiting for GPS data"));
        CHECK(!w.gps_connected());
        // loadAlertMACs(): the sample alert.txt is created on the first run.
        CHECK(slurp(home + "/BruceWardriving/alert.txt") == wd::alert_sample());
    }

    // 2. Data without a fix: date/time block, filename from GPS UTC when the
    //    year is in [CURRENT_YEAR, +5); "No GPS data available" x6 after the
    //    data stops, then "GPS not Found!".
    {
        const std::string home = tmpdir("nofix");
        wd::Storage st(home);
        Sim s;
        s.feed.push_back({0, "garbage\r\n"}); // bytes without a sentence
        s.feed.push_back({4000, nofix_at("152002")});
        s.feed.push_back({9000, nofix_at("152007")});
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.end_reason() == wd::Wardriver::EndReason::NotFound);
        CHECK(line_seen(s, "Waiting for valid GPS data"));
        CHECK(line_seen(s, "Date: 2026-11-22") && line_seen(s, "Time: 15:20:02") && line_seen(s, "Sat:  0") &&
              line_seen(s, "HDOP: 25.50"));
        CHECK(w.filename() == "261122_152002_wardriving.csv");
        CHECK(line_seen(s, "File: 261122_152002_wardriving"));
        int no_data = 0;
        for (const auto& f : s.frames) {
            if (!f.lines.empty() && f.lines.back().text == "No GPS data available") ++no_data;
        }
        CHECK(no_data == 6); // count 0..5, the 7th empty pass ends it (count > 5)
        CHECK(s.popups.back() == "GPS not Found!");
        // ET counts from the first GPS data (sessionStartMs).
        CHECK(line_seen(s, "Distance: 0.00km  ET: 00:00:05"));
        // no fix -> no CSV
        struct stat sb{};
        CHECK(::stat((home + "/BruceWardriving/261122_152002_wardriving.csv").c_str(), &sb) != 0);
    }
    {
        // A year outside [CURRENT_YEAR, +5) names no file.
        const std::string home = tmpdir("year");
        wd::Storage st(home);
        Sim s;
        s.feed.push_back({0, nofix_at("152002")});
        s.stop_at = 12000;
        auto o = wifi_opts();
        o.current_year = 2031;
        wd::Wardriver w(o, s.hooks(), st);
        w.run();
        CHECK(w.filename().empty());
    }

    // 3. Fixes: Distance, rows, header once, dedup, alerts, "Scan done.".
    {
        const std::string home = tmpdir("fix");
        ::mkdir((home + "/BruceWardriving").c_str(), 0755);
        {
            std::ofstream a(home + "/BruceWardriving/alert.txt");
            a << "# mine\r\n02:00:5E:20:00:02\r\n";
        }
        // With an owner the home is used by a forked helper with that user's
        // credentials (here our own: the switch is allowed unprivileged).
        // With test_switch_to_self the home is used by the forked helper with
        // (our own) user credentials, as for a root run.
        wd::StorageOptions me;
        me.test_switch_to_self = true;
        wd::Storage st(home, me);
        Sim s;
        s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        s.feed.push_back({8000, fix_at("152010", "0000.80000", "00004.07340")});
        s.stop_at = 20000;
        s.wifi = {{aps[0], aps[1]}, {aps[0], aps[1], aps[2]}};
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.end_reason() == wd::Wardriver::EndReason::Stopped);
        CHECK(line_seen(s, "Loaded 1 alert MACs"));
        CHECK(line_seen(s, "GPS location updated"));
        CHECK(line_seen(s, "Coord: 0.012345, 0.067890"));
        CHECK(line_seen(s, "Start Scanning..."));
        CHECK(line_seen(s, "Scan done. WiFi: 2") && line_seen(s, "Scan done. WiFi: 3"));
        CHECK(w.wifi_count() == 3 && w.alert_count() == 1);
        CHECK(popup_seen(s, "ALERT: WiFi found! Name: Cafe, \"Free\" MAC: 02:00:5E:20:00:02"));
        CHECK(line_seen(s, "Found WiFi: 3 Alert: 1"));
        const double d = gps::Parser::distanceBetween(0.012345, 0.06789, 0.013333333, 0.06789);
        char want[64];
        std::snprintf(want, sizeof(want), "Distance: %.2fkm", d / 1000);
        bool dist = false;
        for (const auto& f : s.frames)
            for (const auto& l : f.lines) dist = dist || l.text.rfind(want, 0) == 0;
        CHECK(dist && std::abs(w.distance_m() - d) < 0.01 && d > 100);
        const std::string csv = slurp(home + "/BruceWardriving/261122_152002_wardriving.csv");
        CHECK(csv.rfind(wd::wigle_header("9.9.9"), 0) == 0);
        CHECK(csv.find("WigleWifi-1.6", 10) == std::string::npos); // header once
        size_t rows = 0;
        for (size_t p = 0; (p = csv.find(",WIFI\n", p)) != std::string::npos; ++p) ++rows;
        CHECK(rows == 3);
        CHECK(csv.find("02:00:5E:20:00:01,\"TestNet-1\",[WPA_WPA2_PSK],2026-11-22 15:20:02,6,2437,-36,0.012345,"
                       "0.067890,12.300000,1.200000,,,WIFI\n") != std::string::npos);
        CHECK(csv.find("02:00:5E:20:00:03,\"Transition\",[WPA2_WPA3_PSK],2026-11-22 15:20:10,149,5745,-70,0.013333,")
              != std::string::npos);
        struct stat sb{};
        CHECK(::stat((home + "/BruceWardriving/261122_152002_wardriving.csv").c_str(), &sb) == 0);
        CHECK((sb.st_mode & 0777) == 0644 && sb.st_uid == ::getuid());
        CHECK(slurp(home + "/BruceWardriving/alert.txt") == "# mine\r\n02:00:5E:20:00:02\r\n"); // untouched
    }

    // 4. MAC cache: 250 entries, then cleared with Bruce's message.
    {
        const std::string home = tmpdir("cache");
        wd::Storage st(home);
        Sim s;
        s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        s.stop_at = 9000;
        std::vector<ScanAp> many;
        for (int i = 0; i < 300; ++i) {
            char mac[18];
            std::snprintf(mac, sizeof(mac), "02:00:5e:40:%02x:%02x", i / 256, i % 256);
            many.push_back(ap(mac, "n"));
        }
        many.push_back(ap("not-a-mac", "bad")); // unparseable: always logged, never cached
        s.wifi = {many};
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.mac_cache_clears() == 1 && w.wifi_count() == 301);
        CHECK(line_seen(s, "MAC cache cleared to prevent heap overflow"));
        CHECK(line_seen(s, "Scan done. WiFi: 301"));
    }

    // 5. BLE and Both: rows, "Scan done. WiFi: n BLE: m", BLE init failure.
    {
        const std::string home = tmpdir("ble");
        wd::Storage st(home);
        Sim s;
        s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        s.stop_at = 15000; // a second pass draws the banner with the counts
        s.wifi = {{aps[0]}};
        ble::ParseOptions po;
        po.only_seen_in_scan = true;
        s.ble_result = ble::parse_scan(bt, po);
        auto o = wifi_opts();
        o.scan_ble = true;
        wd::Wardriver w(o, s.hooks(), st);
        w.run();
        const int n = static_cast<int>(s.ble_result.devices.size());
        CHECK(line_seen(s, "Scan done. WiFi: 1 BLE: " + std::to_string(n)));
        CHECK(w.ble_count() == n && w.wifi_count() == 1);
        CHECK(line_seen(s, "Found WiFi: 1 BLE: " + std::to_string(n)));
        const std::string csv = slurp(home + "/BruceWardriving/261122_152002_wardriving.csv");
        CHECK(csv.find("02:00:5e:10:00:05,\"Name-05\",Misc [BLE],2026-11-22 15:20:02,0,,") != std::string::npos);
        CHECK(csv.find(",,0600,BLE\n") != std::string::npos && csv.find(",,0969,BLE\n") != std::string::npos);
        // Devices the scan reported without an RSSI were queried once each
        // (bluetoothctl info), the others not.
        CHECK(s.rssi_queries.size() == 5);
        CHECK(std::find(s.rssi_queries.begin(), s.rssi_queries.end(), "02:00:5E:10:00:05") == s.rssi_queries.end());
        CHECK(csv.find("02:00:5e:10:00:02,\"\",Misc [BLE],2026-11-22 15:20:02,0,,-42,") != std::string::npos);
        CHECK(csv.find(",0,,0,") == std::string::npos); // no BLE row with RSSI 0

        Sim f;
        f.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        f.stop_at = 12000;
        f.ble_ok = false;
        auto bo = wifi_opts();
        bo.scan_wifi = false;
        bo.scan_ble = true;
        const std::string home2 = tmpdir("blefail");
        wd::Storage st2(home2);
        wd::Wardriver w2(bo, f.hooks(), st2);
        w2.run();
        CHECK(f.serial.size() == 2 && f.serial[0] == " Failed to init BLE" && f.serial[1] == "ERR: Failed to init BLE");
        CHECK(popup_seen(f, "Failed to init BLE")); // deviation: Bruce logs it on Serial only
        for (const auto& fr : f.frames)
            for (const auto& l : fr.lines) CHECK(l.text.rfind("Scan done.", 0) != 0);
        CHECK(line_seen(f, "Found BLE: 0"));
    }

    // 5b. A device whose RSSI is not known (bluetoothctl printed none and the
    //     lookup during discovery learnt none) is not logged with RSSI 0, not
    //     counted and not marked as found: a later pass that learns its RSSI
    //     logs it (Bruce always has NimBLE's advertised RSSI).
    {
        const std::string home = tmpdir("blenorssi");
        wd::Storage st(home);
        Sim s;
        s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        s.feed.push_back({10000, fix_at("152012", "0000.80000", "00004.07340")});
        s.feed.push_back({20000, fix_at("152022", "0000.90000", "00004.07340")});
        s.stop_at = 40000;
        s.lookup_after_calls = 1; // pass 1 learns nothing, pass 2 learns -42
        s.ble_result = ble::parse_scan(bt, {100, true});
        auto o = wifi_opts();
        o.scan_wifi = false;
        o.scan_ble = true;
        wd::Wardriver w(o, s.hooks(), st);
        w.run();
        const int n = static_cast<int>(s.ble_result.devices.size());
        int without = 0;
        for (const auto& d : s.ble_result.devices) without += d.rssi == 0;
        CHECK(n == 15 && without == 5);
        CHECK(s.ble_calls >= 3);
        // Pass 1 asked for the 5 and logged the 10 others; pass 2 asked for
        // the same 5 again (not found yet) and logged them; pass 3 asked for none.
        CHECK(static_cast<int>(s.rssi_queries.size()) == 2 * without);
        CHECK(w.ble_count() == n);
        CHECK(line_seen(s, "Scan done. BLE: " + std::to_string(n))); // the scan's count, as Bruce
        const std::string csv = slurp(home + "/BruceWardriving/261122_152002_wardriving.csv");
        size_t rows = 0;
        for (size_t p = csv.find(",BLE\n"); p != std::string::npos; p = csv.find(",BLE\n", p + 1)) ++rows;
        CHECK(rows == static_cast<size_t>(n));
        CHECK(csv.find(",0,,0,") == std::string::npos);
        // Pass 1 logged the 10 with a scan RSSI; pass 2 the 5 looked up.
        const size_t r05 = csv.find("02:00:5e:10:00:05,\"Name-05\",Misc [BLE],2026-11-22 15:20:02,0,,-");
        const size_t r02 = csv.find("02:00:5e:10:00:02,\"\",Misc [BLE],2026-11-22 15:20:12,0,,-42,");
        CHECK(r05 != std::string::npos && r02 != std::string::npos && r05 < r02);

        // Never learnt: never logged.
        const std::string home2 = tmpdir("blenorssi2");
        wd::Storage st2(home2);
        Sim z;
        z.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        z.feed.push_back({10000, fix_at("152012", "0000.80000", "00004.07340")});
        z.stop_at = 25000;
        z.lookup_rssi = 0;
        z.ble_result = s.ble_result;
        wd::Wardriver w2(o, z.hooks(), st2);
        w2.run();
        CHECK(w2.ble_count() == n - without);
        CHECK(slurp(home2 + "/BruceWardriving/261122_152002_wardriving.csv").find(",0,,0,") == std::string::npos);
    }

    // 6. Storage errors wait for a key and end the run.
    {
        Sim s;
        s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        wd::Storage st("/nonexistent/home");
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.end_reason() == wd::Wardriver::EndReason::StorageError);
        CHECK(w.popup_waits() && w.frame().popup_text == "Storage setup error");
        CHECK(line_seen(s, "Storage setup error"));
        CHECK(s.wifi_calls == 0);
    }
    {
        // ~/BruceWardriving is a symlink (root must not follow it): open fails.
        const std::string home = tmpdir("link");
        const std::string target = tmpdir("linktarget");
        CHECK(::symlink(target.c_str(), (home + "/BruceWardriving").c_str()) == 0);
        Sim s;
        s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        wd::Storage st(home);
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.end_reason() == wd::Wardriver::EndReason::StorageError);
        CHECK(w.popup_waits() && w.frame().popup_text == "Failed to open file for writing");
        CHECK(line_seen(s, "Failed to open file for writing"));
        CHECK(slurp(target + "/alert.txt").empty()); // nothing written through the link
    }
    if (::geteuid() != 0) {
        // A directory we cannot write to (as root this would not fail).
        const std::string home = tmpdir("ro");
        ::mkdir((home + "/BruceWardriving").c_str(), 0555);
        Sim s;
        s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        wd::Storage st(home);
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.frame().popup_text == "Failed to open file for writing");
        ::chmod((home + "/BruceWardriving").c_str(), 0755);
    }

    // 7. Open errors (Linux): missing node -> "GPS not Found!", others wait.
    {
        Sim s;
        s.open_ok = false;
        s.open_not_found = true;
        wd::Storage st(tmpdir("open1"));
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.end_reason() == wd::Wardriver::EndReason::NotFound && w.frame().popup_text == "GPS not Found!");
        CHECK(!w.popup_waits());
        Sim p;
        p.open_ok = false;
        p.open_message = "Cannot open /dev/ttyACM0: permission denied - add your user to the dialout group";
        wd::Storage st2(tmpdir("open2"));
        wd::Wardriver w2(wifi_opts(), p.hooks(), st2);
        w2.run();
        CHECK(w2.end_reason() == wd::Wardriver::EndReason::OpenFailed && w2.popup_waits());
        CHECK(w2.frame().popup_text == p.open_message);
        // Auto: "Searching GPS..." while probing.
        Sim a;
        a.feed.push_back({0, nofix_at("152002")});
        a.stop_at = 3000;
        auto o = wifi_opts();
        o.auto_port = true;
        wd::Storage st3(tmpdir("open3"));
        wd::Wardriver w3(o, a.hooks(), st3);
        w3.run();
        CHECK(!a.popups.empty() && a.popups.front() == "Searching GPS...");
    }

    // 8. Hang-up (USB GPS unplugged) -> "GPS not Found!" at once.
    {
        Sim s;
        s.feed.push_back({0, nofix_at("152002")});
        s.hang_up_when_drained = true;
        wd::Storage st(tmpdir("hup"));
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.end_reason() == wd::Wardriver::EndReason::NotFound);
        CHECK(s.now < 7000); // one data pass, then the hang-up is reported
    }

    // 9. ESC during a scan: that scan is not logged, the run stops.
    {
        const std::string home = tmpdir("escscan");
        wd::Storage st(home);
        Sim s;
        s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        s.wifi = {{aps[0], aps[1]}};
        s.wifi_stops = true;
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.end_reason() == wd::Wardriver::EndReason::Stopped && w.wifi_count() == 0);
        const std::string csv = slurp(home + "/BruceWardriving/261122_152002_wardriving.csv");
        CHECK(csv == wd::wigle_header("9.9.9"));
    }


    // === review fixes ============================================================

    // R1. Storage: no hard links, no non-regular files, no symlinked folder
    //     or file, O_EXCL creation (an existing file is appended, never
    //     truncated or re-headed), the forked user-credential opener.
    {
        const std::string home = tmpdir("store");
        const std::string d = home + "/BruceWardriving";
        wd::StorageOptions me;
        me.test_switch_to_self = true;
        for (int pass = 0; pass < 2; ++pass) {
            wd::Storage st(home, pass == 0 ? wd::StorageOptions{} : me);
            CHECK(std::system(("rm -rf '" + d + "'").c_str()) == 0);
            bool is_new = false;
            // creates the folder and the file (O_EXCL)
            int fd = st.open("a.csv", is_new);
            CHECK(fd >= 0 && is_new);
            CHECK(st.write(fd, "HEADER\n"));
            st.close(fd);
            CHECK(st.wait_idle(5000));
            struct stat sb{};
            CHECK(::stat((d + "/a.csv").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0644 && sb.st_uid == ::getuid());
            CHECK(::stat(d.c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0755);
            // absent -> existing: a file that appears is appended, not truncated
            { std::ofstream f(d + "/b.csv"); f << "KEEP\n"; }
            fd = st.open("b.csv", is_new);
            CHECK(fd >= 0 && !is_new);
            CHECK(st.write(fd, "ROW\n"));
            st.close(fd);
            CHECK(st.wait_idle(5000));
            CHECK(slurp(d + "/b.csv") == "KEEP\nROW\n");
            // hard link (to a file outside the folder): refused, target untouched
            { std::ofstream f(home + "/victim"); f << "ORIGINAL"; }
            CHECK(::link((home + "/victim").c_str(), (d + "/c.csv").c_str()) == 0);
            CHECK(st.open("c.csv", is_new) < 0 && st.last_errno() == EMLINK);
            CHECK(slurp(home + "/victim") == "ORIGINAL");
            CHECK(::stat((d + "/c.csv").c_str(), &sb) == 0 && sb.st_nlink == 2); // not removed: it existed
            // symlinked file: refused, target untouched
            CHECK(::symlink((home + "/victim").c_str(), (d + "/s.csv").c_str()) == 0);
            CHECK(st.open("s.csv", is_new) < 0 && st.last_errno() == ELOOP);
            CHECK(slurp(home + "/victim") == "ORIGINAL");
            // FIFO and directory: refused without blocking
            CHECK(::mkfifo((d + "/f.csv").c_str(), 0644) == 0);
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(st.open("f.csv", is_new) < 0);
            std::string txt;
            int err = 0;
            CHECK(!st.read("f.csv", txt, &err) && err == EINVAL); // opened O_NONBLOCK, then rejected
            CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
            CHECK(::mkdir((d + "/x.csv").c_str(), 0755) == 0);
            CHECK(st.open("x.csv", is_new) < 0);
            // missing file: ENOENT (alert.txt -> the sample is created)
            CHECK(!st.read("none.txt", txt, &err) && err == ENOENT);
            CHECK(st.create("n.txt", "one") && !st.create("n.txt", "two") && slurp(d + "/n.txt") == "one");
            // the folder replaced by a symlink (intermediate component): refused
            CHECK(std::system(("rm -rf '" + d + "' && mkdir -p '" + home + "/other' && ln -s '" + home + "/other' '" +
                               d + "'").c_str()) == 0);
            CHECK(st.open("a.csv", is_new) < 0 && (st.last_errno() == ELOOP || st.last_errno() == ENOTDIR));
            CHECK(!st.read("alert.txt", txt, &err) && err != ENOENT); // no sample through it
            CHECK(slurp(home + "/other/a.csv").empty() && slurp(home + "/other/alert.txt").empty());
            CHECK(::unlink(d.c_str()) == 0);
        }
        // bounded wait for slow storage (sync on the worker)
        wd::StorageOptions so;
        so.sync_delay_ms = 800;
        wd::Storage slow(home, so);
        bool is_new = false;
        const int fd = slow.open("slow.csv", is_new);
        CHECK(fd >= 0);
        slow.close(fd);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!slow.wait_idle(100));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(400));
        CHECK(slow.wait_idle(3000) && slow.take_error().empty());
    }

    // R10. Who the home is used as (plan_storage, pure): the home's owner,
    //      not $SUDO_UID; a mismatching or odd $SUDO_UID fails closed.
    {
        uint32_t id = 0;
        CHECK(wd::parse_id("1000", id) && id == 1000);
        CHECK(wd::parse_id("0", id) && id == 0);
        for (const char* bad : {"", "-1", "+1000", " 1000", "1000 ", "10a", "4294967295", "4294968296", "99999999999"})
            CHECK(!wd::parse_id(bad, id));
        auto lookup = [](uid_t uid, gid_t& gid, std::vector<gid_t>& groups) {
            if (uid != 1000) return false;
            gid = 1000;
            groups = {1000, 20, 27};
            return true;
        };
        auto p = wd::plan_storage(false, "1000", 4242, 4242, lookup); // not root: own credentials
        CHECK(p.ok && !p.owner.switch_creds);
        p = wd::plan_storage(true, "1000", 1000, 1000, lookup);
        CHECK(p.ok && p.owner.switch_creds && p.owner.uid == 1000 && p.owner.gid == 1000 &&
              p.owner.groups == (std::vector<gid_t>{1000, 20, 27}));
        CHECK(!wd::plan_storage(true, "1000", 1001, 1001, lookup).ok); // home of someone else
        CHECK(!wd::plan_storage(true, "1000", 0, 0, lookup).ok);       // sudo user, root's home
        for (const char* bad : {"", "-1", "+1000", " 1000", "4294967295", "4294968296"})
            CHECK(!wd::plan_storage(true, bad, 1000, 1000, lookup).ok);
        p = wd::plan_storage(true, nullptr, 0, 0, lookup); // the app itself runs as root
        CHECK(p.ok && !p.owner.switch_creds);
        p = wd::plan_storage(true, "0", 0, 0, lookup); // SUDO_UID=0 on root's home
        CHECK(p.ok && !p.owner.switch_creds);
        p = wd::plan_storage(true, "1000", 1000, 5000, lookup); // the passwd gid wins over the home's group
        CHECK(p.ok && p.owner.uid == 1000 && p.owner.gid == 1000);
        p = wd::plan_storage(true, nullptr, 1000, 1000, lookup); // manual root run on a user's home
        CHECK(p.ok && p.owner.switch_creds && p.owner.uid == 1000);
        p = wd::plan_storage(true, "2000", 2000, 3000, lookup); // no passwd entry
        CHECK(p.ok && p.owner.uid == 2000 && p.owner.gid == 3000 && p.owner.groups == (std::vector<gid_t>{3000}));
        // Storage itself: a missing home or a file as home is "Storage setup error".
        CHECK(!wd::Storage("/nonexistent/home").setup());
        const std::string f = tmpdir("homefile") + "/f";
        { std::ofstream o(f); o << "x"; }
        CHECK(!wd::Storage(f).setup());
        CHECK(!wd::Storage("").setup());
    }

    // R11. The home replaced after setup(): the helper notices (EXDEV) and
    //      nothing is written into the replacement.
    {
        const std::string base = tmpdir("swap");
        const std::string home = base + "/h";
        CHECK(::mkdir(home.c_str(), 0755) == 0);
        wd::StorageOptions me;
        me.test_switch_to_self = true;
        wd::Storage st(home, me);
        CHECK(st.setup() && st.owner().switch_creds);
        CHECK(::rename(home.c_str(), (base + "/old").c_str()) == 0 && ::mkdir(home.c_str(), 0755) == 0);
        bool is_new = false;
        CHECK(st.open("a.csv", is_new) < 0 && st.last_errno() == EXDEV);
        struct stat sb{};
        CHECK(::stat((home + "/BruceWardriving").c_str(), &sb) != 0);
    }

    // R12. A file the opener has just created but then rejects (here: the
    //      owner check, as on a filesystem that maps owners) is removed again.
    for (int pass = 0; pass < 2; ++pass) {
        const std::string home = tmpdir(pass ? "reject1" : "reject0");
        wd::StorageOptions o;
        o.test_switch_to_self = pass == 1;
        o.test_expect_other_owner = true;
        wd::Storage st(home, o);
        bool is_new = false;
        CHECK(st.open("z.csv", is_new) < 0 && st.last_errno() == EPERM);
        struct stat sb{};
        CHECK(::stat((home + "/BruceWardriving/z.csv").c_str(), &sb) != 0);
        // ... but a file that already existed and is rejected stays as it was.
        { std::ofstream f(home + "/BruceWardriving/old.csv"); f << "KEEP\n"; }
        CHECK(st.open("old.csv", is_new) < 0 && st.last_errno() == EPERM);
        CHECK(slurp(home + "/BruceWardriving/old.csv") == "KEEP\n");
    }

    // R13. alert.txt is read up to 1 MiB (the user's file, read by root).
    {
        const std::string home = tmpdir("bigalert");
        CHECK(::mkdir((home + "/BruceWardriving").c_str(), 0755) == 0);
        {
            std::ofstream a(home + "/BruceWardriving/alert.txt");
            const std::string line(99, '#');
            for (size_t n = 0; n < wd::Storage::kMaxRead / 100 + 10; ++n) a << line << "\n";
            a << "02:00:5e:20:00:01\n";
        }
        wd::Storage st(home);
        std::string text;
        int err = -1;
        CHECK(st.read("alert.txt", text, &err) && err == 0);
        CHECK(text.size() <= wd::Storage::kMaxRead && !text.empty() && text.back() == '\n');
        CHECK(wd::parse_alert_macs(text).empty()); // the MAC beyond 1 MiB is not read
        // a MAC within the first 1 MiB of a larger file is still loaded
        {
            std::ofstream a(home + "/BruceWardriving/alert2.txt");
            a << "02:00:5E:20:00:02\n";
            const std::string line(99, '#');
            for (size_t n = 0; n < wd::Storage::kMaxRead / 100 + 10; ++n) a << line << "\n";
        }
        text.clear();
        CHECK(st.read("alert2.txt", text, &err) && err == 0);
        const auto macs = wd::parse_alert_macs(text);
        CHECK(macs.size() == 1 && macs.count("02:00:5e:20:00:02"));
    }

    // R14. Root working in a root-owned home (in-process): the home and the
    //      folder must be the expected owner's and not group/other writable
    //      (exercised as ourselves with test_strict_dirs).
    {
        wd::StorageOptions o;
        o.test_strict_dirs = true;
        const std::string home = tmpdir("strict");
        CHECK(::chmod(home.c_str(), 0775) == 0);
        CHECK(!wd::Storage(home, o).setup()); // a group-writable home
        CHECK(::chmod(home.c_str(), 0755) == 0);
        {
            wd::Storage st(home, o);
            bool is_new = false;
            const int fd = st.open("ok.csv", is_new);
            CHECK(fd >= 0 && is_new);
            ::close(fd);
        }
        CHECK(::chmod((home + "/BruceWardriving").c_str(), 0777) == 0); // prepared by "someone else"
        {
            wd::Storage st(home, o);
            bool is_new = false;
            CHECK(st.open("x.csv", is_new) < 0 && st.last_errno() == EPERM);
            struct stat sb{};
            CHECK(::stat((home + "/BruceWardriving/x.csv").c_str(), &sb) != 0);
        }
        // the ordinary (non-root) path does not apply these checks
        wd::Storage plain(home);
        bool is_new = false;
        const int fd = plain.open("y.csv", is_new);
        CHECK(fd >= 0);
        ::close(fd);
    }

    // R2/R3. Scan tools: closing the output does not disable Back / the
    //        timeout; stopping kills descendants that ignore SIGTERM; a tool
    //        that exits leaving a child behind does not hang the capture.
    {
        const std::string dir = tmpdir("cap");
        auto alive = [](const std::string& pidfile) {
            const int pid = std::atoi(slurp(pidfile).c_str());
            if (pid <= 0) return false;
            for (int i = 0; i < 100; ++i) { // killed processes are reaped by init
                if (::kill(pid, 0) != 0) return false;
                ::usleep(10000);
            }
            return true;
        };
        // closes stdout/stderr and keeps running: the timeout still applies
        auto t0 = std::chrono::steady_clock::now();
        auto c = wd::run_capture({"sh", "-c", "echo $$ > " + dir + "/p1; exec >&- 2>&-; exec sleep 30"}, 300, nullptr);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(c.aborted && ms < 800 && !alive(dir + "/p1"));
        // ... and so does stop (Back), set from another thread
        std::atomic<bool> stop{false};
        std::thread stopper([&] { ::usleep(200000); stop = true; });
        t0 = std::chrono::steady_clock::now();
        c = wd::run_capture({"sh", "-c", "echo $$ > " + dir + "/p2; exec >&- 2>&-; exec sleep 30"}, 20000, &stop);
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        stopper.join();
        CHECK(c.aborted && ms < 700 && !alive(dir + "/p2"));
        // a descendant that ignores SIGTERM goes too
        stop = false;
        std::thread stopper2([&] { ::usleep(300000); stop = true; });
        c = wd::run_capture({"sh", "-c", "(trap '' TERM; exec sleep 30) & echo $! > " + dir + "/p3; exec sleep 30"},
                            20000, &stop);
        stopper2.join();
        CHECK(c.aborted && !alive(dir + "/p3"));
        // the tool exits, a child keeps the pipe: done at once, child gone
        t0 = std::chrono::steady_clock::now();
        c = wd::run_capture({"sh", "-c", "sleep 30 & echo $! > " + dir + "/p4; echo done"}, 20000, nullptr);
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!c.aborted && c.status == 0 && c.out == "done\n" && ms < 1000 && !alive(dir + "/p4"));
        // a normal run
        c = wd::run_capture({"sh", "-c", "echo out; echo err >&2; exit 3"}, 5000, nullptr);
        CHECK(!c.aborted && c.status == 3 && c.out == "out\nerr\n");
        c = wd::run_capture({"/nonexistent/tool"}, 5000, nullptr);
        CHECK(c.status == 127 && c.out.find("/nonexistent/tool") != std::string::npos);
        // stdin input, and the probe: called while the tool still runs (with
        // its output so far), then the tool is ended - not "aborted".
        c = wd::run_capture({"sh", "-c", "read a; read b; echo got:$a:$b"}, 5000, nullptr, 100, "one\ntwo\n");
        CHECK(!c.aborted && c.status == 0 && c.out == "got:one:two\n");
        {
            bool running_at_probe = false;
            std::string seen;
            wd::CaptureProbe probe;
            probe.at_ms = 300;
            probe.fn = [&](const std::string& so_far) {
                seen = so_far;
                running_at_probe = ::kill(std::atoi(slurp(dir + "/p5").c_str()), 0) == 0;
            };
            const auto t = std::chrono::steady_clock::now();
            c = wd::run_capture({"sh", "-c", "echo $$ > " + dir + "/p5; echo early; exec sleep 30"}, 20000, nullptr, 100,
                                {}, &probe);
            CHECK(c.ended_early && !c.aborted && c.status == 0 && seen == "early\n" && running_at_probe);
            CHECK(std::chrono::steady_clock::now() - t < std::chrono::seconds(2));
            CHECK(::kill(std::atoi(slurp(dir + "/p5").c_str()), 0) != 0); // ended and reaped
            // A tool that exits first gets no probe call.
            int calls = 0;
            probe.fn = [&](const std::string&) { ++calls; };
            c = wd::run_capture({"sh", "-c", "echo quick"}, 5000, nullptr, 100, {}, &probe);
            CHECK(calls == 0 && !c.ended_early && c.status == 0 && c.out == "quick\n");
            // Back during the probe: aborted.
            std::atomic<bool> halt{false};
            probe.at_ms = 100;
            probe.fn = [&](const std::string&) { halt = true; };
            c = wd::run_capture({"sleep", "30"}, 20000, &halt, 100, {}, &probe);
            CHECK(c.aborted && !c.ended_early && c.status == -1);
        }
        // R9 (round 6). Output that keeps coming cannot hold up the probe's
        // snapshot: it reads at most max_snapshot bytes, and stops at the
        // deadline / Back even with no size bound.
        {
            size_t snap = 0;
            wd::CaptureProbe big;
            big.at_ms = 200; // `yes` keeps the pipe readable all the time
            big.max_snapshot = 8192;
            big.fn = [&](const std::string& so_far) { snap = so_far.size(); };
            auto t = std::chrono::steady_clock::now();
            c = wd::run_capture({"yes", "0123456789abcdef0123456789abcdef"}, 10000, nullptr, 100, {}, &big);
            CHECK(c.ended_early && snap > 0 && c.status == 0);
            CHECK(std::chrono::steady_clock::now() - t < std::chrono::seconds(2));
            int calls = 0;
            wd::CaptureProbe unbounded;
            unbounded.at_ms = 250;
            unbounded.max_snapshot = SIZE_MAX;
            unbounded.fn = [&](const std::string&) { ++calls; };
            t = std::chrono::steady_clock::now();
            c = wd::run_capture({"yes", "0123456789abcdef0123456789abcdef"}, 300, nullptr, 100, {}, &unbounded);
            CHECK(std::chrono::steady_clock::now() - t < std::chrono::seconds(1));
            CHECK(c.aborted ? calls == 0 : (c.ended_early && calls == 1));
            std::atomic<bool> back{false};
            std::thread presser([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(270));
                back = true;
            });
            calls = 0;
            t = std::chrono::steady_clock::now();
            c = wd::run_capture({"yes", "0123456789abcdef0123456789abcdef"}, 20000, &back, 100, {}, &unbounded);
            presser.join();
            CHECK(std::chrono::steady_clock::now() - t < std::chrono::seconds(1));
            CHECK(c.aborted ? calls == 0 : (c.ended_early && calls == 1));
            // After the tool exited, a descendant that keeps writing is read
            // only up to a bound, not until it stops.
            t = std::chrono::steady_clock::now();
            c = wd::run_capture({"sh", "-c", "yes 0123456789abcdef & exit 0"}, 20000, nullptr);
            CHECK(std::chrono::steady_clock::now() - t < std::chrono::seconds(2) && c.status == 0);
            CHECK(c.out.size() <= 256 * 1024 + 4096 * 64);
        }
        // R10 (round 6). stdin far larger than the pipe (64 KiB here), read
        // slowly: it needs many partial writes and EAGAIN waits, and all of
        // it arrives (no truncation); a reader that stops early is
        // reported, without SIGPIPE; Back works while stdin is still being
        // written; a SIGPIPE pending before the call stays pending.
        {
            std::string in;
            for (int i = 0; in.size() < 300 * 1024; ++i) {
                char l[32];
                std::snprintf(l, sizeof(l), "info 02:00:5E:%02X:%02X:%02X\n", (i >> 16) & 255, (i >> 8) & 255, i & 255);
                in += l;
            }
            c = wd::run_capture({"sh", "-c", "sleep 0.2; wc -c"}, 10000, nullptr, 100, in);
            CHECK(!c.aborted && c.status == 0 && !c.input_incomplete &&
                  std::atol(c.out.c_str()) == static_cast<long>(in.size()));
            c = wd::run_capture({"sh", "-c", "head -c 10 >/dev/null; echo done"}, 10000, nullptr, 100, in);
            CHECK(!c.aborted && c.status == 0 && c.input_incomplete && c.out == "done\n");
            std::atomic<bool> back{false};
            std::thread presser([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                back = true;
            });
            const auto t = std::chrono::steady_clock::now();
            c = wd::run_capture({"sleep", "30"}, 20000, &back, 100, in); // never reads stdin
            presser.join();
            CHECK(c.aborted && std::chrono::steady_clock::now() - t < std::chrono::seconds(1));
            // R11 (round 7). A tool that keeps taking input (every write
            // succeeds, never EAGAIN): Back and the deadline still end the
            // capture on time. Writes are made slow and partial (4 KiB, 1 ms)
            // so 3 MiB would take ~0.8 s to send.
            {
                wd::testing::capture_write = [](int fd, const void* buf, size_t n) -> ssize_t {
                    const ssize_t r = ::write(fd, buf, std::min<size_t>(n, 4096));
                    ::usleep(1000);
                    return r;
                };
                const std::string flood(3 * 1024 * 1024, 'x');
                auto t1 = std::chrono::steady_clock::now();
                c = wd::run_capture({"wc", "-c"}, 300, nullptr, 100, flood);
                const auto el1 = std::chrono::steady_clock::now() - t1;
                CHECK(c.aborted && c.input_incomplete);
                CHECK(el1 < std::chrono::milliseconds(600));
                std::atomic<bool> back2{false};
                std::thread p2([&] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    back2 = true;
                });
                t1 = std::chrono::steady_clock::now();
                c = wd::run_capture({"wc", "-c"}, 20000, &back2, 100, flood);
                const auto el2 = std::chrono::steady_clock::now() - t1;
                p2.join();
                CHECK(c.aborted && c.input_incomplete);
                CHECK(el2 < std::chrono::milliseconds(400));
                // Not cut short, the same slow writes still deliver everything.
                c = wd::run_capture({"wc", "-c"}, 20000, nullptr, 100, std::string(256 * 1024, 'y'));
                CHECK(!c.aborted && !c.input_incomplete && std::atol(c.out.c_str()) == 256 * 1024);
                wd::testing::capture_write = ::write;
            }
            sigset_t ps, old, pending;
            sigemptyset(&ps);
            sigaddset(&ps, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &ps, &old);
            pthread_kill(pthread_self(), SIGPIPE);
            c = wd::run_capture({"sh", "-c", "exec 0<&-; sleep 0.1"}, 10000, nullptr, 100, in);
            CHECK(c.input_incomplete && c.status == 0);
            sigemptyset(&pending);
            sigpending(&pending);
            CHECK(sigismember(&pending, SIGPIPE) == 1); // the caller's SIGPIPE is not consumed
            const timespec zero{};
            while (sigtimedwait(&ps, nullptr, &zero) == SIGPIPE) {
            }
            pthread_sigmask(SIG_SETMASK, &old, nullptr);
        }
    }

    // R4. A maximum-length BLE name of quotes: the row is complete and the
    //     next row still parses (Bruce's char[512] would cut it).
    {
        auto parse_csv = [](const std::string& text) {
            std::vector<std::vector<std::string>> recs(1, std::vector<std::string>(1));
            bool q = false;
            for (size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (q) {
                    if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') { recs.back().back() += '"'; ++i; }
                    else if (c == '"') q = false;
                    else recs.back().back() += c;
                } else if (c == '"') q = true;
                else if (c == ',') recs.back().emplace_back();
                else if (c == '\n') recs.emplace_back(1);
                else recs.back().back() += c;
            }
            recs.pop_back(); // after the last "\n"
            return recs;
        };
        ble::Device big;
        big.mac = "02:00:5E:10:00:20";
        big.name = std::string(248, '"');
        big.rssi = -50;
        big.mfgr_id = 0x004c;
        ble::Device small = bd;
        small.name = "x";
        const std::string rows = wd::ble_row(big, fix) + wd::ble_row(small, fix) + wd::wifi_row(aps[1], fix);
        const auto recs = parse_csv(rows);
        CHECK(recs.size() == 3);
        for (const auto& r : recs) CHECK(r.size() == 14);
        CHECK(recs[0][1] == big.name && recs[0][12] == "004C" && recs[0][13] == "BLE");
        CHECK(recs[1][1] == "x" && recs[2][1] == "Cafe, \"Free\"" && recs[2][13] == "WIFI");
        CHECK(wd::ble_row(big, fix).size() > 512);
    }

    // R5. SSIDs that are not valid UTF-8 stay printable escapes.
    {
        CHECK(decode_iw_ssid("bad\\xff") == "bad\\xff");
        CHECK(decode_iw_ssid("trunc\\xe3\\x83") == "trunc\\xe3\\x83");                    // cut 3-byte sequence
        CHECK(decode_iw_ssid("\\xe3\\x83\\x86\\xff\\xe3\\x83\\x88") == "\xe3\x83\x86\\xff\xe3\x83\x88"); // mixed
        CHECK(decode_iw_ssid("\\xc0\\xaf") == "\\xc0\\xaf");                                // overlong '/'
        CHECK(decode_iw_ssid("\\xed\\xa0\\x80") == "\\xed\\xa0\\x80");                      // surrogate
        CHECK(decode_iw_ssid("\\xf4\\x90\\x80\\x80") == "\\xf4\\x90\\x80\\x80");            // > U+10FFFF
        CHECK(decode_iw_ssid("\\xf0\\x9f\\x93\\xa1") == "\xf0\x9f\x93\xa1");                // valid 4-byte
        CHECK(decode_iw_ssid("\\xe3\\x83") == "\\xe3\\x83");
        const std::string raw = "BSS 02:00:5e:20:00:30(on wlan0)\n\tfreq: 2412\n\tSSID: a\\xffb\\xe3\\x83\\x86\n";
        const auto r = parse_iw_scan(raw);
        CHECK(r.size() == 1 && r[0].ssid == "a\\xffb\xe3\x83\x86");
        CHECK(wd::wifi_row(r[0], fix).find(",\"a\\xffb\xe3\x83\x86\",") != std::string::npos);
    }

    // R6. A BLE scan whose discovery failed logs nothing, even when
    //     bluetoothctl listed cached devices first.
    {
        const std::string log = "[NEW] Device 02:00:5E:30:00:01 Cached\n"
                                "Failed to start discovery: org.bluez.Error.NotReady\n";
        ble::ParseOptions po;
        po.only_seen_in_scan = true;
        const auto r = ble::parse_scan(log, po);
        CHECK(r.devices.empty() && !r.discovery_started && !r.error.empty());
        const auto screen = ble::parse_scan(log); // the BLE Scan screen still lists it
        CHECK(screen.devices.size() == 1 && !screen.discovery_started);
        int rssi = 0;
        CHECK(ble::parse_info_rssi("Device 02:00:5E:30:00:01 (random)\n\tName: x\n\tRSSI: 0xffffffb4 (-76)\n", rssi) &&
              rssi == -76);
        CHECK(ble::parse_info_rssi("\x1b[0;94m\tRSSI: -61\x1b[0m\n", rssi) && rssi == -61);
        CHECK(!ble::parse_info_rssi("Device 02:00:5E:30:00:01 not available\n", rssi));
        // Batched `info` (bluetoothctl reading stdin), in BlueZ 5.82's shapes:
        // prompts, a "[CHG] ... RSSI" event glued to a prompt, a device that
        // is gone, one without an RSSI and BlueZ's 127 "unknown".
        const std::string batch =
            "Waiting to connect to bluetoothd...\x1b[0;94m[bluetoothctl]\x1b[0m> info 02:00:5E:40:00:01\n"
            "Device 02:00:5E:40:00:01 (random)\n\tAlias: 02-00-5E-40-00-01\n\tPaired: no\n"
            "\tRSSI: 0xffffffbe (-66)\n\tTxPower: 0x0008 (8)\n\tManufacturerData.Key: 0x0006 (6)\n"
            "[bluetoothctl]> info 02:00:5E:40:00:02\n"
            "Device 02:00:5E:40:00:02 not available\n"
            "[bluetoothctl]>                       [CHG] Device 02:00:5E:40:00:03 RSSI: 0xffffffa6 (-90)\n"
            "[bluetoothctl]> info 02:00:5E:40:00:04\n"
            "Device 02:00:5E:40:00:04 (public)\n\tName: Name-04\n\tPaired: no\n"
            "[bluetoothctl]> info 02:00:5E:40:00:05\n"
            "Device 02:00:5E:40:00:05 (random)\n\tRSSI: 127\n"
            "[bluetoothctl]> info 02:00:5E:40:00:06\n"
            "Device 02:00:5E:40:00:06 (public)\n\tRSSI: -58\n"
            "[bluetoothctl]> quit\n";
        const auto got = ble::parse_info_batch(batch);
        CHECK(got.size() == 3);
        CHECK(got.count("02:00:5E:40:00:01") && got.at("02:00:5E:40:00:01") == -66);
        CHECK(got.count("02:00:5E:40:00:03") && got.at("02:00:5E:40:00:03") == -90);
        CHECK(got.count("02:00:5E:40:00:06") && got.at("02:00:5E:40:00:06") == -58);
        CHECK(!got.count("02:00:5E:40:00:02") && !got.count("02:00:5E:40:00:04") && !got.count("02:00:5E:40:00:05"));
        // The scan's own [CHG] RSSI: BlueZ's 127 / 0 "unknown" keep the last
        // usable value (none here), so they are never logged as an RSSI.
        const auto sentinel = ble::parse_scan("Discovery started\n[NEW] Device 02:00:5E:40:00:07 n\n"
                                              "[CHG] Device 02:00:5E:40:00:07 RSSI: 0x0000007f (127)\n"
                                              "[NEW] Device 02:00:5E:40:00:08 m\n"
                                              "[CHG] Device 02:00:5E:40:00:08 RSSI: 0xffffffb0 (-80)\n"
                                              "[CHG] Device 02:00:5E:40:00:08 RSSI: 0x0000007f (127)\n",
                                              {100, true});
        CHECK(sentinel.devices.size() == 2 && sentinel.devices[0].rssi == 0 && sentinel.devices[1].rssi == -80);
        // An RSSI line after "not available" is not given to the previous block.
        CHECK(ble::parse_info_batch("Device 02:00:5E:40:00:01 (random)\n\tName: a\n"
                                    "[bluetoothctl]> info 02:00:5E:40:00:02\n"
                                    "Device 02:00:5E:40:00:02 not available\n\tRSSI: -50\n")
                  .empty());
        // The screen parser's bound applies while parsing (insertion time).
        std::string many;
        for (int i = 0; i < 300; ++i) {
            char l[64];
            std::snprintf(l, sizeof(l), "[NEW] Device 02:00:5E:31:%02X:%02X n\n", i / 256, i % 256);
            many += l;
        }
        const auto capped = ble::parse_scan(many, {100, false});
        CHECK(capped.devices.size() == 100 && capped.truncated);
    }

    // R7. A write error ends the pass at once and the run with "Failed to
    //     write file" (the firmware ignores it). In a child with a small file
    //     size limit and the DEFAULT SIGXFSZ disposition: the write path itself
    //     must turn the signal into EFBIG. Both mode: after the failure no
    //     BLE scan and no RSSI query runs, and no row is counted unstored.
    auto in_child = [](const std::function<bool()>& body) {
        const pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            ::signal(SIGXFSZ, SIG_DFL);
            _exit(body() ? 0 : 1);
        }
        int st = 0;
        ::waitpid(pid, &st, 0);
        return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    };
    {
        const std::string home = tmpdir("efbig");
        CHECK(in_child([&] {
            rlimit rl{wd::wigle_header("9.9.9").size() + 150, wd::wigle_header("9.9.9").size() + 150}; // header + 1 row
            ::setrlimit(RLIMIT_FSIZE, &rl);
            wd::Storage st(home);
            Sim s;
            s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
            s.wifi = {{aps[0], aps[1], aps[2], aps[3]}};
            s.ble_result = ble::parse_scan(slurp("tests/fixtures/bluetoothctl_scan_le_5.85.txt"), {100, true});
            auto o = wifi_opts();
            o.scan_ble = true;
            wd::Wardriver w(o, s.hooks(), st);
            w.run();
            return w.end_reason() == wd::Wardriver::EndReason::StorageError && w.popup_waits() &&
                   w.frame().popup_text == "Failed to write file" && line_seen(s, "Failed to write file") &&
                   !line_seen(s, "Scan done. WiFi: 4 BLE: 15") && s.ble_calls == 0 && s.rssi_queries.empty() &&
                   w.wifi_count() == 1 && w.alert_count() == 0;
        }) == 0);
    }
    {
        // ESC during the failing pass: the storage reason is kept.
        const std::string home = tmpdir("efbig2");
        CHECK(in_child([&] {
            rlimit rl{0, 0}; // even the header fails
            ::setrlimit(RLIMIT_FSIZE, &rl);
            wd::Storage st(home);
            Sim s;
            s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
            s.stop_at = 500; // Back arrives with the failing pass
            s.wifi = {{aps[0]}};
            wd::Wardriver w(wifi_opts(), s.hooks(), st);
            w.run();
            return w.end_reason() == wd::Wardriver::EndReason::StorageError && s.wifi_calls == 0;
        }) == 0);
    }

    // R8. A failed fdatasync() of the CSV written by a completed scan pass
    //     ends the run while the GPS then only sends no-fix data (no new
    //     scan), at the next tick of the inter-pass wait. alert.txt exists
    //     and the fault applies to the CSV only, so no startup write can be
    //     what fails; the virtual clock waits for the storage worker.
    {
        const std::string home = tmpdir("syncerr");
        CHECK(::mkdir((home + "/BruceWardriving").c_str(), 0755) == 0);
        { std::ofstream a(home + "/BruceWardriving/alert.txt"); a << "# none\n"; }
        wd::StorageOptions so;
        so.sync_fault_errno = EIO;
        wd::Storage st(home, so);
        Sim s;
        s.storage = &st;
        s.feed.push_back({0, fix_at("152002", "0000.74070", "00004.07340")});
        for (int t = 1; t < 120; ++t) {
            char hh[16];
            std::snprintf(hh, sizeof(hh), "15%02d%02d", 20 + t / 60, t % 60);
            s.feed.push_back({static_cast<uint64_t>(t) * 1000, nofix_at(hh)});
        }
        s.wifi = {{aps[0]}};
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        // the intended stimulus: one completed scan pass wrote the CSV
        CHECK(s.wifi_calls == 1 && line_seen(s, "Scan done. WiFi: 1"));
        CHECK(w.filename() == "261122_152002_wardriving.csv");
        CHECK(slurp(home + "/BruceWardriving/261122_152002_wardriving.csv").find(",WIFI\n") != std::string::npos);
        CHECK(w.end_reason() == wd::Wardriver::EndReason::StorageError);
        CHECK(w.popup_waits() && w.frame().popup_text == "Failed to write file");
        // scan ends at 3620 ms (500 + 3000 + 120): caught at the next 50 ms
        // tick, long before the 5 s wait ends and a no-fix pass would come
        CHECK(s.now <= 3620 + 50 + 200 + 1);
        bool logged = false;
        for (const auto& l : s.serial) logged = logged || l.find("sync: Input/output error") != std::string::npos;
        CHECK(logged);
    }

    // R9. Shutdown found a sync/close error after the run ended (Back, or
    //     GPS not Found!): reported over that reason, waiting for OK.
    {
        Sim s;
        s.open_ok = false;
        s.open_not_found = true;
        wd::Storage st(tmpdir("late"));
        wd::Wardriver w(wifi_opts(), s.hooks(), st);
        w.run();
        CHECK(w.end_reason() == wd::Wardriver::EndReason::NotFound);
        w.report_storage_error("close: Input/output error");
        CHECK(w.end_reason() == wd::Wardriver::EndReason::StorageError && w.popup_waits());
        CHECK(s.frames.back().popup_text == "Failed to write file" && !w.gps_connected());
        CHECK(std::string(wd::end_reason_name(w.end_reason())) == "storage");
    }

    std::printf("wardrive self-check OK (%d checks)\n", g_checks);
    return 0;
}
