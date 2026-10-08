/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Wi-Fi wardriving: scan APs with `iw`, tag each with a gpsd fix, and append
 * rows in WiGLE WiFi CSV format (WigleWifi-1.4) for later upload to wigle.net -
 * the same output the ESP32 Bruce firmware produces. Parsing/formatting is pure
 * and unit-tested (tests/wardrive_selfcheck.cpp); the subcommand ties it to
 * `iw`/`gpspipe` at runtime.
 */

#pragma once

#include <string>
#include <vector>

namespace platform {

struct ScanAp {
    std::string bssid;
    std::string ssid;
    std::string auth;   // WiGLE AuthMode token, e.g. "[WPA2-PSK-CCMP][ESS]"
    int channel = 0;
    int signal = 0;     // dBm
};

// 802.11 frequency (MHz) -> channel number (2.4GHz and 5GHz). 0 if unknown.
int freq_to_channel(int mhz);

// WiGLE AuthMode token from the security flags seen in an `iw scan` BSS block.
std::string wigle_auth(bool rsn, bool wpa, bool privacy);

// Parse `iw dev <if> scan` output into a list of APs.
std::vector<ScanAp> parse_iw_scan(const std::string& text);

// Parse a gpsd JSON TPV line; returns true and fills lat/lon/alt when it carries
// a 2D+ fix.
bool parse_gps_tpv(const std::string& json, double& lat, double& lon, double& alt);

// WiGLE CSV pre-header + column header (two lines, no trailing newline).
std::string wigle_header();

// One WiGLE CSV data row (no trailing newline). `when` is "YYYY-MM-DD HH:MM:SS".
std::string wigle_row(const ScanAp& ap, double lat, double lon, double alt, const std::string& when);

// Entry point for `--wardrive [iface] [outfile]`. Loops scan+fix+append until
// SIGTERM; prints progress. Returns 0.
int wardrive_main(int argc, char** argv);

} // namespace platform
