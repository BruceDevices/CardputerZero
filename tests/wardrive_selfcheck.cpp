/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the wardriving parsers / WiGLE CSV formatter. Not
 * part of the CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/platform tests/wardrive_selfcheck.cpp \
 *       src/platform/wardrive.cpp -o /tmp/wardrive_check && /tmp/wardrive_check
 */

#include "wardrive.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

int main() {
    using namespace platform;

    // --- freq -> channel ---
    assert(freq_to_channel(2412) == 1);
    assert(freq_to_channel(2437) == 6);
    assert(freq_to_channel(2472) == 13);
    assert(freq_to_channel(2484) == 14);
    assert(freq_to_channel(5180) == 36);
    assert(freq_to_channel(5745) == 149);

    // --- auth token ---
    assert(wigle_auth(true, false, true) == "[WPA2-PSK-CCMP][ESS]");
    assert(wigle_auth(false, true, true) == "[WPA-PSK-TKIP][ESS]");
    assert(wigle_auth(false, false, true) == "[WEP][ESS]");
    assert(wigle_auth(false, false, false) == "[ESS]"); // open

    // --- iw scan parsing ---
    const std::string scan =
        "BSS aa:bb:cc:dd:ee:ff(on wlan0)\n"
        "\tfreq: 2437\n"
        "\tsignal: -45.00 dBm\n"
        "\tSSID: HomeNet\n"
        "\tRSN:\t * Version: 1\n"
        "\tcapability: ESS Privacy ShortSlotTime (0x0411)\n"
        "BSS 11:22:33:44:55:66(on wlan0)\n"
        "\tfreq: 5180\n"
        "\tsignal: -70.00 dBm\n"
        "\tSSID: \n"                       // hidden SSID
        "\tcapability: ESS (0x0001)\n";
    auto aps = parse_iw_scan(scan);
    assert(aps.size() == 2);
    assert(aps[0].bssid == "aa:bb:cc:dd:ee:ff");
    assert(aps[0].channel == 6);
    assert(aps[0].signal == -45);
    assert(aps[0].ssid == "HomeNet");
    assert(aps[0].auth == "[WPA2-PSK-CCMP][ESS]");
    assert(aps[1].bssid == "11:22:33:44:55:66");
    assert(aps[1].channel == 36);
    assert(aps[1].ssid.empty());
    assert(aps[1].auth == "[ESS]"); // open, no Privacy

    // --- SSID with a comma must not break the CSV row ---
    {
        const std::string s = "BSS de:ad:be:ef:00:01(on wlan0)\n\tfreq: 2412\n\tSSID: a,b\n";
        auto a = parse_iw_scan(s);
        assert(a.size() == 1 && a[0].ssid.find(',') == std::string::npos);
    }

    // --- gpsd TPV parsing ---
    double lat = 0, lon = 0, alt = 0;
    assert(parse_gps_tpv("{\"class\":\"TPV\",\"lat\":37.123456,\"lon\":-122.654321,\"alt\":12.3}",
                         lat, lon, alt));
    assert(lat > 37.12 && lat < 37.13 && lon < -122.65 && alt > 12.0);
    assert(!parse_gps_tpv("{\"class\":\"SKY\"}", lat, lon, alt));
    assert(!parse_gps_tpv("{\"class\":\"TPV\",\"mode\":1}", lat, lon, alt)); // no lat/lon

    // --- WiGLE header + row shape ---
    assert(wigle_header().find("WigleWifi-1.4") == 0);
    assert(wigle_header().find("MAC,SSID,AuthMode") != std::string::npos);
    const std::string row = wigle_row(aps[0], 37.1, -122.6, 10.0, "2026-10-08 12:00:00");
    // 11 fields -> 10 commas, ends in WIFI.
    assert(std::count(row.begin(), row.end(), ',') == 10);
    assert(row.rfind(",WIFI") == row.size() - 5);
    assert(row.rfind("aa:bb:cc:dd:ee:ff,HomeNet,", 0) == 0);

    std::puts("wardrive self-check OK");
    return 0;
}
