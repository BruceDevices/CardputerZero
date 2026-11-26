/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the wardriving parsers / WiGLE CSV formatter. Not
 * part of the CMake build. Run manually:
 *
 *   g++ -std=c++17 -pthread -Isrc/platform tests/wardrive_selfcheck.cpp \
 *       src/platform/wardrive.cpp src/platform/gps_session.cpp src/platform/gps_parser.cpp \
 *       src/platform/gps_port.cpp src/platform/uart_speed.cpp \
 *       -o /tmp/wardrive_check && /tmp/wardrive_check
 */

#include "wardrive.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
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

    // --- WiGLE header + row shape ---
    assert(wigle_header().find("WigleWifi-1.4") == 0);
    assert(wigle_header().find("MAC,SSID,AuthMode") != std::string::npos);
    const std::string row = wigle_row(aps[0], 37.1, -122.6, 10.0, "2026-10-08 12:00:00");
    // 11 fields -> 10 commas, ends in WIFI.
    assert(std::count(row.begin(), row.end(), ',') == 10);
    assert(row.rfind(",WIFI") == row.size() - 5);
    assert(row.rfind("aa:bb:cc:dd:ee:ff,HomeNet,", 0) == 0);

#if defined(__linux__)
    // --wardrive runs in a child with stdout captured: (rc, output).
    auto run = [](std::vector<std::string> args) {
        const std::string out = "/tmp/wardrive_selfcheck_out_" + std::to_string(::getpid()) + ".txt";
        const pid_t pid = ::fork();
        assert(pid >= 0);
        if (pid == 0) {
            FILE* f = std::freopen(out.c_str(), "w", stdout);
            (void)f;
            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(a.data());
            _exit(wardrive_main(static_cast<int>(argv.size()), argv.data()));
        }
        int st = 0;
        ::waitpid(pid, &st, 0);
        std::string text;
        if (FILE* f = std::fopen(out.c_str(), "r")) {
            char buf[512];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        std::remove(out.c_str());
        return std::make_pair(WIFEXITED(st) ? WEXITSTATUS(st) : -1, text);
    };
    const std::string csv = "/tmp/wardrive_selfcheck_" + std::to_string(::getpid()) + ".csv";
    struct stat st {};

    // --- no gpsd any more: a missing GPS node fails fast, no CSV left behind ---
    {
        std::remove(csv.c_str());
        auto r = run({"--wifi", "--port", "/nonexistent/gps0", "--baud", "115200", "wlan0", csv});
        assert(r.first == 1);
        assert(r.second.find("GPS not Found!") != std::string::npos);
        assert(r.second.find("gpspipe") == std::string::npos && r.second.find("gpsd") == std::string::npos);
        assert(stat(csv.c_str(), &st) != 0);
    }
    // --- BLE pass not ported yet; bad options ---
    {
        auto r = run({"--ble", "--port", "/nonexistent/gps0", "wlan0", csv});
        assert(r.first == 1 && r.second.find("BLE wardriving is not available yet") != std::string::npos);
        assert(run({"--bogus"}).first == 2);
        assert(run({"--baud", "0"}).first == 2);
    }
    // --- a fix from the configured port (a file of NMEA): logged, then EOF -> GPS not Found! ---
    {
        const std::string nmea = "/tmp/wardrive_selfcheck_" + std::to_string(::getpid()) + ".nmea";
        FILE* f = std::fopen(nmea.c_str(), "w");
        assert(f);
        std::fputs("$GNGGA,152010.00,3540.87416,N,13946.02750,E,1,07,1.20,41.2,M,39.0,M,,*7B\r\n"
                   "$GNRMC,152010.00,A,3540.87416,N,13946.02750,E,0.50,10.0,221126,,,A,V*3A\r\n", f);
        std::fclose(f);
        std::remove(csv.c_str());
        // An interface that does not exist: `iw` fails quickly, 0 APs.
        auto r = run({"--both", "--port", nmea, "--baud", "9600", "bruce-noif0", csv});
        assert(r.first == 0);
        assert(r.second.find("scanning Wi-Fi only") != std::string::npos);
        assert(r.second.find("GPS on " + nmea + " @ 9600 bps") != std::string::npos);
        assert(r.second.find("fix 35.68124,139.7671") != std::string::npos);
        assert(r.second.find("GPS not Found!") != std::string::npos);
        assert(stat(csv.c_str(), &st) == 0);
        std::remove(csv.c_str());
        std::remove(nmea.c_str());
    }
    // --- Auto on a silent port: SIGTERM (Back in the menu) cancels the probe at
    // once instead of after every port/baud window (5 x 1.5 s here) ---
    {
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
        assert(master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0);
        const std::string slave = ::ptsname(master);
        ::setenv("BRUCE_GPS_PORT_CANDIDATES", slave.c_str(), 1);
        const std::string out = "/tmp/wardrive_selfcheck_cancel_" + std::to_string(::getpid()) + ".txt";
        std::remove(csv.c_str());
        const pid_t pid = ::fork();
        assert(pid >= 0);
        if (pid == 0) {
            FILE* f = std::freopen(out.c_str(), "w", stdout);
            (void)f;
            std::vector<std::string> args = {"--wifi", "--port", "Auto", "--baud", "9600", "bruce-noif0", csv};
            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(a.data());
            _exit(wardrive_main(static_cast<int>(argv.size()), argv.data()));
        }
        ::usleep(300000);
        const auto t0 = std::chrono::steady_clock::now();
        ::kill(pid, SIGTERM);
        int wst = 0;
        ::waitpid(pid, &wst, 0);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::string text;
        if (FILE* f = std::fopen(out.c_str(), "r")) {
            char buf[512];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        std::remove(out.c_str());
        ::unsetenv("BRUCE_GPS_PORT_CANDIDATES");
        ::close(master);
        assert(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
        assert(text.find("Auto: searching GPS on 1 port(s)") != std::string::npos);
        assert(text.find("Stopped before a GPS was found.") != std::string::npos);
        assert(ms < 500);
        assert(stat(csv.c_str(), &st) != 0); // stopped before the CSV was created
        std::printf("wardrive Auto: SIGTERM -> exit in %lld ms\n", static_cast<long long>(ms));
    }
#endif

    std::puts("wardrive self-check OK");
    return 0;
}
