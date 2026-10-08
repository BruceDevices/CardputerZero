/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "wardrive.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <string>

#if defined(__linux__)
#include <csignal>
#include <ctime>
#include <unistd.h>
#endif

namespace platform {
namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

// Replace CSV-hostile characters in an SSID so one field never breaks the row.
std::string sanitize_ssid(const std::string& s) {
    std::string out;
    for (char c : s) {
        out.push_back((c == ',' || c == '\n' || c == '\r') ? ' ' : c);
    }
    return out;
}

} // namespace

int freq_to_channel(int mhz) {
    if (mhz == 2484) return 14;
    if (mhz >= 2412 && mhz <= 2472) return (mhz - 2407) / 5;      // 2.4GHz ch 1-13
    if (mhz >= 5160 && mhz <= 5885) return (mhz - 5000) / 5;      // 5GHz
    if (mhz >= 5955 && mhz <= 7115) return (mhz - 5950) / 5;      // 6GHz
    return 0;
}

std::string wigle_auth(bool rsn, bool wpa, bool privacy) {
    std::string a;
    if (rsn) a = "[WPA2-PSK-CCMP]";
    else if (wpa) a = "[WPA-PSK-TKIP]";
    else if (privacy) a = "[WEP]";
    a += "[ESS]";
    return a;
}

std::vector<ScanAp> parse_iw_scan(const std::string& text) {
    std::vector<ScanAp> aps;
    ScanAp cur;
    bool have = false, rsn = false, wpa = false, privacy = false;

    auto flush = [&]() {
        if (have) {
            cur.auth = wigle_auth(rsn, wpa, privacy);
            aps.push_back(cur);
        }
    };

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.rfind("BSS ", 0) == 0) {
            flush();
            cur = ScanAp{};
            rsn = wpa = privacy = false;
            have = true;
            // "BSS aa:bb:cc:dd:ee:ff(on wlan0)" -> take the MAC token.
            std::string mac = t.substr(4);
            size_t paren = mac.find('(');
            if (paren != std::string::npos) mac = mac.substr(0, paren);
            cur.bssid = trim(mac);
        } else if (!have) {
            continue;
        } else if (t.rfind("freq:", 0) == 0) {
            cur.channel = freq_to_channel(std::atoi(t.c_str() + 5));
        } else if (t.rfind("signal:", 0) == 0) {
            cur.signal = static_cast<int>(std::atof(t.c_str() + 7)); // "-45.00 dBm"
        } else if (t.rfind("SSID:", 0) == 0) {
            cur.ssid = sanitize_ssid(trim(t.substr(5)));
        } else if (t.rfind("RSN:", 0) == 0) {
            rsn = true;
        } else if (t.rfind("WPA:", 0) == 0) {
            wpa = true;
        } else if (t.rfind("capability:", 0) == 0) {
            if (t.find("Privacy") != std::string::npos) privacy = true;
        }
    }
    flush();
    return aps;
}

bool parse_gps_tpv(const std::string& json, double& lat, double& lon, double& alt) {
    if (json.find("\"class\":\"TPV\"") == std::string::npos) return false;
    auto num = [&](const char* key, double& out) {
        size_t p = json.find(key);
        if (p == std::string::npos) return false;
        p += std::strlen(key);
        out = std::atof(json.c_str() + p);
        return true;
    };
    double la = 0, lo = 0, al = 0;
    if (!num("\"lat\":", la) || !num("\"lon\":", lo)) return false;
    num("\"alt\":", al); // altitude optional (2D fix)
    lat = la; lon = lo; alt = al;
    return true;
}

std::string wigle_header() {
    return "WigleWifi-1.4,appRelease=bruce-cp0,model=CardputerZero,release=0,"
           "device=CardputerZero,display=,board=,brand=Bruce\n"
           "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,"
           "AltitudeMeters,AccuracyMeters,Type";
}

std::string wigle_row(const ScanAp& ap, double lat, double lon, double alt, const std::string& when) {
    char buf[384];
    std::snprintf(buf, sizeof(buf), "%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%d,WIFI",
                  ap.bssid.c_str(), ap.ssid.c_str(), ap.auth.c_str(), when.c_str(),
                  ap.channel, ap.signal, lat, lon, alt, 0);
    return buf;
}

#if defined(__linux__)
namespace {
volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

std::string now_utc() {
    time_t t = time(nullptr);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
    return b;
}

// Read a gpsd fix via gpspipe (bounded by `timeout`). Returns false if no fix.
bool get_fix(double& lat, double& lon, double& alt) {
    const std::string js = run("timeout 6 gpspipe -w -n 40 2>/dev/null");
    std::istringstream in(js);
    std::string line;
    while (std::getline(in, line)) {
        if (parse_gps_tpv(line, lat, lon, alt)) return true;
    }
    return false;
}
} // namespace

int wardrive_main(int argc, char** argv) {
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    const std::string iface = (argc >= 1 && argv[0] && argv[0][0]) ? argv[0] : "wlan0";
    std::string outfile;
    if (argc >= 2 && argv[1] && argv[1][0]) {
        outfile = argv[1];
    } else {
        const char* home = std::getenv("HOME");
        std::string dir = (home && home[0]) ? std::string(home) + "/.bruce" : "/tmp";
        run("mkdir -p '" + dir + "'");
        char ts[20];
        time_t t = time(nullptr);
        struct tm tmv;
        gmtime_r(&t, &tmv);
        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tmv);
        outfile = dir + "/wardrive-" + ts + ".csv";
    }

    FILE* csv = fopen(outfile.c_str(), "w");
    if (!csv) {
        std::printf("error: cannot open %s for writing\n", outfile.c_str());
        return 1;
    }
    std::fprintf(csv, "%s\n", wigle_header().c_str());
    std::fflush(csv);

    std::printf("Wardriving on %s -> %s\n", iface.c_str(), outfile.c_str());
    std::printf("Upload the CSV to wigle.net when done. Ctrl-C / Back to stop.\n");

    std::set<std::string> seen;
    bool warned_gps = false;
    while (!g_stop) {
        double lat = 0, lon = 0, alt = 0;
        if (!get_fix(lat, lon, alt)) {
            if (!warned_gps) {
                std::printf("waiting for a GPS fix (is gpsd running with a GPS attached?)...\n");
                warned_gps = true;
            }
            for (int i = 0; i < 20 && !g_stop; ++i) usleep(100000); // ~2s
            continue;
        }
        warned_gps = false;

        const std::string when = now_utc();
        const auto aps = parse_iw_scan(run("timeout 10 iw dev " + iface + " scan 2>/dev/null"));
        int added = 0;
        for (const auto& ap : aps) {
            if (ap.bssid.empty()) continue;
            if (seen.insert(ap.bssid).second) {
                std::fprintf(csv, "%s\n", wigle_row(ap, lat, lon, alt, when).c_str());
                ++added;
            }
        }
        std::fflush(csv);
        std::printf("fix %.5f,%.5f | +%d new (total %zu) @ %s\n", lat, lon, added, seen.size(), when.c_str());

        for (int i = 0; i < 20 && !g_stop; ++i) usleep(100000); // ~2s between passes
    }

    fclose(csv);
    std::printf("\nStopped. %zu networks logged to %s\n", seen.size(), outfile.c_str());
    return 0;
}
#else
int wardrive_main(int, char**) {
    std::fputs("wardriving is only available on Linux builds.\n", stdout);
    return 1;
}
#endif

} // namespace platform
