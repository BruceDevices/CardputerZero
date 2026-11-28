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
 *       src/model/tool_catalog.cpp src/model/gps_config.cpp \
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
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
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

// A STATEFUL fake environment for PortalNet lifecycle tests: it models the
// firewall (an nft table flag + a set of iptables rule keys) so a delete that
// is *issued* is not mistaken for an effect *undone*. `fail` makes matching
// commands fail WITHOUT applying their effect (a stuck add/delete, or - for an
// inspection - an exec/permission error that must read as Unknown, not Absent).
struct FakeSys {
    std::vector<std::string> log;              // exec + write:/kill: markers, in order
    std::map<std::string, std::string> output; // substring -> forced stdout (e.g. nmcli reads)
    std::vector<std::string> present;          // path_exists true if a path contains any of these
    std::vector<std::string> tools{"nft"};     // have_cmd true for these
    std::vector<std::string> fail;             // substrings: command always fails, effect NOT applied
    std::map<std::string, int> fail_n;         // substring -> remaining times to fail (transient)
    bool port_free_val = true;
    bool connected_val = false;

    // AP readiness inputs (drive the REAL wait/parse logic in PortalNet).
    bool pid_alive_val = true;               // hostapd pidfile's process is alive
    std::string ap_log = "wlan0: AP-ENABLED\n"; // default: AP up
    std::vector<std::string> ap_log_seq;     // if set, consumed per read (last entry repeats)
    size_t ap_log_i = 0;
    std::string next_ap_log() {
        if (ap_log_seq.empty()) return ap_log;
        const std::string s = ap_log_seq[std::min(ap_log_i, ap_log_seq.size() - 1)];
        ++ap_log_i;
        return s;
    }

    // Modeled firewall state.
    bool nft_table = false;
    std::set<std::string> ipt; // keys like "udp:1053", "tcp:1053"

    // Modeled DEVICE autoconnect (only touched by the nmcli commands below).
    // `nmcli connection up` re-enables it as a real side effect; `nmcli device
    // set <iface> autoconnect <v>` sets it explicitly.
    std::string dev_autoconnect;

    static bool has(const std::string& c, const std::string& s) {
        return c.find(s) != std::string::npos;
    }
    bool is_fail(const std::string& c) {
        for (const auto& s : fail)
            if (has(c, s)) return true;
        for (auto& kv : fail_n) // transient: fail the first kv.second times, then succeed
            if (kv.second > 0 && has(c, kv.first)) {
                --kv.second;
                return true;
            }
        return false;
    }
    static std::string ipt_key(const std::string& c) {
        const std::string proto = has(c, "-p udp") ? "udp" : (has(c, "-p tcp") ? "tcp" : "?");
        std::string port;
        const auto pos = c.rfind(':'); // ...--to-destination 192.168.4.1:1053
        if (pos != std::string::npos) {
            port = c.substr(pos + 1);
            port = port.substr(0, port.find_first_of(" \n"));
        }
        return proto + ":" + port;
    }
    bool exists(const std::string& p) const {
        for (const auto& s : present)
            if (has(p, s)) return true;
        return false;
    }
    bool has_tool(const std::string& t) const {
        for (const auto& s : tools)
            if (s == t) return true;
        return false;
    }
    CmdResult run_cmd(const std::string& c) {
        CmdResult r;
        r.status = 0; // success by default; branches below override
        for (const auto& kv : output)
            if (has(c, kv.first)) r.out = kv.second;
        const bool forced = is_fail(c);
        if (has(c, "iptables") && has(c, " -A ")) {
            if (forced) return {1, r.out};
            ipt.insert(ipt_key(c));
            return r;
        }
        if (has(c, "iptables") && has(c, " -D ")) {
            if (forced) return {1, r.out}; // delete "fails" -> rule survives
            ipt.erase(ipt_key(c));
            return r;
        }
        if (has(c, "iptables") && has(c, " -C ")) {
            if (forced) return {2, "iptables: resource error"}; // inspection error -> Unknown
            if (ipt.count(ipt_key(c))) return {0, r.out};        // present
            return {1, "iptables: Bad rule (does a matching rule exist in that chain?)."};
        }
        if (has(c, "nft -f")) {
            if (forced) return {1, r.out};
            nft_table = true;
            return r;
        }
        if (has(c, "nft delete table")) {
            if (forced) return {1, r.out}; // delete "fails" -> table survives
            nft_table = false;
            return r;
        }
        if (has(c, "nft list table")) {
            if (forced) return {1, "Error: Operation not permitted"}; // inspection error -> Unknown
            if (nft_table) return {0, "table ip bruce_portal_dns { }"};
            return {1, "Error: No such file or directory"};
        }
        if (has(c, "nmcli") && has(c, "connection up uuid")) {
            if (!forced) dev_autoconnect = "yes"; // re-enables device autoconnect
            return {forced ? 1 : 0, r.out};
        }
        if (has(c, "nmcli device set") && has(c, " autoconnect ")) {
            if (!forced) dev_autoconnect = has(c, "autoconnect no") ? "no" : "yes";
            return {forced ? 1 : 0, r.out};
        }
        return {forced ? 1 : 0, r.out};
    }
    int count(const std::string& sub) const {
        int n = 0;
        for (const auto& c : log)
            if (has(c, sub)) ++n;
        return n;
    }
    bool ran(const std::string& sub) const { return count(sub) > 0; }

    PortalEnv env() {
        PortalEnv e;
        e.exec = [this](const std::string& c) {
            log.push_back(c);
            return run_cmd(c);
        };
        e.have_cmd = [this](const std::string& t) { return has_tool(t); };
        e.path_exists = [this](const std::string& p) { return exists(p); };
        e.port_free = [this](const std::string&, int) { return port_free_val; };
        e.write_file = [this](const std::string& p, const std::string&) {
            log.push_back("write:" + p);
        };
        e.wait_connected = [this](const std::string&) { return connected_val; };
        e.kill_pidfile = [this](const std::string& n) { log.push_back("kill:" + n); };
        e.read_file = [this](const std::string& p) {
            return has(p, "hostapd.log") ? next_ap_log() : std::string();
        };
        e.pid_alive = [this](const std::string&) { return pid_alive_val; };
        e.sleep_ms = [](int) {};
        return e;
    }
};

const model::ToolCategory& wifi_category() {
    model::refresh_dynamic_categories();
    for (const auto& c : model::tool_catalog()) {
        if (std::string(c.label) == "Wi-Fi") return c;
    }
    std::fprintf(stderr, "no Wi-Fi category\n");
    std::abort();
}

// --- Bruce password-mask modes (--pwd-mode) --------------------------------

// For each mode: the CSV row, the /creds <li> entry and lastCred all carry the
// masked password; non-password args and their order are untouched.
void check_pwd_mode_capture_masking(const std::string& home) {
    struct Case {
        PwdMode mode;
        const char* masked;
        const char* file;
    };
    const Case cases[] = {
        {PwdMode::Full, "password", "pwd_full_creds.csv"},
        {PwdMode::FirstLast, "p******d", "pwd_first_last_creds.csv"},
        {PwdMode::Hidden, "*hidden*", "pwd_hidden_creds.csv"},
        {PwdMode::Length, "8 chars", "pwd_length_creds.csv"},
    };
    for (const Case& c : cases) {
        PortalOptions opts;
        opts.creds_dir = home;
        opts.output_file = c.file;
        opts.pwd_mode = c.mode;
        opts.html_page = default_portal_html();
        PortalState state;
        PortalHooks hooks;
        const HttpRequest req = parse_http_request(
            "POST /post HTTP/1.1\r\nContent-Length: 30\r\n\r\n"
            "email=v@x.io&password=password");
        const HttpResponse resp = portal_handle_request(req, opts, state, hooks);
        assert(resp.body == std::string(wifi_load_html()));
        assert(state.victims == 1);
        assert(state.captured_html ==
               std::string("<li>email: v@x.io<br>\npassword: ") + c.masked + "<br>\n</li>\n");
        assert(state.last_cred == std::string("ema: v@x.io\npas: ") + c.masked + "\n");
        assert(slurp(home + "/BruceEvilCreds/" + c.file) ==
               std::string("email: v@x.io,password: ") + c.masked + "\n");
    }

    // FIRST_LAST_CHAR: only length > 2 is masked (0/1/2-char values verbatim);
    // the 3-char case stars exactly the single middle byte.
    {
        struct ShortCase {
            const char* body;
            std::size_t len;
            const char* masked;
            const char* file;
        };
        const ShortCase short_cases[] = {
            {"password=", 9, "", "pwd_fl_zero.csv"},
            {"password=a", 10, "a", "pwd_fl_one.csv"},
            {"password=ab", 11, "ab", "pwd_fl_two.csv"},
            {"password=abc", 12, "a*c", "pwd_fl_three.csv"},
        };
        for (const ShortCase& c : short_cases) {
            PortalOptions opts;
            opts.creds_dir = home;
            opts.output_file = c.file;
            opts.pwd_mode = PwdMode::FirstLast;
            PortalState state;
            PortalHooks hooks;
            const std::string raw = std::string("POST /post HTTP/1.1\r\nContent-Length: ") +
                                    std::to_string(c.len) + "\r\n\r\n" + c.body;
            portal_handle_request(parse_http_request(raw), opts, state, hooks);
            assert(state.last_cred == std::string("pas: ") + c.masked + "\n");
            assert(slurp(home + "/BruceEvilCreds/" + c.file) ==
                   std::string("password: ") + c.masked + "\n");
        }
    }

    // HIDE_PASSWORD masks the empty value too; SAVE_LENGTH reports 0.
    {
        PortalOptions opts;
        opts.creds_dir = home;
        opts.output_file = "pwd_hidden_empty.csv";
        opts.pwd_mode = PwdMode::Hidden;
        PortalState state;
        PortalHooks hooks;
        portal_handle_request(
            parse_http_request("POST /post HTTP/1.1\r\nContent-Length: 9\r\n\r\npassword="), opts,
            state, hooks);
        assert(state.last_cred == "pas: *hidden*\n");
        assert(slurp(home + "/BruceEvilCreds/pwd_hidden_empty.csv") == "password: *hidden*\n");
    }
    {
        PortalOptions opts;
        opts.creds_dir = home;
        opts.output_file = "pwd_length_empty.csv";
        opts.pwd_mode = PwdMode::Length;
        PortalState state;
        PortalHooks hooks;
        portal_handle_request(
            parse_http_request("POST /post HTTP/1.1\r\nContent-Length: 9\r\n\r\npassword="), opts,
            state, hooks);
        assert(state.last_cred == "pas: 0 chars\n");
        assert(slurp(home + "/BruceEvilCreds/pwd_length_empty.csv") == "password: 0 chars\n");
    }

    // Duplicate password fields are each masked, like Bruce's per-arg loop.
    {
        PortalOptions opts;
        opts.creds_dir = home;
        opts.output_file = "pwd_hidden_dup.csv";
        opts.pwd_mode = PwdMode::Hidden;
        PortalState state;
        PortalHooks hooks;
        portal_handle_request(parse_http_request("POST /post HTTP/1.1\r\nContent-Length: 25\r\n\r\n"
                                                 "password=one&password=two"),
                              opts, state, hooks);
        assert(state.last_cred == "pas: *hidden*\npas: *hidden*\n");
        assert(slurp(home + "/BruceEvilCreds/pwd_hidden_dup.csv") ==
               "password: *hidden*,password: *hidden*\n");
    }

    // Byte-wise masking (Arduino String): this password is 10 BYTES, so
    // FIRST_LAST_CHAR stars bytes 1..8 - splitting the two multibyte UTF-8
    // characters exactly as the ESP32 build does - and SAVE_LENGTH says 10.
    {
        const std::string body = "password=p%C3%A4ssw%C3%B6rd";
        const std::string raw = std::string("POST /post HTTP/1.1\r\nContent-Length: ") +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
        {
            PortalOptions opts;
            opts.creds_dir = home;
            opts.output_file = "pwd_first_last_utf8.csv";
            opts.pwd_mode = PwdMode::FirstLast;
            PortalState state;
            PortalHooks hooks;
            portal_handle_request(parse_http_request(raw), opts, state, hooks);
            assert(state.last_cred == std::string("pas: p") + std::string(8, '*') + "d\n");
        }
        {
            PortalOptions opts;
            opts.creds_dir = home;
            opts.output_file = "pwd_length_utf8.csv";
            opts.pwd_mode = PwdMode::Length;
            PortalState state;
            PortalHooks hooks;
            portal_handle_request(parse_http_request(raw), opts, state, hooks);
            assert(state.last_cred == "pas: 10 chars\n");
        }
    }
}

// Verify mode: the hook still gets the RAW password; every stored/displayed
// value is masked, in both the verify and the empty-password fallback paths.
void check_pwd_mode_verify_keeps_raw_password(const std::string& home) {
    PortalOptions opts;
    opts.creds_dir = home;
    opts.verify_mode = true;
    opts.ap_name = "MaskedRouter";
    opts.html_page = verify_portal_html();
    opts.pwd_mode = PwdMode::Hidden;
    PortalState state;
    PortalHooks hooks;
    std::string seen;
    int calls = 0;
    hooks.verify_password = [&](const PortalOptions&, const std::string& password) {
        seen = password;
        ++calls;
        return password == "good";
    };

    {
        const HttpResponse resp = portal_handle_request(
            parse_http_request("POST /post HTTP/1.1\r\nContent-Length: 13\r\n\r\npassword=good"),
            opts, state, hooks);
        assert(resp.body == std::string(wifi_load_html()));
        assert(calls == 1 && seen == "good"); // RAW, not "*hidden*"
        assert(state.last_cred == "pas: *hidden*\n");
        assert(state.captured_html.find("password: *hidden*<br>") != std::string::npos);
    }
    assert(slurp(home + "/BruceEvilCreds/MaskedRouter_creds.csv") ==
           "password: *hidden*, valid: true\n");

    portal_handle_request(
        parse_http_request("POST /post HTTP/1.1\r\nContent-Length: 13\r\n\r\npassword=bad!"), opts,
        state, hooks);
    assert(calls == 2 && seen == "bad!");
    assert(slurp(home + "/BruceEvilCreds/MaskedRouter_creds.csv") ==
           "password: *hidden*, valid: true\npassword: *hidden*, valid: false\n");

    // Empty password: the existing fallback saves to output_file, still masked,
    // and the verify hook is not called.
    portal_handle_request(
        parse_http_request("POST /post HTTP/1.1\r\nContent-Length: 9\r\n\r\npassword="), opts,
        state, hooks);
    assert(calls == 2);
    assert(slurp(home + "/BruceEvilCreds/default_creds.csv").find("password: *hidden*\n") !=
           std::string::npos);
}

// --pwd-mode parsing: exact value mapping, errors, and - critically - the
// value is consumed so it never becomes an SSID/custom-path/clone-channel
// positional. Pure parser, so no root or network is involved.
void check_pwd_mode_cli_parse() {
    auto parse = [](std::vector<const char*> items) {
        std::vector<char*> argv;
        argv.reserve(items.size());
        for (const char* s : items) argv.push_back(const_cast<char*>(s));
        return parse_portal_cli(static_cast<int>(argv.size()), argv.data(), "wlan0");
    };

    {
        const PortalCli cli = parse({});
        assert(cli.ok && cli.pos.empty() && cli.iface == "wlan0");
        assert(cli.opts.pwd_mode == PwdMode::Full); // default
    }
    {
        const PortalCli cli = parse({"--pwd-mode", "hidden"});
        assert(cli.ok && cli.opts.pwd_mode == PwdMode::Hidden && cli.pos.empty());
    }
    {
        const PortalCli cli = parse({"--pwd-mode", "first_last", "MySSID"});
        assert(cli.ok && cli.opts.pwd_mode == PwdMode::FirstLast);
        assert(cli.pos.size() == 1 && cli.pos[0] == "MySSID"); // "first_last" consumed
    }
    {
        const PortalCli cli = parse({"--pwd-mode", "length", "/tmp/portal.html"});
        assert(cli.ok && cli.opts.pwd_mode == PwdMode::Length);
        assert(cli.pos.size() == 1 && cli.pos[0] == "/tmp/portal.html");
    }
    {
        const PortalCli cli = parse({"Cafe", "--pwd-mode", "full", "11"}); // clone shape
        assert(cli.ok && cli.opts.pwd_mode == PwdMode::Full);
        assert(cli.pos.size() == 2 && cli.pos[0] == "Cafe" && cli.pos[1] == "11");
    }
    {
        const PortalCli cli = parse(
            {"--gateway", "10.0.0.1", "--iface", "wlan1", "--channel", "6", "--country", "jp"});
        assert(cli.ok && cli.opts.gateway == "10.0.0.1" && cli.iface == "wlan1");
        assert(cli.opts.channel == 6 && cli.opts.country == "jp");
        // The parser never fills these: evil_portal_main sets them afterwards.
        assert(cli.opts.html_page.empty() && cli.opts.creds_dir.empty());
    }
    {
        const PortalCli cli = parse({"--country", "USA"});
        assert(!cli.ok && !cli.show_usage); // pre-existing: error only, no usage
        assert(cli.error == "--country must be a 2-letter code (e.g. US, JP)");
    }
    {
        const PortalCli cli = parse({"--pwd-mode", "bogus"});
        assert(!cli.ok && cli.show_usage && cli.pos.empty());
        assert(cli.error.find("--pwd-mode") != std::string::npos);
    }
    {
        const PortalCli cli = parse({"--pwd-mode"}); // missing value
        assert(!cli.ok && cli.show_usage && cli.pos.empty());
        assert(cli.error.find("--pwd-mode") != std::string::npos);
    }
    {
        const PortalCli cli = parse({"--nope"});
        assert(!cli.ok && cli.show_usage && cli.error == "unknown option --nope");
    }
}

// End to end: an invalid --pwd-mode exits 1 before any root/network work and
// prints the error plus the usage block.
void check_pwd_mode_cli_error() {
    const std::string out_path = "/tmp/bruce-pwd-mode-cli.txt";
    ::unlink(out_path.c_str());
    char* argv[] = {const_cast<char*>("default"), const_cast<char*>("--pwd-mode"),
                    const_cast<char*>("bogus")};
    std::fflush(stdout);
    const pid_t pid = ::fork();
    assert(pid >= 0);
    if (pid == 0) {
        if (std::freopen(out_path.c_str(), "w", stdout) == nullptr) _exit(2);
        const int rc = evil_portal_main(3, argv);
        std::fflush(nullptr); // redirected printf output must reach the file
        _exit(rc);
    }
    int st = 0;
    assert(::waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 1);
    const std::string out = slurp(out_path);
    assert(out.find("error: --pwd-mode") != std::string::npos);
    assert(out.find("usage: bruce_cardputerzero --evil-portal") != std::string::npos);
    ::unlink(out_path.c_str());
}

// The run-log labels must match Bruce's drawScreen() text exactly.
void check_pwd_mode_labels() {
    assert(std::string(pwd_mode_label(PwdMode::Full)) == "Full");
    assert(std::string(pwd_mode_label(PwdMode::FirstLast)) == "p******d");
    assert(std::string(pwd_mode_label(PwdMode::Hidden)) == "*hidden*");
    assert(std::string(pwd_mode_label(PwdMode::Length)) == "Length only");
    assert(std::string(counter_label(false)) == "Victims");
    assert(std::string(counter_label(true)) == "Attempt");
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

    // --- custom HTML: output CSV name (Bruce lowercases the basename) --------
    assert(custom_csv_name("/home/pi/.bruce/MyPortal.html") == "myportal_creds.csv");
    assert(custom_csv_name("~/portal/Login_Page.html") == "login_page_creds.csv");
    assert(custom_csv_name("ALLCAPS.html") == "allcaps_creds.csv");
    assert(custom_csv_name("noext") == "noext_creds.csv"); // no ".html" suffix to strip
    // ".html" stripped case-insensitively (MyPortal.HTML -> myportal_creds.csv).
    assert(custom_csv_name("MyPortal.HTML") == "myportal_creds.csv");
    assert(custom_csv_name("ALLCAPS.HTML") == "allcaps_creds.csv");
    // ASCII-only lowercasing: A-Z map, the boundary bytes '@' (0x40, before
    // 'A') and '[' (0x5B, after 'Z') and other non-letters stay unchanged.
    assert(custom_csv_name("@AZ[-1.html") == "@az[-1_creds.csv");

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
    const std::string hap = hostapd_conf("wlan0", "My \"Net\"", 6, "JP");
    assert(hap.find("driver=nl80211") != std::string::npos); // mandatory line
    assert(hap.find("interface=wlan0") != std::string::npos);
    assert(hap.find("ssid=My \\\"Net\\\"") != std::string::npos);
    assert(hap.find("channel=6") != std::string::npos);
    // brcmfmac needs a regdom to beacon: country_code + ieee80211d.
    assert(hap.find("\ncountry_code=JP\n") != std::string::npos);
    assert(hap.find("\nieee80211d=1\n") != std::string::npos);
    assert(hostapd_conf("wlan0", "x", 1, "").find("country_code") == std::string::npos);

    // country-code validation + `iw reg get` parsing.
    assert(valid_country("US") && valid_country("jp") && !valid_country("USA") &&
           !valid_country("U1") && !valid_country("U"));
    assert(parse_reg_country("global\ncountry JP: DFS-JP\n\t(2402 - 2482 @ 40)\n") == "JP");
    assert(parse_reg_country("country gb: DFS-ETSI\n") == "GB"); // lowercased input -> upper
    assert(parse_reg_country("global\ncountry 00: DFS-UNSET\n").empty()); // world / unset
    assert(parse_reg_country("").empty());
    const std::string dns = dnsmasq_conf("wlan0", "192.168.4.1");
    assert(dns.find("address=/#/192.168.4.1") != std::string::npos); // DNS hijack
    assert(dns.find("dhcp-range=192.168.4.10,192.168.4.50") != std::string::npos);
    assert(dns.find("dhcp-option=6,192.168.4.1") != std::string::npos);
    // DNS binds an alternate high port (default 1053) so it coexists with a
    // system resolver on :53; a DNAT (below) redirects clients' :53 to it.
    assert(dns.find("\nport=1053\n") != std::string::npos);
    assert(dns.find("\nno-hosts\n") != std::string::npos); // /etc/hosts never overrides wildcard
    assert(dns.find("\nlocal=/#/\n") != std::string::npos); // answer locally, never forward
    // DHCP stays on 67: no "port=" should touch the DHCP side (option 6 has no port).
    assert(dnsmasq_conf("wlan0", "192.168.4.1", 1054).find("\nport=1054\n") != std::string::npos);
    // Must be the valid dnsmasq keyword (dhcp-leasefile), not the bogus
    // "leasefile=" which makes dnsmasq reject the config and never serve DHCP.
    assert(dns.find("dhcp-leasefile=/tmp/bruce-portal/") != std::string::npos);
    assert(dns.find("\nleasefile=") == std::string::npos);
    assert(wpa_conf("Net", "pa\"ss").find("psk=\"pa\\\"ss\"") != std::string::npos);

    // --- captive DNS redirect helpers (nft/iptables DNAT + teardown) --------
    assert(std::string(dns_dnat_table()) == "bruce_portal_dns");
    assert(dns_port_candidates().front() == 1053); // 1053 first, never 5353 (mDNS)
    assert(dns_port_candidates().size() >= 2);      // has fallbacks (1054, ...)
    {
        const std::string nft = nft_dnat_script("wlan0", "192.168.4.1", 1053);
        assert(nft.find("add table ip bruce_portal_dns\n") != std::string::npos);
        assert(nft.find("type nat hook prerouting priority -101") != std::string::npos);
        // udp AND tcp :53 both DNAT-ed to the gateway's alternate DNS port,
        // scoped to the AP interface and the gateway address.
        assert(nft.find("iifname \"wlan0\" ip daddr 192.168.4.1 udp dport 53 "
                        "dnat to 192.168.4.1:1053") != std::string::npos);
        assert(nft.find("iifname \"wlan0\" ip daddr 192.168.4.1 tcp dport 53 "
                        "dnat to 192.168.4.1:1053") != std::string::npos);
        // Exact teardown command: removes ONLY our table.
        assert(nft_dnat_delete() == "delete table ip bruce_portal_dns");

        // iptables fallback: tagged, -w (lock-wait) add/delete/check for udp AND tcp.
        const std::string ipt_add = iptables_dnat_add("wlan0", "192.168.4.1", 1053);
        assert(ipt_add.find("iptables -w -t nat -A PREROUTING -i wlan0 -d 192.168.4.1 -p udp "
                            "--dport 53 -m comment --comment bruce_portal_dns -j DNAT "
                            "--to-destination 192.168.4.1:1053") != std::string::npos);
        assert(ipt_add.find("-p tcp --dport 53") != std::string::npos);
        const std::string ipt_del = iptables_dnat_delete("wlan0", "192.168.4.1", 1053);
        assert(ipt_del.find("iptables -w -t nat -D PREROUTING -i wlan0") != std::string::npos);
        assert(ipt_del.find("--comment bruce_portal_dns") != std::string::npos);
        // -C check mirrors the add, so teardown can tell absent from failed.
        const std::string ipt_chk = iptables_dnat_check("wlan0", "192.168.4.1", 1053);
        assert(ipt_chk.find("iptables -w -t nat -C PREROUTING -i wlan0") != std::string::npos);
    }

    // --- UUID validation + self-managed detection ---------------------------
    assert(valid_uuid("11111111-2222-3333-4444-555555555555"));
    assert(!valid_uuid("not-a-uuid"));
    assert(!valid_uuid("11111111-2222-3333-4444-555555555555 ; rm -rf /")); // no injection
    assert(!valid_uuid("11111111222233334444555555555555"));                // missing dashes
    assert(reg_is_self_managed("global\ncountry 00: DFS-UNSET\nphy#0 (self-managed)\n"));
    assert(!reg_is_self_managed("global\ncountry JP: DFS-JP\n"));

    // --- wiphy index parse + PHY-scoped self-managed detection --------------
    assert(parse_wiphy_index("Interface wlan0\n\tifindex 3\n\twiphy 0\n\ttype managed\n") == 0);
    assert(parse_wiphy_index("Interface wlan1\n\twiphy 7\n") == 7);
    assert(parse_wiphy_index("Interface wlan0\n\ttype managed\n") == -1); // no wiphy line
    {
        // Two PHYs, the '(self-managed)' marker ONLY on phy#1. The target (phy#0)
        // must read non-self-managed in BOTH section orderings.
        const std::string a = "global\ncountry 00: DFS-UNSET\n\n"
                              "phy#0\ncountry US: DFS-FCC\n\n"
                              "phy#1 (self-managed)\ncountry 00: DFS-UNSET\n";
        const std::string b = "global\ncountry 00: DFS-UNSET\n\n"
                              "phy#1 (self-managed)\ncountry 00: DFS-UNSET\n\n"
                              "phy#0\ncountry US: DFS-FCC\n";
        assert(!phy_is_self_managed(a, 0) && phy_is_self_managed(a, 1));
        assert(!phy_is_self_managed(b, 0) && phy_is_self_managed(b, 1));
        // Exact index match: looking for phy#1 must not hit phy#10.
        const std::string c = "phy#10 (self-managed)\ncountry 00: DFS-UNSET\n";
        assert(!phy_is_self_managed(c, 1) && phy_is_self_managed(c, 10));
        assert(!phy_is_self_managed(a, -1)); // unresolved wiphy -> not self-managed
    }

    // --- AP-ENABLED EXISTENCE (diagnostics) vs LAST state (readiness) -------
    assert(ap_event_ever_enabled("wlan0", "wlan0: AP-ENABLED\nwlan0: AP-DISABLED\n"));
    assert(!ap_event_enabled("wlan0", "wlan0: AP-ENABLED\nwlan0: AP-DISABLED\n"));
    assert(!ap_event_ever_enabled("wlan0", "wlan0: AP-DISABLED\n")); // never enabled
    assert(ap_event_ever_enabled("wlan0", "wlan0: AP-ENABLED \n"));  // trailing space tolerated
    assert(!ap_event_ever_enabled("wlan0", "wlan1: AP-ENABLED\n"));  // different iface

    // --- full interface release sequence (exact order) ----------------------
    {
        const std::vector<std::string> rel = iface_release_commands("wlan0");
        assert(rel.size() == 3);
        assert(rel[0] == "nmcli --wait 15 device disconnect wlan0"); // break association FIRST
        assert(rel[1] == "nmcli device set wlan0 managed no");       // then unmanage
        assert(rel[2] == "ip link set dev wlan0 down");              // then down
    }

    // --- interface restore sequence (reverse; managed-only NM parts) --------
    {
        NetState st;
        st.iface = "wlan0";
        st.gateway = "192.168.4.1";
        st.nmcli_present = true;
        st.ownership_read_ok = true;
        st.iface_was_managed = true;
        st.active_uuid = "11111111-2222-3333-4444-555555555555";
        st.autoconnect = "yes";
        st.regdom_changed = true;
        st.orig_country = "JP";
        const std::vector<std::string> r = iface_restore_commands(st);
        // ip addr del -> link down -> type managed -> link up -> regdom -> NM...
        assert(r[0] == "ip addr del 192.168.4.1/24 dev wlan0");
        assert(r[1] == "ip link set dev wlan0 down");
        assert(r[2] == "iw dev wlan0 set type managed");
        assert(r[3] == "ip link set dev wlan0 up");
        assert(r[4] == "iw reg set JP"); // original regdom restored
        assert(r[5] == "nmcli device set wlan0 managed yes");
        // DEVICE autoconnect restored (not the profile's connection.autoconnect).
        assert(r[6] == "nmcli device set wlan0 autoconnect yes");
        assert(r[7] == "nmcli --wait 30 connection up uuid "
                       "11111111-2222-3333-4444-555555555555 ifname wlan0");
        assert(r.size() == 8);
        // We must NOT rewrite the profile's connection.autoconnect.
        for (const auto& c : r) assert(c.find("connection.autoconnect") == std::string::npos);
        assert(iface_restore_commands(st) == r); // deterministic

        NetState unmanaged = st; // never managed -> cycle the link, but NO nmcli parts
        unmanaged.iface_was_managed = false;
        const std::vector<std::string> u = iface_restore_commands(unmanaged);
        assert(u.size() == 5 && u[4] == "iw reg set JP"); // link cycle + regdom only
        for (const auto& c : u) assert(c.find("nmcli") == std::string::npos);

        NetState no_regdom = st; // regdom not changed -> no `iw reg set`
        no_regdom.regdom_changed = false;
        const std::vector<std::string> n = iface_restore_commands(no_regdom);
        for (const auto& c : n) assert(c.rfind("iw reg set", 0) != 0);

        NetState no_ac = st; // no saved device-autoconnect -> no autoconnect set, but still up
        no_ac.autoconnect.clear();
        const std::vector<std::string> a = iface_restore_commands(no_ac);
        bool has_ac = false, has_up = false;
        for (const auto& c : a) {
            if (c.find("device set wlan0 autoconnect") != std::string::npos) has_ac = true;
            if (c.find("connection up uuid") != std::string::npos) has_up = true;
        }
        assert(!has_ac && has_up);

        // Managed but DISCONNECTED (no active UUID) + device-autoconnect yes:
        // restore the device autoconnect, but issue NO `connection up`.
        NetState disc = st;
        disc.active_uuid.clear();
        disc.autoconnect = "yes";
        const std::vector<std::string> d = iface_restore_commands(disc);
        bool d_ac = false, d_up = false;
        for (const auto& c : d) {
            if (c == "nmcli device set wlan0 autoconnect yes") d_ac = true;
            if (c.find("connection up uuid") != std::string::npos) d_up = true;
        }
        assert(d_ac && !d_up);

        // Originally-DISABLED device autoconnect + active UUID: `connection up`
        // re-enables the device autoconnect, so the saved "no" must be RE-APPLIED
        // AFTER the up. Expect: set no (pre), connection up, set no (post).
        NetState off = st;
        off.autoconnect = "no";
        const std::vector<std::string> o = iface_restore_commands(off);
        int set_no = 0, up_idx = -1, last_set_no = -1;
        for (int i = 0; i < static_cast<int>(o.size()); ++i) {
            if (o[i] == "nmcli device set wlan0 autoconnect no") {
                ++set_no;
                last_set_no = i;
            }
            if (o[i].find("connection up uuid") != std::string::npos) up_idx = i;
        }
        assert(set_no == 2 && up_idx >= 0 && last_set_no > up_idx); // re-applied AFTER up

        // Disabled autoconnect but NO active UUID: no `connection up`, so the
        // device setting is never re-enabled -> a single "no" suffices.
        NetState off_disc = off;
        off_disc.active_uuid.clear();
        const std::vector<std::string> od = iface_restore_commands(off_disc);
        int od_set_no = 0, od_up = 0;
        for (const auto& c : od) {
            if (c == "nmcli device set wlan0 autoconnect no") ++od_set_no;
            if (c.find("connection up uuid") != std::string::npos) ++od_up;
        }
        assert(od_set_no == 1 && od_up == 0);

        // Originally-ENABLED autoconnect + UUID: `connection up` leaves it "yes",
        // so NO extra re-apply is emitted (restoring yes would be a no-op).
        int yes_set = 0;
        for (const auto& c : r)
            if (c == "nmcli device set wlan0 autoconnect yes") ++yes_set;
        assert(yes_set == 1);
    }

    // --- PortalNet lifecycle (injected fake runner) -------------------------
    const PortalOptions lopts = [] {
        PortalOptions o;
        o.ap_name = "Free Wifi";
        o.gateway = "192.168.4.1";
        return o;
    }();
    // Drive the production precondition: iw present + a readable regdom, then
    // resolve_country (US). start() refuses to act until a country is resolved,
    // so every lifecycle case goes through the real resolve path (no shortcut).
    auto prepare = [](FakeSys& fs, PortalNet& net) {
        fs.tools.push_back("iw");
        fs.output["iw reg get"] = "country US:\n";
        std::string e;
        assert(net.resolve_country("", e));
    };

    {   // regdom guard: start() with NO resolved country issues NOTHING.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1"); // deliberately NOT prepared
        const StartResult r = net.start(lopts);
        assert(!r.ok() && !r.regdom && fs.log.empty()); // not a single command issued
    }

    {   // hostapd never reaches AP-ENABLED (pid dead): regdom set, then bail
        // before addressing / DNS / DNAT.
        FakeSys fs;
        fs.pid_alive_val = false; // hostapd pid never alive -> never ready
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        const StartResult r = net.start(lopts);
        assert(r.regdom && !r.hostapd && !r.dnsmasq && !r.dnat && !r.ok());
        assert(fs.ran("iw reg set US"));     // regdom set before hostapd
        assert(!fs.ran("ip addr replace"));  // but bailed before addressing
        assert(!fs.ran("dnsmasq -C"));
        assert(!fs.ran("nft -f") && !fs.nft_table && fs.ipt.empty());
    }

    {   // readiness honours the LAST AP event: enabled-then-disabled is NOT up.
        FakeSys fs;
        fs.ap_log = "wlan0: AP-ENABLED\nwlan0: AP-DISABLED\n";
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        const StartResult r = net.start(lopts);
        assert(!r.hostapd); // the disable cancels the earlier enable
    }

    {   // FAILED regdom set: no hostapd / addressing / DNS / DNAT follows.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        fs.fail = {"iw reg set"}; // the global regdom request fails
        const StartResult r = net.start(lopts);
        assert(!r.regdom && !r.ok());
        assert(!fs.ran("hostapd -B") && !fs.ran("ip addr replace") && !fs.ran("dnsmasq -C") &&
               !fs.ran("nft -f"));
    }

    {   // hostapd up, dnsmasq never: every candidate port tried, DNAT skipped.
        FakeSys fs;
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        const StartResult r = net.start(lopts);
        assert(r.hostapd && !r.dnsmasq && !r.dnat);
        assert(fs.count("dnsmasq -C") == (int)dns_port_candidates().size()); // retried each
        assert(!fs.ran("nft -f") && !fs.nft_table); // DNAT only after the resolver is up
    }

    {   // AP-restart failure propagates through verify(): restart not ok.
        FakeSys fs;              // dnsmasq.pid never present -> restart can't finish
        fs.connected_val = true; // password would verify
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        StartResult restart;
        const bool connected = net.verify(lopts, "pw", restart);
        assert(connected);                   // wait_connected said COMPLETED
        assert(!restart.ok());               // but the AP did not come back up
        assert(fs.ran("wpa_supplicant -B")); // our own supplicant, by pidfile
        assert(fs.ran("kill:wpa"));
    }

    {   // FINDING 1: iptables partial install (udp ok, tcp add fails) then a
        // FAILED udp rollback must leave the surviving udp rule TRACKED, and a
        // later cleanup must actually undo it (not merely issue a delete).
        FakeSys fs;
        fs.tools = {"iptables"};
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        fs.fail = {"-A PREROUTING -i wlan0 -d 192.168.4.1 -p tcp",  // 2nd add fails
                   "-D PREROUTING -i wlan0 -d 192.168.4.1 -p udp"}; // udp rollback delete fails
        const StartResult r = net.start(lopts);
        assert(r.hostapd && r.dnsmasq && !r.dnat);        // firewall install failed
        assert(fs.ipt.count("udp:1053") == 1);            // udp rule really survives
        assert(net.state().dnat_rules.size() == 1 &&      // and is still OWNED
               net.state().dnat_rules[0].proto == "udp");
        // Lock clears; cleanup must retry the surviving rule and CONFIRM removal.
        fs.fail.clear();
        assert(net.cleanup());
        assert(fs.ipt.count("udp:1053") == 0);            // effect actually undone
        assert(net.state().dnat_rules.empty());
    }

    {   // FINDING 4: a restart whose teardown fails must NOT bring a new AP up
        // (which could pick a new port and strand clients on the dead old one).
        FakeSys fs;
        fs.tools = {"iptables"};
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.connected_val = true;
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());                    // running on 1053
        assert(fs.ipt.count("udp:1053") && fs.ipt.count("tcp:1053"));
        fs.fail = {"-D PREROUTING -i wlan0 -d 192.168.4.1 -p udp",
                   "-D PREROUTING -i wlan0 -d 192.168.4.1 -p tcp"}; // removal stuck
        StartResult restart;
        (void)net.verify(lopts, "pw", restart);
        assert(!restart.ok());                            // AP NOT restarted
        assert(fs.ipt.count("udp:1053") && fs.ipt.count("tcp:1053")); // old rules retained
        assert(!fs.ipt.count("udp:1054") && !fs.ran(":1054")); // no new-port rule installed
        assert(net.state().dnat_rules.size() == 2);       // still owed removal
        fs.fail.clear();                                  // lock clears -> cleanup finishes
        assert(net.cleanup());
        assert(fs.ipt.empty());
    }

    {   // cleanup twice, happy path: removes the table, second call is a no-op.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"}; // tools default {"nft"}
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        assert(net.state().dnat == DnatBackend::Nft && fs.nft_table);
        assert(net.cleanup());                 // confirmed removal
        assert(!fs.nft_table && net.state().dnat_rules.empty()); // effect undone
        const size_t after_first = fs.log.size();
        assert(net.cleanup());                 // idempotent no-op
        assert(fs.log.size() == after_first);  // issued nothing new
    }

    {   // FINDING 3 state: a FAILED delete keeps ownership and retries.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        fs.fail = {"nft delete table"}; // delete never takes -> table survives
        const int del_before = fs.count("nft delete table");
        assert(!net.cleanup());                 // not clean (table still present)
        assert(!net.cleanup());                 // retried, not given up
        assert(fs.nft_table);                   // really still there
        assert(net.state().dnat_rules.size() == 1); // ownership retained
        assert(fs.count("nft delete table") == del_before + 2);
    }

    {   // FINDING 2a: delete SUCCEEDS but inspection fails -> Unknown, NOT
        // "absent": we must retain ownership rather than claim a clean teardown.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        fs.fail = {"nft list table"}; // presence probe errors out
        assert(!net.cleanup());                     // Unknown state -> not fully clean
        assert(!fs.nft_table);                      // (actually gone, but we can't confirm)
        assert(net.state().dnat_rules.size() == 1); // ownership retained on unknown
    }

    {   // FINDING 2b: BOTH delete and inspection fail -> ownership retained.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        fs.fail = {"nft delete table", "nft list table"};
        assert(!net.cleanup());
        assert(fs.nft_table);                       // still present
        assert(net.state().dnat_rules.size() == 1); // not dropped
    }

    {   // a failed (non-benign) restore step counts against "fully clean"...
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        fs.fail = {"iw dev wlan0 set type managed"}; // a restore step fails
        assert(!net.cleanup()); // DNAT removed, but restore incomplete -> not fully clean
    }

    {   // a failed `ip addr del` is benign ONLY when the address is CONFIRMED
        // gone: the inspection query shows it absent -> cleanup still clean.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        fs.fail = {"ip addr del"}; // delete returns nonzero...
        // default `ip -o -4 addr show` output is empty -> address confirmed gone
        assert(net.cleanup());     // benign absence -> still fully clean
    }

    {   // GENUINE deletion failure: the delete fails AND the address is still
        // present afterward -> NOT clean (must not be silenced as "absence").
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        fs.fail = {"ip addr del"};
        fs.output["ip -o -4 addr show"] =
            "3: wlan0    inet 192.168.4.1/24 brd 192.168.4.255 scope global wlan0\n";
        assert(!net.cleanup());            // address remains -> not fully clean
        assert(!net.cleanup());            // retried, not given up
    }

    {   // delete fails AND the inspection query itself fails -> UNKNOWN, which is
        // NOT confirmed absence -> not clean (we don't assume it worked).
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        fs.fail = {"ip addr del", "ip -o -4 addr show"};
        assert(!net.cleanup());
    }

    {   // REGRESSION FIX: nft load fails but is CONFIRMED gone -> fall back to
        // iptables, which succeeds, so startup is OK.
        FakeSys fs;
        fs.tools = {"nft", "iptables"};
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        fs.fail = {"nft -f"}; // nft install fails; its rollback delete+probe confirm absence
        const StartResult r = net.start(lopts);
        assert(r.ok());                                 // fell back successfully
        assert(net.state().dnat == DnatBackend::Iptables);
        assert(!fs.nft_table && fs.ipt.count("udp:1053") && fs.ipt.count("tcp:1053"));
    }

    {   // ...but UNRESOLVED nft ownership must PREVENT fallback (can't stack
        // iptables on unknown nft state): startup fails, nft entry retained.
        FakeSys fs;
        fs.tools = {"nft", "iptables"};
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        fs.fail = {"nft -f", "nft delete table", "nft list table"}; // load fails, rollback unknown
        const StartResult r = net.start(lopts);
        assert(!r.dnat);                   // did not fall back
        assert(fs.ipt.empty());            // no iptables rule was attempted
        assert(net.state().dnat_rules.size() == 1 &&
               net.state().dnat_rules[0].backend == DnatBackend::Nft); // ownership retained
    }

    {   // SHARED TEARDOWN: a TRANSIENT failure clears on retry -> teardown() OK.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        fs.fail_n["nft delete table"] = 1; // teardown's FIRST delete fails, then succeeds
        assert(net.teardown(3));           // bounded retry rides out the transient failure
        assert(!fs.nft_table && net.state().dnat_rules.empty());
    }

    {   // SHARED TEARDOWN: a PERSISTENT failure is reported (false), not hidden.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).ok());
        fs.fail = {"nft delete table"}; // never clears
        assert(!net.teardown(3));          // persistent -> caller exits nonzero + diagnoses
        assert(fs.nft_table && net.state().dnat_rules.size() == 1);
    }

    // --- AP-ENABLED readiness parsing (pure) + the real poll via the seam ----
    assert(ap_event_enabled("wlan0", "wlan0: AP-ENABLED\n"));
    assert(!ap_event_enabled("wlan0", "wlan0: AP-ENABLED")); // partial line (no newline)
    assert(!ap_event_enabled("wlan0", "wlan0: AP-ENABLED\nwlan0: AP-DISABLED\n")); // last wins
    assert(ap_event_enabled("wlan0", "wlan0: AP-DISABLED\nwlan0: AP-ENABLED\n")); // re-enabled
    assert(!ap_event_enabled("wlan0", "something AP-ENABLED here\n")); // not an exact record
    assert(!ap_event_enabled("wlan0", "wlan1: AP-ENABLED\n"));         // other interface
    assert(!ap_event_enabled("wlan0", ""));
    {   // the REAL await loop via the injected seam: a split write that only
        // completes on a later poll is still recognized.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.ap_log_seq = {"wlan0: AP-ENAB", "wlan0: AP-ENABLED\n"}; // partial, then complete
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(net.start(lopts).hostapd); // poll 1 partial, poll 2 completes -> up
    }
    {   // a stale/dead pidfile is never "ready" even with an AP-ENABLED log.
        FakeSys fs;
        fs.pid_alive_val = false;
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        assert(!net.start(lopts).hostapd);
    }

    {   // regdom is GLOBAL: teardown restores the device's ORIGINAL country.
        // --country JP wins the AP's regdom while the device was US, so the
        // restore ("iw reg set US") is distinguishable from the set ("...JP").
        FakeSys fs;
        fs.tools = {"iw", "nft"};
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.output["iw reg get"] = "country US:\n"; // device's original regdom
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        std::string e;
        assert(net.resolve_country("JP", e) && net.state().country == "JP" &&
               net.state().orig_country == "US");
        assert(net.start(lopts).ok());
        assert(fs.ran("iw reg set JP") && !fs.ran("iw reg set US")); // set JP, not yet restored
        assert(net.cleanup());
        assert(fs.ran("iw reg set US")); // ORIGINAL regdom restored on teardown
    }

    {   // unknown original regdom (world): nothing to restore, cleanup still clean.
        FakeSys fs;
        fs.tools = {"iw", "nft"};
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.output["iw reg get"] = "country 00:\n"; // world -> orig unknown
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        std::string e;
        assert(net.resolve_country("DE", e) && net.state().orig_country.empty());
        assert(net.start(lopts).ok());
        assert(net.cleanup());           // no restore attempted, still fully clean
        assert(!fs.ran("iw reg set 00")); // never tries to set the world/unset code
    }

    {   // self-managed radio: resolve succeeds with NO country, start sets no
        // regdom and writes no country_code, yet still comes up. Detection is
        // scoped to THIS iface's wiphy (phy#0 via `iw dev wlan0 info`).
        FakeSys fs;
        fs.tools = {"iw", "nft"};
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.output["iw dev wlan0 info"] = "Interface wlan0\n\twiphy 0\n\ttype managed\n";
        fs.output["iw reg get"] = "global\ncountry 00: DFS-UNSET\nphy#0 (self-managed)\n";
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        std::string e;
        assert(net.resolve_country("", e));
        assert(net.state().reg_self_managed && net.state().country.empty());
        const StartResult r = net.start(lopts);
        assert(r.ok() && r.regdom);         // regdom "ok" = device-managed, nothing to set
        assert(!fs.ran("iw reg set"));      // we never touch a self-managed regdom
    }

    {   // SCOPED self-managed: wlan0 is phy#0 (ordinary); a SECOND radio (phy#1)
        // is self-managed. The stray marker must NOT make us skip country for
        // wlan0 - it resolves non-self-managed and gets its US regdom set.
        FakeSys fs;
        fs.tools = {"iw", "nft"};
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.output["iw dev wlan0 info"] = "Interface wlan0\n\twiphy 0\n\ttype managed\n";
        fs.output["iw reg get"] = "global\ncountry US: DFS-FCC\n\n"
                                  "phy#1 (self-managed)\ncountry 00: DFS-UNSET\n\n"
                                  "phy#0\ncountry US: DFS-FCC\n";
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        std::string e;
        assert(net.resolve_country("", e));
        assert(!net.state().reg_self_managed && net.state().country == "US");
        assert(net.start(lopts).ok());
        assert(fs.ran("iw reg set US")); // country IS set for the non-self-managed target
    }

    {   // FULL release: exact command order; it SUCCEEDS only on a successful,
        // explicit "Not connected." post-condition. A failed managed-no /
        // link-down, a failed/empty/unknown query, or a still-associated link
        // aborts (so serve_portal routes to cleanup).
        FakeSys fs;
        fs.tools = {"nmcli", "iw"};
        fs.output["iw dev wlan0 link"] = "Not connected.\n"; // explicit disconnected
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        assert(net.release_interface()); // disconnect (best-effort) + managed no + link down
        assert(fs.ran("nmcli --wait 15 device disconnect wlan0"));
        assert(fs.ran("nmcli device set wlan0 managed no"));
        assert(fs.ran("ip link set dev wlan0 down"));

        FakeSys fm; // managed-no fails -> release aborts
        fm.tools = {"nmcli", "iw"};
        fm.fail = {"managed no"};
        PortalNet nm(fm.env());
        nm.set_interface("wlan0", "192.168.4.1");
        assert(!nm.release_interface());

        FakeSys fl; // link-down fails -> release aborts
        fl.tools = {"nmcli", "iw"};
        fl.fail = {"ip link set dev wlan0 down"};
        PortalNet nl(fl.env());
        nl.set_interface("wlan0", "192.168.4.1");
        assert(!nl.release_interface());

        FakeSys fc; // still associated after the sequence -> abort
        fc.tools = {"nmcli", "iw"};
        fc.output["iw dev wlan0 link"] = "Connected to aa:bb:cc:dd:ee:ff (on wlan0)\n";
        PortalNet nc(fc.env());
        nc.set_interface("wlan0", "192.168.4.1");
        assert(!nc.release_interface());

        FakeSys fq; // the link QUERY itself fails -> unknown, NOT disconnected
        fq.tools = {"nmcli", "iw"};
        fq.fail = {"iw dev wlan0 link"};
        PortalNet nq(fq.env());
        nq.set_interface("wlan0", "192.168.4.1");
        assert(!nq.release_interface());

        FakeSys fe; // successful query but EMPTY output -> not recognized disconnected
        fe.tools = {"nmcli", "iw"};
        fe.output["iw dev wlan0 link"] = ""; // status 0, no "Not connected"
        PortalNet ne(fe.env());
        ne.set_interface("wlan0", "192.168.4.1");
        assert(!ne.release_interface());

        FakeSys fd; // disconnect FAILS, but the post-condition confirms disconnected
        fd.tools = {"nmcli", "iw"};
        fd.fail = {"device disconnect"};           // best-effort disconnect returns nonzero
        fd.output["iw dev wlan0 link"] = "Not connected.\n";
        PortalNet nd(fd.env());
        nd.set_interface("wlan0", "192.168.4.1");
        assert(nd.release_interface());            // still OK: confirmed not connected
    }

    {   // readiness STABILITY: AP-ENABLED then AP-DISABLED within the window is
        // NOT a false positive (enabled on the first read, dropped on a later one).
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.ap_log_seq = {"wlan0: AP-ENABLED\n",                        // phase 1: up
                         "wlan0: AP-ENABLED\nwlan0: AP-DISABLED\n"};   // phase 2: dropped
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        const StartResult r = net.start(lopts);
        assert(!r.hostapd);      // not stable -> not up
        assert(r.ap_event_seen); // ...but AP-ENABLED DID appear (existence, for diag)
    }

    {   // P2 REGRESSION: the log already shows AP-ENABLED then AP-DISABLED at the
        // FIRST read. Readiness (last state) is false, but the diagnostic must
        // still record that AP-ENABLED existed (enabled-then-dropped, not
        // never-enabled) and that the pid was alive.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.ap_log = "wlan0: AP-ENABLED\nwlan0: AP-DISABLED\n"; // constant: last state disabled
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        const StartResult r = net.start(lopts);
        assert(!r.hostapd);                        // last state is disabled -> not up
        assert(r.ap_event_seen && r.pid_ever_alive); // diagnostics distinguish the cause
    }

    {   // never enabled + dead pid: diagnostics show BOTH were false (vs the case
        // above), so an on-device [diag] line tells the two apart.
        FakeSys fs;
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.ap_log = "wlan0: interface state UNINITIALIZED->COUNTRY_UPDATE\n"; // never AP-ENABLED
        fs.pid_alive_val = false;                                            // launch failed
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        prepare(fs, net);
        const StartResult r = net.start(lopts);
        assert(!r.hostapd && !r.ap_event_seen && !r.pid_ever_alive);
    }

    {   // capture_ownership records the DEVICE autoconnect (GENERAL.AUTOCONNECT),
        // restored on teardown because `nmcli device disconnect` inhibits it.
        // Distinct output keys so the autoconnect read and the main managed read
        // don't collide on the "device show" substring.
        FakeSys fs;
        fs.tools = {"nmcli"};
        fs.output["GENERAL.NM-MANAGED,GENERAL.CON-UUID"] =
            "GENERAL.NM-MANAGED:yes\nGENERAL.CON-UUID:11111111-2222-3333-4444-555555555555\n";
        fs.output["GENERAL.AUTOCONNECT"] = "yes\n";
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        assert(net.capture_ownership());
        assert(net.state().autoconnect == "yes");
        assert(fs.ran("-g GENERAL.AUTOCONNECT device show wlan0")); // the DEVICE property

        // Device autoconnect is captured even with NO active connection ("--").
        FakeSys fs2;
        fs2.tools = {"nmcli"};
        fs2.output["GENERAL.NM-MANAGED,GENERAL.CON-UUID"] =
            "GENERAL.NM-MANAGED:yes\nGENERAL.CON-UUID:--\n";
        fs2.output["GENERAL.AUTOCONNECT"] = "no\n"; // originally disabled
        PortalNet net2(fs2.env());
        net2.set_interface("wlan0", "192.168.4.1");
        assert(net2.capture_ownership());
        assert(net2.state().active_uuid.empty() && net2.state().autoconnect == "no");

        // For a MANAGED iface the device-autoconnect read must succeed AND be
        // strictly yes/no: a failed, empty, or garbled read fails ownership
        // capture BEFORE any NIC mutation (so no release command is ever run).
        auto no_mutation = [](const FakeSys& f) {
            for (const auto& c : f.log) {
                assert(c.find("managed no") == std::string::npos);
                assert(c.find("link set") == std::string::npos);
                assert(c.find("ip addr") == std::string::npos);
                assert(c.find("device set") == std::string::npos);
            }
        };
        const char* bad_ac[] = {"", "maybe\n", "yes\nno\n"}; // empty / invalid / ambiguous
        for (const char* bad : bad_ac) {
            FakeSys f;
            f.tools = {"nmcli"};
            f.output["GENERAL.NM-MANAGED,GENERAL.CON-UUID"] =
                "GENERAL.NM-MANAGED:yes\nGENERAL.CON-UUID:--\n";
            f.output["GENERAL.AUTOCONNECT"] = bad;
            PortalNet n(f.env());
            n.set_interface("wlan0", "192.168.4.1");
            assert(!n.capture_ownership() && !n.state().ownership_read_ok);
            no_mutation(f);
        }
        {   // autoconnect read command FAILS (nonzero) -> same: capture fails.
            FakeSys f;
            f.tools = {"nmcli"};
            f.output["GENERAL.NM-MANAGED,GENERAL.CON-UUID"] =
                "GENERAL.NM-MANAGED:yes\nGENERAL.CON-UUID:--\n";
            f.fail = {"GENERAL.AUTOCONNECT"};
            PortalNet n(f.env());
            n.set_interface("wlan0", "192.168.4.1");
            assert(!n.capture_ownership() && !n.state().ownership_read_ok);
            no_mutation(f);
        }
        {   // UNMANAGED iface: a missing autoconnect is irrelevant (never handed
            // back to NM), so ownership capture still succeeds.
            FakeSys f;
            f.tools = {"nmcli"};
            f.output["GENERAL.NM-MANAGED,GENERAL.CON-UUID"] =
                "GENERAL.NM-MANAGED:no\nGENERAL.CON-UUID:--\n";
            f.fail = {"GENERAL.AUTOCONNECT"};
            PortalNet n(f.env());
            n.set_interface("wlan0", "192.168.4.1");
            assert(n.capture_ownership() && !n.state().iface_was_managed);
        }
    }

    {   // P2 (autoconnect): teardown must land the DEVICE autoconnect back on
        // its captured value even though `connection up` re-enables it. Model
        // that side effect and check the final device state after cleanup().
        FakeSys fs;
        fs.tools = {"nmcli"};
        fs.output["GENERAL.NM-MANAGED,GENERAL.CON-UUID"] =
            "GENERAL.NM-MANAGED:yes\nGENERAL.CON-UUID:11111111-2222-3333-4444-555555555555\n";
        fs.output["GENERAL.AUTOCONNECT"] = "no\n"; // originally disabled + active UUID
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        assert(net.capture_ownership());
        assert(net.state().autoconnect == "no" && !net.state().active_uuid.empty());
        fs.dev_autoconnect = "no"; // real starting device state
        assert(net.cleanup());
        assert(fs.ran("connection up uuid"));   // we did reactivate (which flips it)
        assert(fs.dev_autoconnect == "no");     // ...but the post-up re-apply won

        // Failure of that device-autoconnect restore is a cleanup failure.
        FakeSys fs2;
        fs2.tools = {"nmcli"};
        fs2.output["GENERAL.NM-MANAGED,GENERAL.CON-UUID"] =
            "GENERAL.NM-MANAGED:yes\nGENERAL.CON-UUID:11111111-2222-3333-4444-555555555555\n";
        fs2.output["GENERAL.AUTOCONNECT"] = "no\n";
        PortalNet net2(fs2.env());
        net2.set_interface("wlan0", "192.168.4.1");
        assert(net2.capture_ownership());
        fs2.fail = {"device set wlan0 autoconnect no"}; // the restore never takes
        assert(!net2.cleanup());
    }

    {   // capture_ownership parses nmcli; release honors managed state.
        FakeSys fs;
        fs.tools = {"nmcli", "nft"};
        fs.output["GENERAL.NM-MANAGED,GENERAL.CON-UUID"] =
            "GENERAL.NM-MANAGED:yes\nGENERAL.CON-UUID:11111111-2222-3333-4444-555555555555\n";
        fs.output["GENERAL.AUTOCONNECT"] = "yes\n";
        fs.output["iw dev wlan0 link"] = "Not connected.\n";
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        assert(net.capture_ownership());
        assert(net.state().iface_was_managed);
        assert(net.state().active_uuid == "11111111-2222-3333-4444-555555555555");
        assert(net.release_interface()); // managed -> issues `managed no` (status 0)
        assert(fs.ran("nmcli device set wlan0 managed no"));

        // Valid "no active connection": managed, but no connection to bring up.
        FakeSys fsn;
        fsn.tools = {"nmcli"};
        fsn.output["device show"] = "GENERAL.NM-MANAGED:no\nGENERAL.CON-UUID:--\n";
        PortalNet netn(fsn.env());
        netn.set_interface("wlan0", "192.168.4.1");
        assert(netn.capture_ownership());
        assert(!netn.state().iface_was_managed && netn.state().active_uuid.empty());

        // Garbled NM-MANAGED (empty) => invalid read, not "unmanaged".
        FakeSys fsg;
        fsg.tools = {"nmcli"};
        fsg.output["device show"] = "GENERAL.NM-MANAGED:\nGENERAL.CON-UUID:--\n";
        PortalNet netg(fsg.env());
        netg.set_interface("wlan0", "192.168.4.1");
        assert(!netg.capture_ownership() && !netg.state().ownership_read_ok);

        // Missing CON-UUID line => incomplete read.
        FakeSys fsm;
        fsm.tools = {"nmcli"};
        fsm.output["device show"] = "GENERAL.NM-MANAGED:yes\n";
        PortalNet netm(fsm.env());
        netm.set_interface("wlan0", "192.168.4.1");
        assert(!netm.capture_ownership());

        // Malformed-present CON-UUID (not "--", not a valid UUID) => garbled
        // snapshot, rejected (accept only "--" or a valid UUID).
        FakeSys fsb;
        fsb.tools = {"nmcli"};
        fsb.output["device show"] = "GENERAL.NM-MANAGED:yes\nGENERAL.CON-UUID:garbled\n";
        PortalNet netb(fsb.env());
        netb.set_interface("wlan0", "192.168.4.1");
        assert(!netb.capture_ownership() && !netb.state().ownership_read_ok);

        // A failed read must NOT be taken as "unmanaged".
        FakeSys fs2;
        fs2.tools = {"nmcli"};
        fs2.fail = {"device show"}; // read fails
        PortalNet net2(fs2.env());
        net2.set_interface("wlan0", "192.168.4.1");
        assert(!net2.capture_ownership());
        assert(!net2.state().ownership_read_ok);

        // Release failure is surfaced (NM may still own the NIC).
        FakeSys fsr;
        fsr.tools = {"nmcli"};
        fsr.output["GENERAL.NM-MANAGED,GENERAL.CON-UUID"] =
            "GENERAL.NM-MANAGED:yes\nGENERAL.CON-UUID:11111111-2222-3333-4444-555555555555\n";
        fsr.output["GENERAL.AUTOCONNECT"] = "yes\n";
        fsr.fail = {"nmcli device set"};
        PortalNet netr(fsr.env());
        netr.set_interface("wlan0", "192.168.4.1");
        assert(netr.capture_ownership());
        assert(!netr.release_interface());
    }

    // --- country / regdom resolution (precedence + failure) -----------------
    {   // --country flag wins over the device regdom.
        FakeSys fs;
        fs.tools = {"iw"};
        fs.output["iw reg get"] = "country JP:\n";
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        std::string err;
        assert(net.resolve_country("us", err)); // lowercase flag accepted, uppercased
        assert(net.state().country == "US");

        // No flag -> read the device regdom.
        FakeSys fs2;
        fs2.tools = {"iw"};
        fs2.output["iw reg get"] = "country JP:\n";
        PortalNet net2(fs2.env());
        net2.set_interface("wlan0", "192.168.4.1");
        assert(net2.resolve_country("", err) && net2.state().country == "JP");

        // Unknown regdom (world) and no flag -> fail (and nothing to start on).
        FakeSys fs3;
        fs3.tools = {"iw", "nft"};
        fs3.output["iw reg get"] = "country 00:\n";
        PortalNet net3(fs3.env());
        net3.set_interface("wlan0", "192.168.4.1");
        assert(!net3.resolve_country("", err) && net3.state().country.empty());
        assert(!fs3.ran("hostapd") && !fs3.ran("nft -f")); // caller never starts

        // No `iw` at all -> fail with a clear message (needed to read AND set).
        FakeSys fs4;
        fs4.tools = {"nft"};
        PortalNet net4(fs4.env());
        net4.set_interface("wlan0", "192.168.4.1");
        assert(!net4.resolve_country("US", err));

        // Invalid --country rejected.
        FakeSys fs5;
        fs5.tools = {"iw"};
        PortalNet net5(fs5.env());
        net5.set_interface("wlan0", "192.168.4.1");
        assert(!net5.resolve_country("USA", err));
    }

    {   // start() issues `iw reg set` BEFORE hostapd, and the conf carries the CC.
        FakeSys fs;
        fs.tools = {"iw", "nft"};
        fs.present = {"hostapd.pid", "dnsmasq.pid"};
        fs.output["iw reg get"] = "country JP:\n";
        PortalNet net(fs.env());
        net.set_interface("wlan0", "192.168.4.1");
        std::string err;
        assert(net.resolve_country("", err)); // -> JP
        PortalOptions o = lopts;
        o.country.clear(); // force use of the resolved net_ country (JP)
        assert(net.start(o).ok());
        // regdom set precedes the hostapd launch in the command log.
        int i_reg = -1, i_hostapd = -1;
        for (size_t k = 0; k < fs.log.size(); ++k) {
            if (i_reg < 0 && fs.log[k].find("iw reg set JP") != std::string::npos) i_reg = (int)k;
            if (i_hostapd < 0 && fs.log[k].find("hostapd -B") != std::string::npos)
                i_hostapd = (int)k;
        }
        assert(i_reg >= 0 && i_hostapd >= 0 && i_reg < i_hostapd);
    }

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

    // --- --pwd-mode: Bruce password-masking modes --------------------------
    check_pwd_mode_capture_masking(home);
    check_pwd_mode_verify_keeps_raw_password(home);
    check_pwd_mode_cli_parse();
    check_pwd_mode_cli_error();
    check_pwd_mode_labels();

    // --- Wi-Fi menu: Evil Portal rows + clone-AP drill-down -----------------
    std::system("rm -rf /tmp/bruce-portal");
    {
        const auto& wifi = wifi_category();
        const size_t static_count = wifi.actions.size() - 2; // scan + "(no APs yet)"
        // The catalog builds the scan command from wifi_interface() (real NIC;
        // "wlan0" on the device, first phy80211 NIC otherwise) - compute the
        // expected interface the same way instead of hardcoding "wlan0", so
        // this passes on hosts whose Wi-Fi NIC is not wlan0.
        const std::string scan_cmd = "iw dev " + model::wifi_interface() + " scan";
        // The static fixes are present.
        bool have_scan_root = false, have_driver = false, have_portal = false;
        for (const auto& a : wifi.actions) {
            const std::string sh = a.argv.size() >= 3 && a.argv[0] == "sh" ? a.argv[2] : "";
            if (std::string(a.label) == "Scan nearby networks") {
                have_scan_root = a.needs_root && sh.find(scan_cmd) != std::string::npos;
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
                assert(launched[4].find(scan_cmd) != std::string::npos);
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
    // Back key: on the AP list there is nothing to undo (leave Wi-Fi); in the
    // clone view it acts like the "< Back to AP list" row.
    assert(!model::on_category_back(wifi_idx));
    model::on_navigation_action(wifi_idx, static_cast<int>(wifi_static) + 1);
    assert(wifi_category().actions.size() == wifi_static + 2);   // clone view
    assert(model::on_category_back(wifi_idx));
    assert(wifi_category().actions.size() == wifi_static + 3);   // AP list again
    assert(!model::on_category_back(wifi_idx));

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
