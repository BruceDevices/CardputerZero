/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "wardrive.h"

#include "gps_port.h"
#include "gps_session.h"

#include <atomic>
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
namespace gps = platform::gps;
// Set by SIGTERM (Back in the menu: ProcessRunner::stop()) / SIGINT. An
// atomic so the Auto probe can watch it as its cancel flag; lock-free, hence
// safe to store from the handler.
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "g_stop is set from a signal handler");
void on_signal(int) { g_stop.store(true); }

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

// Wait up to ~6 s (Ctrl-C aware) for a fresh position from the in-app GPS
// reader (TinyGPS++ location.isUpdated()). Returns false without a fix;
// `lost` is set when the port hung up (USB GPS unplugged).
bool get_fix(gps::Session& session, double& lat, double& lon, double& alt, bool& lost) {
    for (int i = 0; i < 60 && !g_stop; ++i) {
        bool got = false;
        session.snapshot([&](size_t, bool hung_up, gps::Parser& p) {
            lost = hung_up;
            if (p.location.isValid() && p.location.isUpdated()) {
                lat = p.location.lat();
                lon = p.location.lng();
                alt = p.altitude.isValid() ? p.altitude.meters() : 0.0;
                got = true;
            }
        });
        if (got) return true;
        if (lost) return false;
        usleep(100000);
    }
    return false;
}

void usage() {
    std::printf("usage: --wardrive [--wifi|--ble|--both] [--port <node>|Auto] [--baud <bps>] "
                "[iface] [outfile]\n");
}
} // namespace

int wardrive_main(int argc, char** argv) {
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // Options first (the GPS menu passes the configured port/baud), then the
    // historical positionals: [iface] [outfile].
    std::string mode = "wifi";
    std::string port = "/dev/serial0";
    int baud = 115200;
    if (const char* env = std::getenv("GPS_UART"); env && env[0]) port = env;
    if (const char* env = std::getenv("GPS_BAUD"); env && std::atoi(env) > 0) baud = std::atoi(env);
    std::vector<std::string> pos;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "--wifi" || a == "--ble" || a == "--both") {
            mode = a.substr(2);
        } else if ((a == "--port" || a == "--baud") && i + 1 < argc) {
            const std::string v = argv[++i];
            if (a == "--port") port = v;
            else baud = std::atoi(v.c_str());
        } else if (a.rfind("--", 0) == 0) {
            usage();
            return 2;
        } else {
            pos.push_back(a);
        }
    }
    if (baud <= 0) {
        usage();
        return 2;
    }
    // Until the Bruce wardriving port lands, only the Wi-Fi pass exists.
    if (mode == "ble") {
        std::printf("BLE wardriving is not available yet on CardputerZero.\n"
                    "Use \"Scan WiFi Networks\" for now.\n");
        return 1;
    }
    if (mode == "both") {
        std::printf("BLE wardriving is not available yet: scanning Wi-Fi only.\n");
    }
    const std::string iface = (!pos.empty() && !pos[0].empty()) ? pos[0] : "wlan0";

    if (port == "Auto") {
        const auto ports = gps::auto_order("/dev/serial0", gps::port_candidates());
        std::printf("Auto: searching GPS on %zu port(s)...\n", ports.size());
        // g_stop cancels the probe within ~100 ms, so Back (SIGTERM) does not
        // wait for every port/baud window to run out.
        const auto res = gps::auto_detect(ports, baud, {9600, 19200, 38400, 57600, 115200}, 1500, &g_stop);
        if (res.kind == gps::ProbeResult::Kind::Cancelled || g_stop) {
            std::printf("\nStopped before a GPS was found.\n");
            return 0;
        }
        if (res.kind != gps::ProbeResult::Kind::Found) {
            std::printf("%s\n", res.message.c_str());
            return 1;
        }
        std::printf("Auto: %s\n", res.message.c_str());
        port = res.port;
        baud = res.baud;
    }
    // The fix comes from the GPS serial port directly (no gpsd): opened for
    // the whole run, released on exit.
    if (g_stop) {
        return 0;
    }
    gps::Session session;
    gps::OpenResult open_res;
    if (!session.open(port, baud, open_res)) {
        std::printf("%s\n", open_res.message.c_str());
        return 1;
    }
    std::printf("GPS on %s @ %d bps\n", port.c_str(), baud);

    std::string outfile;
    if (pos.size() >= 2 && !pos[1].empty()) {
        outfile = pos[1];
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
        bool lost = false;
        if (!get_fix(session, lat, lon, alt, lost)) {
            if (lost) {
                std::printf("GPS not Found!\n");
                break;
            }
            if (!warned_gps && !g_stop) {
                std::printf("waiting for a GPS fix (antenna outdoors; check GPS > Config "
                            "port/baud if this never ends)...\n");
                warned_gps = true;
            }
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

    session.close();
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
