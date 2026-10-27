/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the Evil Portal port and its menu drill-down.
 * Not part of the CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/platform -Isrc/model tests/evil_portal_selfcheck.cpp \
 *       src/platform/evil_portal.cpp src/platform/root_sh.cpp \
 *       src/model/tool_catalog.cpp \
 *       -o /tmp/portal_check && /tmp/portal_check
 *
 * Checks the Bruce-parity web logic (route table, capture format, CSV files,
 * verify mode) with injected hooks, plus the clone-AP state machine in the
 * Wi-Fi menu (the part that silently breaks if the builder and
 * on_navigation_action disagree on row indexes).
 */

#include "evil_portal.h"
#include "root_sh.h"
#include "tool_catalog.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace platform;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const model::ToolCategory& wifi_category() {
    model::refresh_dynamic_categories();
    for (const auto& c : model::tool_catalog()) {
        if (std::string(c.label) == "Wi-Fi") return c;
    }
    std::fprintf(stderr, "no Wi-Fi category\n");
    std::abort();
}

} // namespace

int main() {
    // --- url decoding / form parsing ---------------------------------------
    assert(url_decode("a%40b.c+de%21") == "a@b.c de!");
    assert(url_decode("100%25") == "100%");
    assert(url_decode("%zz") == "%zz"); // invalid escape stays verbatim

    auto args = parse_form_args("email=a%40b.c&password=hunter%202");
    assert(args.size() == 2);
    assert(args[0].first == "email" && args[0].second == "a@b.c");
    assert(args[1].first == "password" && args[1].second == "hunter 2");
    assert(parse_form_args("novalue").size() == 1);
    assert(parse_form_args("").empty());

    // --- Bruce's noise-key skip list ---------------------------------------
    assert(is_noise_key("q") && is_noise_key("cup2x") && is_noise_key("plain_y") &&
           is_noise_key("P1") && is_noise_key("P4"));
    assert(!is_noise_key("email") && !is_noise_key("password") && !is_noise_key("P10"));

    // --- capture formatting -------------------------------------------------
    args = parse_form_args("q=ignored&email=me%40x.org&password=pw");
    // CSV: "key: value" pairs, comma-separated, noise keys dropped.
    assert(csv_line(args) == "email: me@x.org,password: pw");
    // /creds <li> entry.
    assert(captured_li(args) == "<li>email: me@x.org<br>\npassword: pw<br>\n</li>\n");
    // Log view: first 3 chars of each key (Bruce's lastCred).
    assert(last_cred_text(args) == "ema: me@x.org\npas: pw\n");

    // --- custom HTML: <!-- AP="..." --> first-line tag ----------------------
    assert(parse_ap_tag("<!-- AP=\"Free Wifi\" -->\n<html></html>") == "Free Wifi");
    assert(parse_ap_tag("<html>\n<!-- AP=\"nope\" -->") == ""); // first line only
    assert(parse_ap_tag("<!-- AP=\"x\" --><!-- AP=\"y\" -->") == "x");

    // --- HTTP request parsing ----------------------------------------------
    {
        const HttpRequest req = parse_http_request(
            "POST /post HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Length: 24\r\n\r\n"
            "email=x@y.z&password=abc");
        assert(req.method == "POST" && req.path == "/post");
        assert(req.args.size() == 2 && req.args[1].second == "abc");
    }
    {
        const HttpRequest req = parse_http_request("GET /post?email=q%40x HTTP/1.1\r\n\r\n");
        assert(req.path == "/post" && req.args.size() == 1 && req.args[0].second == "q@x");
    }

    // --- config generators --------------------------------------------------
    const std::string hap = hostapd_conf("wlan0", "My \"Net\"", 6);
    assert(hap.find("driver=nl80211") != std::string::npos); // mandatory line
    assert(hap.find("interface=wlan0") != std::string::npos);
    assert(hap.find("ssid=My \\\"Net\\\"") != std::string::npos);
    assert(hap.find("channel=6") != std::string::npos);
    const std::string dns = dnsmasq_conf("wlan0", "192.168.4.1");
    assert(dns.find("address=/#/192.168.4.1") != std::string::npos); // DNS hijack
    assert(dns.find("dhcp-range=192.168.4.10,192.168.4.50") != std::string::npos);
    assert(dns.find("dhcp-option=6,192.168.4.1") != std::string::npos);
    assert(dns.find("leasefile=/tmp/bruce-portal/") != std::string::npos);
    assert(wpa_conf("Net", "pa\"ss").find("psk=\"pa\\\"ss\"") != std::string::npos);

    // --- route table, plain capture mode ------------------------------------
    const std::string home = "/tmp/bruce-portal-check";
    std::system(("rm -rf " + home).c_str());
    ::mkdir(home.c_str(), 0755);

    PortalOptions opts;
    opts.creds_dir = home;
    opts.html_page = default_portal_html();
    PortalState state;
    PortalHooks hooks;
    int restarts = 0;
    hooks.on_ap_restart = [&](const PortalOptions&) { ++restarts; };

    // "/" serves the portal page.
    assert(portal_handle_request(parse_http_request("GET / HTTP/1.1\r\n\r\n"), opts, state, hooks)
               .body == default_portal_html());
    assert(state.victims == 0);

    // /post captures: wifi-load animation back, CSV appended, counters bumped.
    {
        const HttpRequest req = parse_http_request(
            "POST /post HTTP/1.1\r\nContent-Length: 34\r\n\r\n"
            "email=vic@tim.org&password=letmein");
        const HttpResponse resp = portal_handle_request(req, opts, state, hooks);
        assert(resp.body == std::string(wifi_load_html()));
        assert(state.victims == 1);
        const std::string csv = slurp(home + "/BruceEvilCreds/default_creds.csv");
        assert(csv == "email: vic@tim.org,password: letmein\n");
    }

    // /creds shows the accumulated capture (Bruce's captured list page).
    {
        const HttpResponse resp =
            portal_handle_request(parse_http_request("GET /creds HTTP/1.1\r\n\r\n"), opts, state, hooks);
        assert(resp.body.find("<ol><li>email: vic@tim.org<br>") != std::string::npos);
        assert(resp.body.find("Back to Index") != std::string::npos);
        assert(resp.body.find("<title>Free Wifi</title>") != std::string::npos);
    }

    // /ssid form, then /postssid renames the AP, restarts it and resets the
    // counter (Bruce restartWiFi(reset=true)); the CSV file stays.
    {
        assert(portal_handle_request(parse_http_request("GET /ssid HTTP/1.1\r\n\r\n"), opts, state,
                                     hooks)
                   .body.find("Set a new SSID for Evil Portal:") != std::string::npos);
        const HttpRequest req =
            parse_http_request("POST /postssid HTTP/1.1\r\nContent-Length: 9\r\n\r\nssid=Cafe");
        const HttpResponse resp = portal_handle_request(req, opts, state, hooks);
        assert(opts.ap_name == "Cafe");
        assert(state.victims == 0);
        assert(restarts == 1);
        assert(resp.body.find("restarting with SSID <b>Cafe</b>") != std::string::npos);
        assert(!slurp(home + "/BruceEvilCreds/default_creds.csv").empty());
    }

    // Unknown paths: captive fallback. No args -> portal; args -> capture
    // (portals POST to arbitrary paths; Bruce's onNotFound does the same).
    {
        assert(portal_handle_request(parse_http_request("GET /generate_204 HTTP/1.1\r\n\r\n"), opts,
                                     state, hooks)
                   .body == default_portal_html());
        portal_handle_request(
            parse_http_request("GET /hotspot-detect.html?email=a@b.c HTTP/1.1\r\n\r\n"), opts,
            state, hooks);
        assert(state.victims == 1);
        const std::string csv = slurp(home + "/BruceEvilCreds/default_creds.csv");
        assert(csv.find("email: a@b.c\n") != std::string::npos);
    }

    // --- route table, verify mode (Bruce "Router Update") -------------------
    PortalOptions vopts;
    vopts.creds_dir = home;
    vopts.verify_mode = true;
    vopts.ap_name = "RouterFi";
    vopts.html_page = verify_portal_html();
    PortalState vstate;
    PortalHooks vhooks;
    vhooks.verify_password = [](const PortalOptions&, const std::string& password) {
        return password == "good";
    };

    {
        const HttpRequest req = parse_http_request(
            "POST /post HTTP/1.1\r\nContent-Length: 13\r\n\r\npassword=good");
        const HttpResponse resp = portal_handle_request(req, vopts, vstate, vhooks);
        assert(resp.body == std::string(wifi_load_html()));
        assert(vstate.victims == 1);
        const std::string csv = slurp(home + "/BruceEvilCreds/RouterFi_creds.csv");
        assert(csv == "password: good, valid: true\n");
    }
    {
        const HttpRequest req = parse_http_request(
            "POST /post HTTP/1.1\r\nContent-Length: 13\r\n\r\npassword=bad!");
        portal_handle_request(req, vopts, vstate, vhooks);
        const std::string csv = slurp(home + "/BruceEvilCreds/RouterFi_creds.csv");
        assert(csv.find("password: bad!, valid: false\n") != std::string::npos);
    }

    // --- Wi-Fi menu: Evil Portal rows + clone-AP drill-down -----------------
    std::system("rm -rf /tmp/bruce-portal");
    {
        const auto& wifi = wifi_category();
        const size_t static_count = wifi.actions.size() - 2; // scan + "(no APs yet)"
        // The static fixes are present.
        bool have_scan_root = false, have_driver = false, have_portal = false;
        for (const auto& a : wifi.actions) {
            const std::string sh = a.argv.size() >= 3 && a.argv[0] == "sh" ? a.argv[2] : "";
            if (std::string(a.label) == "Scan nearby networks") {
                have_scan_root = a.needs_root && sh.find("iw dev wlan0 scan") != std::string::npos;
            }
            if (std::string(a.label) == "Rogue open AP (hostapd)") {
                have_driver = sh.find("driver=nl80211") != std::string::npos &&
                              sh.find("dnsmasq") != std::string::npos;
            }
            if (std::string(a.label).rfind("Evil Portal:", 0) == 0) {
                have_portal = a.argv.size() >= 2 && a.argv[0].find("--evil-portal") == std::string::npos;
            }
        }
        assert(have_scan_root);
        assert(have_driver);
        assert(have_portal);

        // Root escalation: the scan row (and every other root-needing shell
        // row) must launch through the sudoers-authorized binary's --root-sh
        // passthrough - never `sudo sh`, which the one-binary rule doesn't
        // cover. This is what made the Evil Portal clone flow die with
        // "needs root" when APPLaunch starts the app non-root.
        if (::geteuid() != 0) {
            for (const auto& a : wifi.actions) {
                if (std::string(a.label) != "Scan nearby networks") continue;
                const auto launched = model::launch_argv(a);
                assert(launched.size() == 5);
                assert(launched[0] == "sudo" && launched[1] == "-n");
                assert(launched[3] == "--root-sh");
                assert(launched[4].find("iw dev wlan0 scan") != std::string::npos);
                assert(launched[4].find("id -u") == std::string::npos); // guard superseded
                break;
            }
        }
        // Unscanned state: scan row runnable, "(no APs yet)" runnable.
        assert(wifi.actions[static_count].argv.size() == 3); // sh -c <script>
        assert(wifi.actions[static_count + 1].argv.size() == 3);
    }

    // Scanned state: scan + one navigation row per AP.
    {
        ::mkdir("/tmp/bruce-portal", 0755);
        std::ofstream o("/tmp/bruce-portal/targets");
        o << "6\tHomeNet\n11\tCafe Guest\n";
    }
    int wifi_idx = -1;
    {
        const auto& cat = model::tool_catalog();
        for (size_t i = 0; i < cat.size(); ++i) {
            if (std::string(cat[i].label) == "Wi-Fi") wifi_idx = static_cast<int>(i);
        }
    }
    assert(wifi_idx >= 0);
    const size_t wifi_static =
        wifi_category().actions.size() - 3; // fixed rows (same in every state)
    {
        const auto& wifi = wifi_category();
        assert(wifi.actions[wifi_static + 1].argv.empty()); // Clone "HomeNet"
        assert(std::string(wifi.actions[wifi_static + 1].label).find("HomeNet") != std::string::npos);
        assert(std::string(wifi.actions[wifi_static + 2].label).find("Cafe Guest") !=
               std::string::npos);

        // Tap the 2nd AP row -> selected, re-entry offers back + clone run.
        model::on_navigation_action(wifi_idx, static_cast<int>(wifi_static) + 2);
    }
    {
        const auto& wifi = wifi_category();
        assert(wifi.actions.size() == wifi_static + 2);
        assert(wifi.actions[wifi_static].argv.empty()); // "< Back to AP list"
        const auto& run = wifi.actions[wifi_static + 1];
        assert(run.argv.size() == 5);
        assert(run.argv[1] == "--evil-portal" && run.argv[2] == "clone" &&
               run.argv[3] == "Cafe Guest" && run.argv[4] == "11");

        // Back row clears the selection; target rows come back (like Nmap).
        model::on_navigation_action(wifi_idx, static_cast<int>(wifi_static));
    }
    {
        const auto& wifi = wifi_category();
        assert(wifi.actions.size() == wifi_static + 3); // scan + 2 target rows
        assert(wifi.actions[wifi_static + 1].argv.empty());
        assert(wifi.actions[wifi_static + 2].argv.empty());
    }

    // --- --root-sh: the sudo'd re-exec target runs the script verbatim ------
    {
        const char* script = "printf 'root-sh ran: %s\\n' captive > /tmp/bruce-root-sh-check";
        char* sub_argv[] = {const_cast<char*>(script), nullptr};
        const pid_t pid = ::fork();
        assert(pid >= 0);
        if (pid == 0) {
            _exit(platform::root_sh_main(1, sub_argv)); // execl's /bin/sh on success
        }
        int st = 0;
        assert(::waitpid(pid, &st, 0) == pid);
        assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        assert(slurp("/tmp/bruce-root-sh-check") == "root-sh ran: captive\n");
        std::system("rm -f /tmp/bruce-root-sh-check");
    }

    std::system(("rm -rf " + home + " /tmp/bruce-portal").c_str());
    std::puts("evil portal self-check OK");
    return 0;
}
