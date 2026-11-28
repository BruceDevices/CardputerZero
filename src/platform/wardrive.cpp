/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Port of Bruce firmware 1.17 src/modules/gps/wardriving.cpp, function by
 * function; the Linux deviations are commented where they occur. Firmware
 * references are given as wardriving.cpp:<line>.
 */

#include "wardrive.h"

#include "gps_port.h"
#include "../model/id_parse.h"
#include "gps_session.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if defined(__linux__)
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <grp.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace platform {
namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// snprintf into a string of the needed length. Bruce formats rows into a
// fixed char[512]; with '"' doubled a 248-byte BLE name could overflow that,
// so the row would be cut and the next one glued onto it.
std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string out;
    if (n > 0) {
        out.resize(static_cast<size_t>(n) + 1);
        std::vsnprintf(&out[0], out.size(), fmt, args);
        out.resize(static_cast<size_t>(n));
    }
    va_end(args);
    return out;
}

std::string chomp(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Whitespace-separated suite tokens of an "Authentication suites:" value.
// iw names them PSK, SAE, OWE, "IEEE 802.1X", FT/PSK, PSK/SHA-256, FT/SAE,
// SAE-EXT-KEY, FILS/SHA-256, ... (iw scan.c print_auth()).
bool suites_have(const std::string& suites, const char* what) {
    std::istringstream in(suites);
    std::string tok;
    while (in >> tok) {
        if (tok.find(what) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool suites_enterprise(const std::string& suites) {
    return suites.find("802.1X") != std::string::npos || suites.find("FILS") != std::string::npos;
}

} // namespace

int freq_to_channel(int mhz) {
    if (mhz == 2484) return 14;
    if (mhz >= 2412 && mhz <= 2472) return (mhz - 2407) / 5; // 2.4GHz ch 1-13
    if (mhz >= 5160 && mhz <= 5885) return (mhz - 5000) / 5; // 5GHz
    if (mhz >= 5955 && mhz <= 7115) return (mhz - 5950) / 5; // 6GHz
    return 0;
}

// The ESP32 driver's wifi_auth_mode_t, as Bruce names it (wardriving.cpp:203-216):
// 802.1X -> WPA2_ENTERPRISE, SAE(+PSK) -> WPA3_PSK / WPA2_WPA3_PSK, RSN+WPA
// PSK -> WPA_WPA2_PSK, Privacy without RSN/WPA -> WEP. Suites Bruce has no
// name for (OWE, ...) are "UNKNOWN", as the firmware's default case.
std::string bruce_auth_mode(bool privacy, bool has_rsn, const std::string& rsn_suites, bool has_wpa,
                            const std::string& wpa_suites) {
    if (!has_rsn && !has_wpa) {
        return privacy ? "WEP" : "OPEN";
    }
    if ((has_rsn && suites_enterprise(rsn_suites)) || (has_wpa && suites_enterprise(wpa_suites))) {
        return "WPA2_ENTERPRISE";
    }
    const bool rsn_sae = has_rsn && suites_have(rsn_suites, "SAE");
    const bool rsn_psk = has_rsn && suites_have(rsn_suites, "PSK");
    const bool wpa_psk = has_wpa && suites_have(wpa_suites, "PSK");
    if (rsn_sae) {
        return rsn_psk ? "WPA2_WPA3_PSK" : "WPA3_PSK";
    }
    if (rsn_psk && wpa_psk) {
        return "WPA_WPA2_PSK";
    }
    if (rsn_psk) {
        return "WPA2_PSK";
    }
    if (wpa_psk) {
        return "WPA_PSK";
    }
    return "UNKNOWN";
}

namespace {
// Length of the well-formed UTF-8 sequence at s[i] (RFC 3629: no overlongs,
// no surrogates, <= U+10FFFF), or 0 when the bytes there are not one.
size_t utf8_len(const std::string& s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    size_t n;
    unsigned char lo = 0x80, hi = 0xbf; // allowed range of the 2nd byte
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) n = 2;
    else if (c >= 0xe0 && c <= 0xef) {
        n = 3;
        if (c == 0xe0) lo = 0xa0;
        if (c == 0xed) hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        n = 4;
        if (c == 0xf0) lo = 0x90;
        if (c == 0xf4) hi = 0x8f;
    } else {
        return 0;
    }
    if (i + n > s.size()) return 0;
    if (b(i + 1) < lo || b(i + 1) > hi) return 0;
    for (size_t k = 2; k < n; ++k) {
        if (b(i + k) < 0x80 || b(i + k) > 0xbf) return 0;
    }
    return n;
}
} // namespace

std::string decode_iw_ssid(const std::string& escaped) {
    // 1. iw's \xHH escapes back to the raw SSID bytes.
    std::string raw;
    bool all_nul = !escaped.empty();
    for (size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] == '\\' && i + 3 < escaped.size() && escaped[i + 1] == 'x' &&
            hexval(escaped[i + 2]) >= 0 && hexval(escaped[i + 3]) >= 0) {
            const int v = hexval(escaped[i + 2]) * 16 + hexval(escaped[i + 3]);
            all_nul = all_nul && v == 0;
            raw.push_back(static_cast<char>(v));
            i += 3;
            continue;
        }
        all_nul = false;
        raw.push_back(escaped[i]);
    }
    if (all_nul) {
        return {}; // hidden network
    }
    // 2. Keep well-formed UTF-8; control bytes and bytes that are not valid
    //    UTF-8 (an SSID is any 32 bytes) stay as printable \xHH escapes.
    std::string out;
    for (size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        const size_t n = (c < 0x20 || c == 0x7f) ? 0 : utf8_len(raw, i);
        if (n == 0) {
            char e[5];
            std::snprintf(e, sizeof(e), "\\x%02x", c);
            out += e;
            ++i;
        } else {
            out.append(raw, i, n);
            i += n;
        }
    }
    return out;
}

std::vector<ScanAp> parse_iw_scan(const std::string& text) {
    std::vector<ScanAp> aps;
    ScanAp cur;
    bool have = false, privacy = false, rsn = false, wpa = false;
    std::string rsn_suites, wpa_suites;
    enum class Elem { None, Rsn, Wpa } elem = Elem::None;

    auto flush = [&]() {
        if (have) {
            cur.auth = bruce_auth_mode(privacy, rsn, rsn_suites, wpa, wpa_suites);
            aps.push_back(cur);
        }
    };

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (starts_with(line, "BSS ")) {
            flush();
            cur = ScanAp{};
            privacy = rsn = wpa = false;
            rsn_suites.clear();
            wpa_suites.clear();
            elem = Elem::None;
            have = true;
            // "BSS aa:bb:cc:dd:ee:ff(on wlan0) -- associated" -> the MAC token.
            std::string mac = line.substr(4);
            const size_t paren = mac.find('(');
            if (paren != std::string::npos) mac = mac.substr(0, paren);
            cur.bssid = trim(mac);
            continue;
        }
        if (!have) {
            continue;
        }
        // A top-level element starts with exactly one tab; its sub-items
        // ("\t\t * ...") follow with deeper indentation.
        const bool top = line.size() > 1 && line[0] == '\t' && line[1] != '\t';
        const std::string t = trim(line);
        if (top) {
            elem = Elem::None;
            if (starts_with(t, "freq:")) {
                cur.freq = std::atoi(t.c_str() + 5); // "2437" or "2437.0"
                cur.channel = freq_to_channel(cur.freq);
            } else if (starts_with(t, "signal:")) {
                cur.signal = static_cast<int>(std::atof(t.c_str() + 7)); // "-45.00 dBm"
            } else if (starts_with(line, "\tSSID: ") || t == "SSID:") {
                cur.ssid = decode_iw_ssid(line.size() > 7 ? line.substr(7) : std::string());
            } else if (starts_with(t, "capability:")) {
                privacy = t.find("Privacy") != std::string::npos;
            } else if (starts_with(t, "RSN:")) {
                rsn = true;
                elem = Elem::Rsn;
            } else if (starts_with(t, "WPA:")) {
                wpa = true;
                elem = Elem::Wpa;
            }
        }
        const size_t suites = t.find("Authentication suites:");
        if (suites != std::string::npos && elem != Elem::None) {
            const std::string v = trim(t.substr(suites + 22));
            (elem == Elem::Rsn ? rsn_suites : wpa_suites) += " " + v;
        }
    }
    flush();
    return aps;
}

namespace wardrive {

std::string wigle_header(const std::string& app_version) {
    // wardriving.cpp:243-251. Deviation: the identity fields describe this
    // device instead of "M5Stack GPS Unit" / "ESP32 M5Stack" / "SPI TFT".
    return "WigleWifi-1.6,appRelease=v" + app_version + ",model=CardputerZero,release=v" + app_version +
           ",device=CardputerZero,display=ST7789 LCD,board=CardputerZero (CM0),brand=Bruce,star=Sol,body=4,"
           "subBody=1\r\n"
           "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,"
           "AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\r\n";
}

std::string csv_quoted(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') {
            out += "\"\"";
        } else if (c == '\n' || c == '\r') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

std::string wifi_row(const ScanAp& ap, const Fix& f) {
    // wardriving.cpp:281. Deviation: Frequency is iw's real MHz, not the
    // 2.4 GHz-only `channel != 14 ? 2407 + channel * 5 : 2484`.
    return format("%s,\"%s\",[%s],%04u-%02u-%02u %02u:%02u:%02u,%ld,%ld,%ld,%f,%f,%f,%f,,,WIFI\n",
                  upper(ap.bssid).c_str(), csv_quoted(ap.ssid).c_str(), ap.auth.c_str(), f.year, f.month, f.day,
                  f.hour, f.minute, f.second, static_cast<long>(ap.channel), static_cast<long>(ap.freq),
                  static_cast<long>(ap.signal), f.lat, f.lng, f.alt, f.hdop * 1.0);
}

std::string ble_row(const ble::Device& dev, const Fix& f) {
    // wardriving.cpp:384-391
    char mfgr[8] = "";
    if (dev.mfgr_id != 0) {
        std::snprintf(mfgr, sizeof(mfgr), "%04X", dev.mfgr_id);
    }
    return format("%s,\"%s\",Misc [BLE],%04u-%02u-%02u %02u:%02u:%02u,0,,%d,%f,%f,%f,%f,,%s,BLE\n",
                  lower(dev.mac).c_str(), csv_quoted(dev.name).c_str(), f.year, f.month, f.day, f.hour, f.minute,
                  f.second, dev.rssi, f.lat, f.lng, f.alt, f.hdop * 1.0, mfgr);
}

std::string wardriving_filename(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute,
                                unsigned second) {
    return format("%02u%02u%02u_%02u%02u%02u", year % 100, month % 100, day % 100, hour % 100, minute % 100,
                  second % 100) +
           "_wardriving.csv";
}

bool parse_mac_u64(const std::string& mac, uint64_t& out) {
    uint64_t value = 0;
    int nibbles = 0;
    for (char c : mac) {
        if (c == ':' || c == '-') continue;
        const int v = hexval(c);
        if (v < 0) return false;
        value = (value << 4) | static_cast<uint64_t>(v);
        if (++nibbles > 12) return false;
    }
    if (nibbles != 12) return false;
    out = value;
    return true;
}

std::set<std::string> parse_alert_macs(const std::string& text) {
    std::set<std::string> macs;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) { // readStringUntil('\n')
        line = trim(line);
        if (!line.empty() && line[0] != '#') {
            macs.insert(lower(line));
        }
    }
    return macs;
}

std::string alert_sample() {
    return "# Alert MAC addresses - one per line\r\n"
           "# Lines starting with # are comments\r\n"
           "# Example:\r\n"
           "# aa:bb:cc:dd:ee:ff\r\n";
}

// --- protocol -------------------------------------------------------------------

namespace {
std::string one_line(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20) {
            c = ' ';
        }
    }
    return out;
}
} // namespace

std::string encode_frame(const gps::ScreenFrame& frame, bool popup_waits, bool gps_connected) {
    std::string s = "@F " + one_line(frame.title) + "\n";
    for (const auto& l : frame.lines) {
        s += (l.padx >= 2 ? "@2 " : "@1 ") + one_line(l.text) + "\n";
    }
    if (frame.popup != gps::PopupKind::None) {
        const char* k = frame.popup == gps::PopupKind::Message ? "M" : (popup_waits ? "W" : "E");
        s += std::string("@P ") + k + " " + one_line(frame.popup_text) + "\n";
    }
    s += gps_connected ? "@G 1\n" : "@G 0\n";
    s += "@.\n";
    return s;
}

bool FrameDecoder::feed(const std::string& text) {
    partial_ += text;
    bool changed = false;
    size_t pos;
    while ((pos = partial_.find('\n')) != std::string::npos) {
        std::string l = partial_.substr(0, pos);
        partial_.erase(0, pos + 1);
        if (!l.empty() && l.back() == '\r') {
            l.pop_back();
        }
        const uint32_t before = frame_.version;
        const bool was_ended = ended();
        line(l);
        changed = changed || frame_.version != before || ended() != was_ended;
    }
    return changed;
}

void FrameDecoder::line(const std::string& l) {
    auto rest = [&](size_t n) { return l.size() > n ? l.substr(n) : std::string(); };
    if (starts_with(l, "@F")) {
        building_ = gps::ScreenFrame{};
        building_.title = rest(3);
        building_waits_ = false;
        in_frame_ = true;
    } else if (in_frame_ && (starts_with(l, "@1") || starts_with(l, "@2"))) {
        building_.lines.push_back(gps::ScreenLine{l[1] == '2' ? 2 : 1, rest(3)});
    } else if (in_frame_ && starts_with(l, "@P ") && l.size() >= 4) {
        building_.popup = l[3] == 'M' ? gps::PopupKind::Message : gps::PopupKind::Error;
        building_waits_ = l[3] == 'W';
        building_.popup_text = rest(5);
    } else if (starts_with(l, "@G ")) {
        gps_connected_ = rest(3) == "1";
    } else if (in_frame_ && l == "@.") {
        in_frame_ = false;
        building_.version = frame_.version + 1;
        frame_ = building_;
        popup_waits_ = building_waits_;
    } else if (starts_with(l, "@X")) {
        end_reason_ = rest(3).empty() ? "error" : rest(3);
    } else if (!l.empty()) {
        log_.push_back(l);
        if (log_.size() > 20) {
            log_.erase(log_.begin());
        }
    }
}

bool storage_error_after_back(FrameDecoder& decoder, const std::string& undelivered) {
    decoder.feed(undelivered);
    return decoder.end_reason() == "storage";
}

// --- storage ----------------------------------------------------------------------

bool parse_id(const char* text, uint32_t& out) {
    return model::parse_id(text, out); // src/model/id_parse.h
}

StoragePlan plan_storage(bool is_root, const char* sudo_uid, uid_t home_uid, gid_t home_gid,
                         const std::function<bool(uid_t, gid_t&, std::vector<gid_t>&)>& lookup) {
    StoragePlan plan;
    if (!is_root) {
        plan.ok = true; // the user's own run: its own credentials
        return plan;
    }
    if (sudo_uid) {
        uint32_t id = 0;
        if (!parse_id(sudo_uid, id) || static_cast<uid_t>(id) != home_uid) {
            return plan; // fail closed: an odd $SUDO_UID, or a home that is not the sudo user's
        }
    }
    if (home_uid == 0) {
        plan.ok = true; // root's own home (the app runs as root): no switch
        return plan;
    }
    plan.owner.switch_creds = true;
    plan.owner.uid = home_uid;
    gid_t gid = home_gid;
    std::vector<gid_t> groups;
    if (lookup && lookup(home_uid, gid, groups)) {
        plan.owner.gid = gid;
        plan.owner.groups = groups;
    } else {
        plan.owner.gid = home_gid; // no passwd entry: the home's group only
    }
    if (plan.owner.groups.empty()) {
        plan.owner.groups.push_back(plan.owner.gid);
    }
    plan.ok = true;
    return plan;
}

#if defined(__linux__)
namespace {

// Open <home>/BruceWardriving/<name> relative to the opened home `hfd`,
// creating the folder when missing (Bruce: if (!exists("/BruceWardriving"))
// mkdir(...)). Only system calls: it also runs in the forked helper of a
// multi-threaded process. The folder and the file are never followed when
// they are symlinks; O_NONBLOCK keeps a FIFO from blocking the open. The
// result must be a regular file with one link owned by `expect_uid`; a file
// this call created that fails that check is removed again (a filesystem that
// maps owners, e.g. vfat uid=, would otherwise leave one behind).
// `strict_dirs` (root working in a root-owned home, in-process): the folder
// must also belong to `expect_uid` and not be group/other writable, so no
// other user who can write in that home (/tmp, a root:users 0775 directory)
// can prepare BruceWardriving for root.
bool dir_safe(int fd, uid_t owner) {
    struct stat st;
    return ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == owner &&
           (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

int open_in_home(int hfd, const char* name, int flags, uid_t expect_uid, bool strict_dirs, int* err) {
    if (::mkdirat(hfd, "BruceWardriving", 0755) != 0 && errno != EEXIST) {
        *err = errno;
        return -1;
    }
    const int dfd = ::openat(hfd, "BruceWardriving", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
        *err = errno;
        return -1;
    }
    if (strict_dirs && !dir_safe(dfd, expect_uid)) {
        ::close(dfd);
        *err = EPERM;
        return -1;
    }
    const int fd = ::openat(dfd, name, flags | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC, 0644);
    if (fd < 0) {
        *err = errno;
        ::close(dfd);
        return -1;
    }
    struct stat st;
    int bad = 0;
    if (::fstat(fd, &st) != 0) {
        bad = errno;
    } else if (!S_ISREG(st.st_mode)) {
        bad = EINVAL;
    } else if (st.st_nlink != 1) {
        bad = EMLINK;
    } else if (st.st_uid != expect_uid) {
        bad = EPERM;
    }
    if (bad != 0) {
        if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
            (void)::unlinkat(dfd, name, 0); // ours: created by this very call
        }
        ::close(fd);
        ::close(dfd);
        *err = bad;
        return -1;
    }
    ::close(dfd);
    *err = 0;
    return fd;
}

// The same in a forked helper running with `owner`'s credentials. The helper
// opens the home by path with the user's rights and requires it to be the
// inode Storage::setup() saw (EXDEV otherwise). `name` == nullptr only checks
// that. The descriptor comes back over a socketpair (SCM_RIGHTS). Bounded:
// a helper that does not answer within 5 s is killed, and one that cannot be
// reaped within another 0.5 s (stuck in uninterruptible I/O) is abandoned.
int open_in_home_as(const StorageOwner& owner, const char* home, uint64_t dev, uint64_t ino, const char* name,
                    int flags, uid_t expect_uid, int* err) {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        *err = errno;
        return -1;
    }
    const gid_t* groups = owner.groups.data();
    const size_t ngroups = owner.groups.size();
    const pid_t pid = ::fork();
    if (pid < 0) {
        *err = errno;
        ::close(sv[0]);
        ::close(sv[1]);
        return -1;
    }
    if (pid == 0) {
        // Child: system calls only (the parent has other threads).
        int e = 0;
        int fd = -1;
        uid_t ru, eu, su;
        gid_t rg, eg, sg;
        if (::geteuid() == 0 && ::setgroups(ngroups, groups) != 0) {
            e = errno;
        } else if (::setresgid(owner.gid, owner.gid, owner.gid) != 0 ||
                   ::setresuid(owner.uid, owner.uid, owner.uid) != 0) {
            e = errno;
        } else if (::getresuid(&ru, &eu, &su) != 0 || ::getresgid(&rg, &eg, &sg) != 0 || ru != owner.uid ||
                   eu != owner.uid || su != owner.uid || rg != owner.gid || eg != owner.gid || sg != owner.gid) {
            e = EPERM; // the switch did not take
        } else if (owner.uid != 0 && ::setresuid(0, 0, 0) == 0) {
            e = EPERM; // root could be regained: never open anything then
        } else if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
            // No ptrace / /proc/<pid>/fd access to the root descriptors this
            // process inherited, whatever fs.suid_dumpable says; if that
            // cannot be set, open nothing (like the checks above).
            e = EPERM;
        } else {
            ::umask(022);
            const int hfd = ::open(home, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            struct stat st;
            if (hfd < 0) {
                e = errno;
            } else if (::fstat(hfd, &st) != 0) {
                e = errno;
            } else if (static_cast<uint64_t>(st.st_dev) != dev || static_cast<uint64_t>(st.st_ino) != ino) {
                e = EXDEV; // the home was replaced since setup()
            } else if (name) {
                fd = open_in_home(hfd, name, flags, expect_uid, false, &e);
            }
            if (hfd >= 0) {
                ::close(hfd);
            }
        }
        int payload = (fd >= 0 || (!name && e == 0)) ? 0 : (e != 0 ? e : EIO);
        iovec iov{&payload, sizeof(payload)};
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))] = {};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (fd >= 0) {
            msg.msg_control = ctrl;
            msg.msg_controllen = sizeof(ctrl);
            cmsghdr* c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
        }
        (void)!::sendmsg(sv[1], &msg, MSG_NOSIGNAL);
        _exit(0);
    }
    ::close(sv[1]);
    int fd = -1;
    pollfd p{sv[0], POLLIN, 0};
    int r;
    while ((r = ::poll(&p, 1, 5000)) < 0 && errno == EINTR) {
    }
    if (r <= 0) {
        *err = ETIMEDOUT; // a hung home filesystem must not hang the run
        ::kill(pid, SIGKILL);
    } else {
        int payload = EIO;
        iovec iov{&payload, sizeof(payload)};
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))] = {};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);
        if (::recvmsg(sv[0], &msg, MSG_CMSG_CLOEXEC) == static_cast<ssize_t>(sizeof(payload))) {
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                    std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
                }
            }
        } else {
            payload = EIO;
        }
        *err = payload;
        if (payload != 0 && fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    ::close(sv[0]);
    for (int i = 0; i < 50; ++i) { // reap; give up on an unkillable helper
        const pid_t w = ::waitpid(pid, nullptr, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) {
            break;
        }
        usleep(10000);
    }
    return fd;
}

bool passwd_lookup(uid_t uid, gid_t& gid, std::vector<gid_t>& groups) {
    const passwd* pw = ::getpwuid(uid);
    if (!pw || !pw->pw_name) {
        return false;
    }
    gid = pw->pw_gid;
    int n = 64;
    groups.assign(static_cast<size_t>(n), 0);
    if (::getgrouplist(pw->pw_name, gid, groups.data(), &n) < 0) {
        if (n <= 64) {
            groups.clear();
            return true; // primary group only
        }
        groups.assign(static_cast<size_t>(n), 0);
        if (::getgrouplist(pw->pw_name, gid, groups.data(), &n) < 0) {
            groups.clear(); // changed meanwhile: primary group only
            return true;
        }
    }
    groups.resize(static_cast<size_t>(n));
    return true;
}

} // namespace

// One worker that runs fdatasync() + close() for finished passes.
struct Storage::Sync {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<int, bool>> fds; // fd, is a CSV (fault injection)
    bool busy = false;
    bool quit = false;
    std::string error;
    int delay_ms = 0;    // tests: slow storage
    int fault_errno = 0; // tests: fdatasync() of a CSV reports this errno
    std::thread worker;
};

Storage::Storage(std::string home, StorageOptions options)
    : home_(std::move(home)), options_(options), sync_(std::make_shared<Sync>()) {
    sync_->delay_ms = options_.sync_delay_ms;
    sync_->fault_errno = options_.sync_fault_errno;
}

Storage::~Storage() {
    if (hfd_ >= 0) {
        ::close(hfd_);
    }
    if (!sync_->worker.joinable()) {
        return;
    }
    const bool idle = wait_idle(0);
    {
        std::lock_guard<std::mutex> lock(sync_->m);
        sync_->quit = true;
    }
    sync_->cv.notify_all();
    if (idle) {
        sync_->worker.join();
    } else {
        sync_->worker.detach(); // stuck storage: the worker keeps its own state
    }
}

bool Storage::setup() {
    if (setup_done_) {
        return setup_ok_;
    }
    setup_done_ = true;
    if (home_.empty()) {
        return false;
    }
    // The home, opened with our own credentials; its owner decides who the
    // files are opened as. Any failure is "Storage setup error". (This one
    // open runs without the helper's 5 s bound: a hung network home can still
    // delay the start, as any program opening it would.)
    const int hfd = ::open(home_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (hfd < 0) {
        return false;
    }
    struct stat st;
    if (::fstat(hfd, &st) != 0) {
        ::close(hfd);
        return false;
    }
    home_dev_ = static_cast<uint64_t>(st.st_dev);
    home_ino_ = static_cast<uint64_t>(st.st_ino);
    if (options_.test_switch_to_self) {
        owner_.switch_creds = true;
        owner_.uid = ::getuid();
        owner_.gid = ::getgid();
        owner_.groups = {owner_.gid};
    } else {
        const StoragePlan plan = plan_storage(::geteuid() == 0, options_.sudo_uid, st.st_uid, st.st_gid,
                                              passwd_lookup);
        if (!plan.ok) {
            ::close(hfd);
            return false;
        }
        owner_ = plan.owner;
    }
    // Root working in-process (a root-owned home): the home and the folder
    // must be root's and not writable by group/other (see open_in_home()).
    strict_dirs_ = (!owner_.switch_creds && ::geteuid() == 0) || options_.test_strict_dirs;
    if (strict_dirs_ && !dir_safe(hfd, owner_.switch_creds ? owner_.uid : ::geteuid())) {
        ::close(hfd);
        last_errno_ = EPERM;
        return false;
    }
    if (owner_.switch_creds) {
        ::close(hfd);
        // The user must reach the same home with their own rights.
        int err = 0;
        open_in_home_as(owner_, home_.c_str(), home_dev_, home_ino_, nullptr, 0, owner_.uid, &err);
        if (err != 0) {
            last_errno_ = err;
            return false;
        }
    } else {
        hfd_ = hfd;
    }
    setup_ok_ = true;
    return true;
}

int Storage::open_file(const std::string& name, int flags, int& err) {
    err = 0;
    if (!setup()) {
        err = ENOENT;
        last_errno_ = err;
        return -1;
    }
    uid_t expect = owner_.switch_creds ? owner_.uid : ::geteuid();
    if (options_.test_expect_other_owner) {
        expect += 1;
    }
    int fd;
    if (owner_.switch_creds) {
        fd = open_in_home_as(owner_, home_.c_str(), home_dev_, home_ino_, name.c_str(), flags, expect, &err);
    } else {
        fd = open_in_home(hfd_, name.c_str(), flags, expect, strict_dirs_, &err);
    }
    if (fd < 0) {
        last_errno_ = err;
        return -1;
    }
    // Defence in depth: re-check what came back.
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != expect) {
        ::close(fd);
        err = EPERM;
        last_errno_ = err;
        return -1;
    }
    const int fl = ::fcntl(fd, F_GETFL);
    if (fl >= 0) {
        (void)::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }
    return fd;
}

bool Storage::read(const std::string& name, std::string& text, int* err) {
    int e = 0;
    const int fd = open_file(name, O_RDONLY, e);
    if (fd < 0) {
        if (err) *err = e;
        return false;
    }
    // Bounded: alert.txt is the user's file, read by root.
    char buf[4096];
    ssize_t n = 0;
    bool more = false;
    while (true) {
        n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        const size_t room = kMaxRead - text.size();
        text.append(buf, std::min(room, static_cast<size_t>(n)));
        if (static_cast<size_t>(n) > room || text.size() >= kMaxRead) {
            more = true;
            break;
        }
    }
    if (more) {
        const size_t nl = text.rfind('\n');
        text.resize(nl == std::string::npos ? 0 : nl + 1);
        n = 0;
    }
    if (err) *err = n == 0 ? 0 : errno;
    ::close(fd);
    return n == 0;
}

bool Storage::create(const std::string& name, const std::string& text) {
    int e = 0;
    const int fd = open_file(name, O_WRONLY | O_CREAT | O_EXCL, e);
    if (fd < 0) {
        return false;
    }
    const bool ok = write(fd, text);
    ::close(fd); // the 4-line alert.txt sample: closed at once, not a CSV
    return ok;
}

int Storage::open(const std::string& name, bool& is_new) {
    int e = 0;
    int fd = open_file(name, O_WRONLY | O_CREAT | O_EXCL, e);
    is_new = fd >= 0;
    if (fd < 0 && e == EEXIST) {
        fd = open_file(name, O_WRONLY | O_APPEND, e);
    }
    return fd;
}

bool Storage::write(int handle, const std::string& data) {
    // A write past RLIMIT_FSIZE raises SIGXFSZ on this thread, whose default
    // action kills the process before the GPS port is released. Blocked for
    // the write (as in GpxWriter), the write fails with EFBIG instead; the
    // pending signal is consumed before the mask is restored.
    sigset_t xfsz, old;
    sigemptyset(&xfsz);
    sigaddset(&xfsz, SIGXFSZ);
    pthread_sigmask(SIG_BLOCK, &xfsz, &old);
    size_t off = 0;
    bool ok = true;
    int err = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(handle, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            err = n < 0 ? errno : EIO;
            ok = false;
            break;
        }
        off += static_cast<size_t>(n);
    }
    const timespec zero{};
    while (::sigtimedwait(&xfsz, nullptr, &zero) == SIGXFSZ) {
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    errno = err;
    return ok;
}

void Storage::close(int handle) {
    if (handle < 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(sync_->m);
    sync_->fds.emplace_back(handle, true);
    if (!sync_->worker.joinable()) {
        auto st = sync_;
        sync_->worker = std::thread([st]() {
            std::unique_lock<std::mutex> lk(st->m);
            while (true) {
                st->cv.wait(lk, [&] { return st->quit || !st->fds.empty(); });
                if (st->fds.empty()) {
                    return; // quit
                }
                const int fd = st->fds.front().first;
                const bool csv = st->fds.front().second;
                st->fds.pop_front();
                st->busy = true;
                lk.unlock();
                if (st->delay_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(st->delay_ms));
                }
                std::string err;
                if (::fdatasync(fd) != 0) {
                    err = std::string("sync: ") + std::strerror(errno);
                } else if (csv && st->fault_errno != 0) {
                    err = std::string("sync: ") + std::strerror(st->fault_errno);
                }
                if (::close(fd) != 0 && err.empty()) {
                    err = std::string("close: ") + std::strerror(errno);
                }
                lk.lock();
                st->busy = false;
                if (!err.empty() && st->error.empty()) {
                    st->error = err;
                }
                st->cv.notify_all();
            }
        });
    }
    sync_->cv.notify_all();
}

std::string Storage::take_error() {
    std::lock_guard<std::mutex> lock(sync_->m);
    std::string e;
    e.swap(sync_->error);
    return e;
}

bool Storage::wait_idle(int timeout_ms) {
    std::unique_lock<std::mutex> lock(sync_->m);
    return sync_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [&] { return sync_->fds.empty() && !sync_->busy; });
}
#else
struct Storage::Sync {};
Storage::Storage(std::string home, StorageOptions options) : home_(std::move(home)), options_(options) {}
Storage::~Storage() = default;
bool Storage::setup() { return false; }
int Storage::open_file(const std::string&, int, int& err) { err = ENOSYS; return -1; }
bool Storage::read(const std::string&, std::string&, int* err) { if (err) *err = ENOSYS; return false; }
bool Storage::create(const std::string&, const std::string&) { return false; }
int Storage::open(const std::string&, bool& is_new) { is_new = false; return -1; }
bool Storage::write(int, const std::string&) { return false; }
void Storage::close(int) {}
std::string Storage::take_error() { return {}; }
bool Storage::wait_idle(int) { return true; }
#endif

// --- Wardriver ----------------------------------------------------------------

namespace {
constexpr uint32_t kMaxWait = 5000; // wardriving.cpp:18 MAX_WAIT
constexpr int kMaxEmptyPasses = 5;  // loop(): if (count > 5) "GPS not Found!"
} // namespace

const char* end_reason_name(Wardriver::EndReason r) {
    switch (r) {
        case Wardriver::EndReason::Stopped:
            return "stopped";
        case Wardriver::EndReason::NotFound:
            return "notfound";
        case Wardriver::EndReason::OpenFailed:
            return "open";
        case Wardriver::EndReason::StorageError:
            return "storage";
        case Wardriver::EndReason::None:
            break;
    }
    return "error";
}

Wardriver::Wardriver(Options options, Hooks hooks, Storage& storage)
    : options_(std::move(options)), hooks_(std::move(hooks)), storage_(storage) {
    if (options_.current_year <= 0) {
        options_.current_year = gps::build_year();
    }
}

void Wardriver::padprintln(const std::string& text, int padx) {
    frame_.lines.push_back(gps::ScreenLine{padx, text});
    ++frame_.version;
    dirty_ = true;
}

void Wardriver::popup(gps::PopupKind kind, const std::string& text, bool waits) {
    frame_.popup = kind;
    frame_.popup_text = text;
    popup_waits_ = waits;
    ++frame_.version;
    dirty_ = true;
}

// displayError() (core/display.cpp:347-355): the red stripe, "ERR: " on
// Serial and a 200 ms delay; with `wait_key` it then waits for a key (here
// the screen's OK button, after the run ends).
void Wardriver::display_error(const std::string& text, bool wait_key) {
    popup(gps::PopupKind::Error, text, wait_key);
    if (hooks_.serial) hooks_.serial("ERR: " + text);
    wait(200);
}

// displayTextLine() (core/display.cpp:387-395): primary-colour stripe,
// "MESSAGE: " on Serial, 200 ms delay.
void Wardriver::display_text_line(const std::string& text) {
    popup(gps::PopupKind::Message, text);
    if (hooks_.serial) hooks_.serial("MESSAGE: " + text);
    wait(200);
}

// scanWiFiBLE()'s storage failures: padprintln + displayError(msg, true) +
// returnToMenu (wardriving.cpp:220-225, 235-240).
void Wardriver::storage_failed(const std::string& text) {
    padprintln(text);
    display_error(text, true);
    return_to_menu_ = true;
}

// The sync worker reported a failed fdatasync()/close() of an earlier pass:
// checked at every tick of the inter-pass wait, so it ends the run even while
// no new fix (and so no scan) comes.
bool Wardriver::deferred_storage_error() {
    const std::string err = storage_.take_error();
    if (err.empty()) {
        return false;
    }
    hooks_.serial("Failed to write " + storage_.dir() + "/" + filename_ + ": " + err);
    storage_failed("Failed to write file");
    return true;
}

void Wardriver::report_storage_error(const std::string& detail) {
    if (hooks_.serial) hooks_.serial("Failed to write " + storage_.dir() + "/" + filename_ + ": " + detail);
    popup(gps::PopupKind::Error, "Failed to write file", true);
    end_reason_ = EndReason::StorageError; // over Stopped / NotFound: the screen waits for OK
    ended_ = true;
    gps_connected_ = false;
    dirty_ = true;
    flush();
}

void Wardriver::flush() {
    if (dirty_ && hooks_.draw) {
        hooks_.draw(frame_, popup_waits_, gps_connected_);
    }
    dirty_ = false;
}

void Wardriver::wait(uint32_t ms) {
    flush();
    hooks_.delay(ms);
}

uint64_t Wardriver::take_bytes(bool& hung_up) {
    hooks_.gps([&](size_t bytes, bool hung, gps::Parser&) {
        available_ += bytes;
        hung_up = hung;
    });
    return available_;
}

void Wardriver::run() {
    setup();
    flush();
}

// wardriving.cpp:53-70
void Wardriver::setup() {
    wifi_network_count_ = 0;
    bluetooth_device_count_ = 0;
    // ioExpander / PMIC GPS power: the Cap's GNSS is always powered.
    display_banner();
    padprintln("Initializing...");

    load_alert_macs();
    // begin_wifi() (wardriving.cpp:72-75): WiFi.mode(WIFI_STA) - iw scans
    // the interface in whatever mode it is in, nothing to switch.
    if (!begin_gps()) {
        return;
    }

    session_start_ms_ = hooks_.millis();
    wait(500);
    loop();
}

// wardriving.cpp:77-98
bool Wardriver::begin_gps() {
    // GPSserial.begin(): on Linux opening the node can fail (missing, no
    // permission, busy), which an ESP32 UART never does.
    if (options_.auto_port) {
        popup(gps::PopupKind::Message, "Searching GPS...");
        flush();
    }
    std::string message;
    bool not_found = false;
    if (!hooks_.open_gps(message, not_found)) {
        if (hooks_.esc()) {
            end(EndReason::Stopped);
            return false;
        }
        // A missing node is Bruce's "GPS not Found!"; the other (longer)
        // Linux errors wait for a key so they can be read.
        display_error(not_found ? "GPS not Found!" : message, !not_found);
        end(not_found ? EndReason::NotFound : EndReason::OpenFailed);
        return false;
    }
    if (options_.auto_port) {
        frame_.popup = gps::PopupKind::None;
        frame_.popup_text.clear();
        ++frame_.version;
        dirty_ = true;
    }

    int count = 0;
    padprintln("Waiting for GPS data");
    bool hung_up = false;
    while (take_bytes(hung_up) == 0) {
        if (hooks_.esc()) {
            end(EndReason::Stopped);
            return false;
        }
        if (hung_up) {
            // Linux only: the port went away before any byte arrived.
            display_error("GPS not Found!");
            end(EndReason::NotFound);
            return false;
        }
        display_text_line("Waiting GPS: " + std::to_string(count) + "s");
        count++;
        wait(1000);
    }

    gps_connected_ = true;
    dirty_ = true;
    return true;
}

// wardriving.cpp:100-112: the radios need no deinit (iw / bluetoothctl ran to
// completion); the front end closes the port.
void Wardriver::end(EndReason reason) {
    if (ended_) {
        return;
    }
    ended_ = true;
    end_reason_ = reason;
    return_to_menu_ = true;
    gps_connected_ = false;
    dirty_ = true;
    flush();
}

// wardriving.cpp:114-157
void Wardriver::loop() {
    int count = 0;
    return_to_menu_ = false;
    while (true) {
        display_banner();

        bool scan = false;
        bool not_found = false;
        Fix fix;
        hooks_.gps([&](size_t bytes, bool hung_up, gps::Parser& gps) {
            available_ += bytes;
            if (available_ > 0) {
                count = 0;
                available_ = 0; // while (GPSserial.available() > 0) gps.encode(...)
                if (gps.location.isUpdated()) {
                    padprintln("GPS location updated");
                    set_position(gps);
                    // What scanWiFiBLE() reads from TinyGPS++; the reader keeps
                    // parsing in the background, the firmware does not until
                    // the next pass, so the values are taken now.
                    fix.lat = gps.location.lat();
                    fix.lng = gps.location.lng();
                    fix.year = gps.date.year();
                    fix.month = gps.date.month();
                    fix.day = gps.date.day();
                    fix.hour = gps.time.hour();
                    fix.minute = gps.time.minute();
                    fix.second = gps.time.second();
                    fix.alt = gps.altitude.meters();
                    fix.hdop = gps.hdop.hdop();
                    scan = true;
                } else {
                    padprintln("GPS location not updated");
                    dump_gps_data(gps);

                    const int year = gps.date.year();
                    if (filename_.empty() && year >= options_.current_year && year < options_.current_year + 5) {
                        const unsigned y = year;
                        const unsigned mo = gps.date.month(), d = gps.date.day();
                        const unsigned h = gps.time.hour(), mi = gps.time.minute(), s = gps.time.second();
                        create_filename(y, mo, d, h, mi, s);
                    }
                }
            } else {
                // Linux: a hung-up port (USB GPS unplugged) is reported at once.
                if (count > kMaxEmptyPasses || hung_up) {
                    not_found = true;
                    return;
                }
                padprintln("No GPS data available");
                count++;
            }
        });
        if (not_found) {
            display_error("GPS not Found!");
            return end(EndReason::NotFound);
        }
        if (scan) {
            scan_wifi_ble(fix);
        }

        // while (millis() - tmp < MAX_WAIT && !gps.location.isUpdated()): the
        // firmware only encodes at the top of a pass, so location.isUpdated()
        // cannot flip during this wait - it always lasts MAX_WAIT.
        flush();
        const uint64_t tmp = hooks_.millis();
        while (hooks_.millis() - tmp < kMaxWait) {
            // A storage error wins over ESC: the reason survives a Back
            // pressed while it was being reported.
            if (return_to_menu_ || deferred_storage_error()) { // scanWiFiBLE() / the sync worker
                return end(EndReason::StorageError);
            }
            if (hooks_.esc()) {
                return end(EndReason::Stopped);
            }
            hooks_.delay(50);
        }
    }
}

// wardriving.cpp:159-168
void Wardriver::set_position(gps::Parser& gps) {
    const double lat = gps.location.lat();
    const double lng = gps.location.lng();

    if (initial_position_set_) {
        distance_ += gps::Parser::distanceBetween(cur_lat_, cur_lng_, lat, lng);
    } else {
        initial_position_set_ = true;
    }

    cur_lat_ = lat;
    cur_lng_ = lng;
}

// wardriving.cpp:170-189
void Wardriver::display_banner() {
    frame_.title = "Wardriving"; // drawMainBorderWithTitle() clears the screen
    frame_.lines.clear();
    frame_.popup = gps::PopupKind::None;
    frame_.popup_text.clear();
    popup_waits_ = false;
    ++frame_.version;
    dirty_ = true;

    padprintln("");
    if (!filename_.empty()) {
        padprintln("File: " + filename_.substr(0, filename_.size() - 4));
    }
    std::string txt = "Found";
    if (options_.scan_wifi) txt += " WiFi: " + std::to_string(wifi_network_count_);
    if (options_.scan_ble) txt += " BLE: " + std::to_string(bluetooth_device_count_);
    if (found_mac_address_count_) txt += " Alert: " + std::to_string(found_mac_address_count_);
    padprintln(txt);

    const uint64_t elapsed_ms = hooks_.millis() - session_start_ms_;
    const unsigned long elapsed_seconds = static_cast<unsigned long>(elapsed_ms / 1000);
    const unsigned long hours = elapsed_seconds / 3600;
    const unsigned long minutes = (elapsed_seconds / 60) % 60;
    const unsigned long seconds = elapsed_seconds % 60;
    padprintln(chomp(format("Distance: %.2fkm  ET: %02lu:%02lu:%02lu\n", distance_ / 1000, hours, minutes, seconds)));
}

// wardriving.cpp:191-201
void Wardriver::dump_gps_data(gps::Parser& gps) {
    if (!date_time_updated_ && (!gps.date.isUpdated() || !gps.time.isUpdated())) {
        padprintln("Waiting for valid GPS data");
        return;
    }
    date_time_updated_ = true;
    const unsigned year = gps.date.year();
    const unsigned month = gps.date.month();
    const unsigned day = gps.date.day();
    padprintln(chomp(format("Date: %02u-%02u-%02u\n", year, month, day)), 2);
    const unsigned hour = gps.time.hour();
    const unsigned minute = gps.time.minute();
    const unsigned second = gps.time.second();
    padprintln(chomp(format("Time: %02u:%02u:%02u\n", hour, minute, second)), 2);
    padprintln(chomp(format("Sat:  %u\n", static_cast<unsigned>(gps.satellites.value()))), 2);
    padprintln(chomp(format("HDOP: %.2f\n", gps.hdop.hdop())), 2);
}

// wardriving.cpp:218-431
void Wardriver::scan_wifi_ble(const Fix& fix) {
    if (!storage_.setup()) {
        storage_failed("Storage setup error");
        return;
    }
    // A previous pass's fdatasync()/close() failed (Linux: done after the
    // pass, see Storage): the observations may not be on storage.
    if (deferred_storage_error()) {
        return;
    }

    if (filename_.empty()) {
        create_filename(fix.year, fix.month, fix.day, fix.hour, fix.minute, fix.second);
    }

    // if (!exists(dir)) mkdir(dir); exists(file) ? FILE_APPEND : FILE_WRITE
    bool is_new_file = false;
    const int file = storage_.open(filename_, is_new_file);

    if (file < 0) {
        storage_failed("Failed to open file for writing");
        return;
    }

    bool closed = false;
    auto close_file = [&]() { // file.close(), once
        if (!closed) {
            closed = true;
            storage_.close(file);
        }
    };
    // The firmware ignores write errors. Here the first one ends the pass at
    // once - no further rows, scans or RSSI queries - and the run, like an
    // open error, instead of counting rows that were not stored.
    auto print = [&](const std::string& row) {
        if (storage_.write(file, row)) {
            return true;
        }
        const int err = errno;
        hooks_.serial("Failed to write " + storage_.dir() + "/" + filename_ + ": " + std::strerror(err));
        close_file();
        storage_failed("Failed to write file");
        return false;
    };

    if (is_new_file && !print(wigle_header(options_.app_version))) {
        return;
    }

    padprintln(chomp(format("Coord: %.6f, %.6f\n", fix.lat, fix.lng)));
    padprintln("Start Scanning...");
    flush();

    int networks_found = 0;
    if (options_.scan_wifi) {
        bool ok = true;
        const std::vector<ScanAp> aps = hooks_.scan_wifi(ok);
        if (hooks_.esc()) {
            close_file(); // a scan cut short by ESC is not logged
            return;
        }
        networks_found = static_cast<int>(aps.size());
        for (const auto& ap : aps) {
            uint64_t mac_key = 0;
            const bool mac_key_ok = parse_mac_u64(ap.bssid, mac_key);

            // Check if MAC was already found in this session
            enforce_registered_mac_limit();
            if (!mac_key_ok || registered_macs_.find(mac_key) == registered_macs_.end()) {
                if (!print(wifi_row(ap, fix))) {
                    return;
                }
                if (mac_key_ok) registered_macs_.insert(mac_key); // Adds MAC to cache

                // Check for alert
                check_for_alert(upper(ap.bssid), "WiFi", ap.ssid);

                wifi_network_count_++;
            }
        }
        wait(120); // WiFi.scanDelete(); vTaskDelay(120)
    }

    int ble_found = 0;
    if (options_.scan_ble) {
        flush();
        ble::ScanResult found;
        const auto need_rssi = [this](const std::string& mac) {
            uint64_t key = 0;
            return !parse_mac_u64(mac, key) || registered_macs_.find(key) == registered_macs_.end();
        };
        if (!hooks_.scan_ble(kScanTime, need_rssi, found)) {
            if (hooks_.esc()) {
                close_file();
                return;
            }
            hooks_.serial(" Failed to init BLE");
            // Deviation: Bruce reports this on Serial only; the red stripe
            // says why a BLE pass logged nothing.
            display_error("Failed to init BLE");
            close_file();
            wait(500);
            return;
        }
        if (hooks_.esc()) {
            close_file();
            return;
        }
        ble_found = static_cast<int>(found.devices.size());
        if (ble_found == 0) {
            wait(150); // pBLEScan->clearResults(); vTaskDelay(150)
        }
        for (const auto& found_dev : found.devices) {
            // Check if MAC was already found in this session
            enforce_registered_mac_limit();
            uint64_t mac_key = 0;
            const bool mac_key_ok = parse_mac_u64(found_dev.mac, mac_key);
            if (!mac_key_ok || registered_macs_.find(mac_key) == registered_macs_.end()) {
                const ble::Device& dev = found_dev;
                // Deviation (Linux): NimBLE reports every device with the RSSI
                // it advertised. One whose RSSI could not be learnt while
                // discovery ran is not logged with a made-up 0, nor marked as
                // found: the next pass that hears it logs it.
                if (dev.rssi == 0) {
                    continue;
                }
                if (!print(ble_row(dev, fix))) {
                    return;
                }
                if (mac_key_ok) registered_macs_.insert(mac_key); // Adds MAC to cache

                // Check for alert
                check_for_alert(lower(dev.mac), "BLE", dev.name);

                bluetooth_device_count_++;
            }
        }
        if (ble_found > 0) {
            wait(20); // pBLEScan->clearResults(); vTaskDelay(20)
        }
    }

    // scan_summary:
    if (options_.scan_wifi || options_.scan_ble) {
        std::string summary = "Scan done.";
        if (options_.scan_wifi) summary += " WiFi: " + std::to_string(networks_found);
        if (options_.scan_ble) summary += " BLE: " + std::to_string(ble_found);
        padprintln(summary);
    }

    close_file();
}

// wardriving.cpp:433-439
void Wardriver::enforce_registered_mac_limit() {
    if (registered_macs_.size() < kMaxRegisteredMacs) return;

    registered_macs_.clear();
    mac_cache_clears_++;
    padprintln("MAC cache cleared to prevent heap overflow");
}

// wardriving.cpp:447-479
void Wardriver::load_alert_macs() {
    if (!storage_.setup()) return;

    // if (!exists("/BruceWardriving")) mkdir(...) happens with the open.
    std::string text;
    int err = 0;
    if (storage_.read("alert.txt", text, &err)) {
        alert_macs_ = parse_alert_macs(text);
        if (!alert_macs_.empty()) {
            padprintln("Loaded " + std::to_string(alert_macs_.size()) + " alert MACs");
        }
    } else if (err == ENOENT) {
        // Create sample alert file
        storage_.create("alert.txt", alert_sample());
    }
}

// wardriving.cpp:481-494
void Wardriver::create_filename(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s) {
    filename_ = wardriving_filename(y, mo, d, h, mi, s);
}

// wardriving.cpp:512-528
void Wardriver::check_for_alert(const std::string& mac, const std::string& type, const std::string& name) {
    if (alert_macs_.find(lower(mac)) != alert_macs_.end()) {
        std::string msg = "ALERT: " + type + " found!";
        if (!name.empty()) {
            msg += " Name: " + name;
        }
        msg += " MAC: " + mac;

        found_mac_address_count_++;

        display_error(msg); // displayError(alertMsg.c_str())

        // Brief delay to make alert visible
        wait(2000);
    }
}

} // namespace wardrive

#if defined(__linux__)
namespace wardrive {
namespace {
uint64_t cap_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}
// The leader exited (not reaped yet: WNOWAIT keeps its pid and process group
// from being reused while the group is signalled).
// After the tool exited: at most this much of what a descendant still writes.
constexpr size_t kExitDrainLimit = 256 * 1024;
// stdin bytes written per loop pass.
constexpr size_t kInputBudget = 64 * 1024;

bool leader_exited(pid_t pid) {
    siginfo_t si{};
    return ::waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == pid;
}
} // namespace

namespace testing {
ssize_t (*capture_write)(int fd, const void* buf, size_t n) = ::write;
} // namespace testing

Capture run_capture(const std::vector<std::string>& argv, int timeout_ms, const std::atomic<bool>* stop,
                    int grace_ms, const std::string& input, const CaptureProbe* probe) {
    Capture c;
    int out[2];
    int in[2];
    if (argv.empty() || ::pipe2(out, O_CLOEXEC) != 0) {
        return c;
    }
    if (::pipe2(in, O_CLOEXEC) != 0) {
        ::close(out[0]);
        ::close(out[1]);
        return c;
    }
    std::vector<char*> args;
    for (const auto& a : argv) {
        args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {out[0], out[1], in[0], in[1]}) ::close(fd);
        return c;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr); // the tool gets a clean mask
        ::dup2(in[0], STDIN_FILENO);
        ::dup2(out[1], STDOUT_FILENO);
        ::dup2(out[1], STDERR_FILENO);
        ::execvp(args[0], args.data());
        const char* e = std::strerror(errno);
        (void)!::write(STDERR_FILENO, args[0], std::strlen(args[0]));
        (void)!::write(STDERR_FILENO, ": ", 2);
        (void)!::write(STDERR_FILENO, e, std::strlen(e));
        (void)!::write(STDERR_FILENO, "\n", 1);
        _exit(127);
    }
    ::setpgid(pid, pid);
    ::close(in[0]);
    ::close(out[1]);
    // stdin: written as the tool takes it (non-blocking, from this loop), then
    // closed - at once when there is no input. EOF on the tool's stdin, like
    // ProcessRunner's.
    int in_fd = in[1];
    size_t in_off = 0;
    auto close_input = [&]() {
        if (in_fd >= 0) {
            ::close(in_fd);
            in_fd = -1;
        }
    };
    if (input.empty()) {
        close_input();
    } else {
        ::fcntl(in_fd, F_SETFL, ::fcntl(in_fd, F_GETFL) | O_NONBLOCK);
    }
    // A tool that stopped reading raises SIGPIPE on this thread: blocked for
    // the write and consumed - unless one was already pending, which stays.
    const uint64_t start = cap_ms();
    const uint64_t deadline = start + static_cast<uint64_t>(timeout_ms);
    auto cancelled = [&]() { return (stop && stop->load()) || cap_ms() >= deadline; };
    // At most kInputBudget bytes per call and a Back / deadline check before
    // every write, so a tool that keeps taking input (no EAGAIN) cannot hold
    // the loop: output, exit, Back and the timeout are serviced in between.
    auto feed_input = [&]() {
        sigset_t pipe_sig, old, pending;
        sigemptyset(&pipe_sig);
        sigaddset(&pipe_sig, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe_sig, &old);
        sigemptyset(&pending);
        ::sigpending(&pending);
        const bool was_pending = sigismember(&pending, SIGPIPE) == 1;
        size_t budget = kInputBudget;
        while (in_fd >= 0 && in_off < input.size() && budget > 0 && !cancelled()) {
            const size_t want = std::min(budget, input.size() - in_off);
            const ssize_t n = testing::capture_write(in_fd, input.data() + in_off, want);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break; // wait for POLLOUT
            if (n <= 0) { // EPIPE: the tool closed its stdin
                c.input_incomplete = true;
                close_input();
                break;
            }
            in_off += static_cast<size_t>(n);
            budget -= std::min(budget, static_cast<size_t>(n));
        }
        if (in_off >= input.size()) {
            close_input();
        }
        if (!was_pending) {
            const timespec zero{};
            while (::sigtimedwait(&pipe_sig, nullptr, &zero) == SIGPIPE) {
            }
        }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
    };
    feed_input();

    bool pipe_open = true;
    bool exited = false;
    bool probed = !(probe && probe->fn);
    // What the pipe holds now, at most `limit` bytes; ends at Back / the
    // deadline too, so output that keeps coming cannot hold this up.
    auto drain = [&](size_t limit) {
        size_t got = 0;
        pollfd p{out[0], POLLIN, 0};
        while (pipe_open && got < limit && !cancelled() && ::poll(&p, 1, 0) > 0) {
            char buf[4096];
            const ssize_t n = ::read(out[0], buf, std::min(sizeof(buf), limit - got));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                pipe_open = n < 0 && errno == EAGAIN;
                break;
            }
            c.out.append(buf, static_cast<size_t>(n));
            got += static_cast<size_t>(n);
        }
    };
    // One loop for output, input, exit, stop and deadline: closing the output
    // does not end the wait for the tool, nor does it disable Back / the
    // timeout.
    while (true) {
        if (cancelled()) {
            c.aborted = true;
            break;
        }
        if (!probed && !exited && cap_ms() >= start + static_cast<uint64_t>(std::max(0, probe->at_ms))) {
            probed = true;
            drain(probe->max_snapshot); // what the tool printed up to now
            if (cancelled()) {
                c.aborted = true;
                break;
            }
            probe->fn(c.out);
            if (stop && stop->load()) {
                c.aborted = true;
            } else {
                c.ended_early = true;
            }
            break;
        }
        if (!pipe_open && exited) {
            break;
        }
        pollfd fds[2];
        nfds_t nfds = 0;
        int out_i = -1;
        int in_i = -1;
        if (pipe_open) {
            out_i = static_cast<int>(nfds);
            fds[nfds++] = pollfd{out[0], POLLIN, 0};
        }
        if (in_fd >= 0) {
            in_i = static_cast<int>(nfds);
            fds[nfds++] = pollfd{in_fd, POLLOUT, 0};
        }
        if (nfds == 0) {
            usleep(20000);
        } else if (::poll(fds, nfds, 50) > 0) {
            if (in_i >= 0 && fds[in_i].revents != 0) {
                feed_input(); // POLLOUT, or POLLERR when the tool closed its stdin
            }
            if (out_i >= 0 && fds[out_i].revents != 0) {
                char buf[4096];
                const ssize_t n = ::read(out[0], buf, sizeof(buf));
                if (n > 0) {
                    c.out.append(buf, static_cast<size_t>(n));
                } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                    pipe_open = false;
                }
            }
        }
        if (!exited && leader_exited(pid)) {
            exited = true;
            if (in_fd >= 0) { // it ended without reading all of its input
                c.input_incomplete = true;
                close_input();
            }
            // A descendant that kept the pipe open: take what is buffered.
            drain(kExitDrainLimit);
            pipe_open = false;
        }
    }
    if (in_fd >= 0) {
        c.input_incomplete = true;
        close_input();
    }
    ::close(out[0]);
    if (c.aborted || c.ended_early) {
        ::kill(-pid, SIGTERM);
        const uint64_t until = cap_ms() + static_cast<uint64_t>(grace_ms);
        while (!leader_exited(pid) && cap_ms() < until) {
            usleep(10000);
        }
    }
    // The leader is not reaped yet, so the group id is still ours: anything
    // left in it (descendants ignoring SIGTERM, or kept running after the
    // tool exited) goes now.
    ::kill(-pid, SIGKILL);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (c.ended_early) {
        c.status = 0;
    } else if (!c.aborted) {
        c.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    return c;
}
} // namespace wardrive
#endif

// --- console / child front end ------------------------------------------------------

#if defined(__linux__)
namespace {
namespace gps = platform::gps;
namespace wd = platform::wardrive;

// BLE pass: bluetoothctl's own --timeout ends a scan the probe did not; at
// most this many `info` lookups per pass (~6 ms each), bounded in time.
constexpr int kScanBackstopS = 10;
constexpr size_t kMaxRssiLookups = 400;
constexpr int kRssiLookupMs = 4000;

std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "g_stop is set from a signal handler");
std::atomic<int64_t> g_first_signal_ms{0};
static_assert(std::atomic<int64_t>::is_always_lock_free, "set from a signal handler");

int64_t signal_clock_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts); // async-signal-safe
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// First SIGINT/SIGTERM: orderly stop (the port is released, the CSV closed).
// Another one at least 1 s later forces the exit; a duplicate that arrives
// with the first (the process-group signal plus a wrapper forwarding it)
// does not skip the cleanup.
void on_signal(int sig) {
    if (g_stop.load()) {
        if (signal_clock_ms() - g_first_signal_ms.load() >= 1000) {
            _exit(128 + sig);
        }
        return;
    }
    g_first_signal_ms.store(signal_clock_ms());
    g_stop.store(true);
}

uint64_t mono_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

// Sleep `ms`, returning early (within 50 ms) once a stop was requested.
void nap(uint32_t ms) {
    const uint64_t until = mono_ms() + ms;
    while (!g_stop.load()) {
        const uint64_t now = mono_ms();
        if (now >= until) {
            break;
        }
        const uint64_t left = until - now;
        usleep(static_cast<useconds_t>((left < 50 ? left : 50) * 1000));
    }
}

void usage() {
    std::printf("usage: --wardrive wifi|ble|both [--port <node>|Auto] [--baud <bps>] [--home <dir>] "
                "[--iface <if>] [--ui] [iface]\n");
}

// The app user's home: --home, else for a root run the sudo user's passwd
// entry (never $HOME, which may be root's), else $HOME / our passwd entry.
std::string resolve_home(const std::string& given) {
    if (!given.empty()) {
        return given;
    }
    if (::geteuid() == 0) {
        const char* sudo_uid = std::getenv("SUDO_UID");
        uint32_t id = 0;
        if (sudo_uid && !wd::parse_id(sudo_uid, id)) {
            return {}; // an odd $SUDO_UID: fail closed ("Storage setup error")
        }
        const uid_t uid = sudo_uid ? static_cast<uid_t>(id) : ::geteuid();
        if (const passwd* pw = ::getpwuid(uid); pw && pw->pw_dir) {
            return pw->pw_dir;
        }
        return {};
    }
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        return home;
    }
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) {
        return pw->pw_dir;
    }
    return {};
}

} // namespace

int wardrive_main(int argc, char** argv) {
    struct sigaction sa{};
    sa.sa_handler = on_signal; // not SA_RESTART: sleeps and poll() return at once
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::string mode;
    std::string port = "/dev/serial0";
    int baud = 115200;
    std::string home_arg;
    std::string iface;
    bool ui = false;
    if (const char* env = std::getenv("GPS_UART"); env && env[0]) port = env;
    if (const char* env = std::getenv("GPS_BAUD"); env && std::atoi(env) > 0) baud = std::atoi(env);
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "wifi" || a == "ble" || a == "both") {
            mode = a;
        } else if (a == "--wifi" || a == "--ble" || a == "--both") {
            mode = a.substr(2);
        } else if (a == "--ui") {
            ui = true;
        } else if ((a == "--port" || a == "--baud" || a == "--home" || a == "--iface") && i + 1 < argc) {
            const std::string v = argv[++i] ? argv[i] : "";
            if (a == "--port") port = v;
            else if (a == "--baud") baud = std::atoi(v.c_str());
            else if (a == "--home") home_arg = v;
            else iface = v;
        } else if (a.rfind("--", 0) == 0 || !iface.empty() || a.empty()) {
            usage();
            return 2;
        } else {
            iface = a;
        }
    }
    if (baud <= 0 || port.empty()) {
        usage();
        return 2;
    }
    if (mode.empty()) {
        mode = "wifi";
    }
    if (iface.empty()) {
        iface = "wlan0";
    }

    const std::string home = resolve_home(home_arg);
    const bool root = ::geteuid() == 0;
    int sync_delay = 0;
    int sync_fault = 0;
    if (const char* d = std::getenv("BRUCE_WARDRIVE_TEST_SYNC_DELAY_MS"); d && std::atoi(d) > 0) {
        sync_delay = std::atoi(d); // tests: storage that takes this long to sync
    }
    if (const char* e = std::getenv("BRUCE_WARDRIVE_TEST_SYNC_ERRNO"); e && std::atoi(e) > 0) {
        sync_fault = std::atoi(e); // tests: fdatasync() fails with this errno
    }
    wd::StorageOptions sopt;
    sopt.sudo_uid = root ? std::getenv("SUDO_UID") : nullptr; // checked against the home's owner
    sopt.sync_delay_ms = sync_delay;
    sopt.sync_fault_errno = sync_fault;
    wd::Storage storage(home, sopt);

    gps::Session session;
    const uint64_t t0 = mono_ms();
    uint32_t shown = 0;

    wd::Wardriver::Options opt;
    opt.scan_wifi = mode != "ble";
    opt.scan_ble = mode != "wifi";
    opt.auto_port = port == "Auto";
    opt.app_version = APP_VERSION;
    if (const char* y = std::getenv("BRUCE_WARDRIVE_TEST_YEAR"); y && std::atoi(y) > 0) {
        opt.current_year = std::atoi(y); // tests: fixed CURRENT_YEAR
    }

    if (!ui) {
        std::printf("Wardriving (%s) on GPS %s @ %d bps, %s -> %s (Ctrl-C to stop)\n", mode.c_str(), port.c_str(),
                    baud, iface.c_str(), storage.dir().c_str());
    }

    wd::Wardriver::Hooks hooks;
    hooks.millis = [t0]() { return mono_ms() - t0; };
    hooks.delay = [](uint32_t ms) { nap(ms); };
    hooks.esc = []() { return g_stop.load(); };
    hooks.open_gps = [&](std::string& message, bool& not_found) {
        if (port == "Auto") {
            const auto ports = gps::auto_order("/dev/serial0", gps::port_candidates());
            const auto res = gps::auto_detect(ports, baud, {9600, 19200, 38400, 57600, 115200}, 1500, &g_stop);
            if (res.kind != gps::ProbeResult::Kind::Found) {
                message = res.message;
                not_found = false;
                return false;
            }
            std::fprintf(stderr, "Auto: %s\n", res.message.c_str());
            port = res.port;
            baud = res.baud;
        }
        gps::OpenResult r;
        if (!session.open(port, baud, r)) {
            message = r.message;
            not_found = r.error == gps::OpenError::NotFound;
            return false;
        }
        std::fprintf(stderr, "GPS on %s @ %d bps\n", port.c_str(), baud);
        return true;
    };
    hooks.gps = [&](const std::function<void(size_t, bool, gps::Parser&)>& fn) { session.snapshot(fn); };
    hooks.scan_wifi = [&](bool& ok) {
        // WiFi.scanNetworks(): an active scan. A scan already in progress
        // (NetworkManager) answers EBUSY (-16): retry a few times.
        for (int attempt = 0; attempt < 4 && !g_stop.load(); ++attempt) {
            const wd::Capture c = wd::run_capture({"iw", "dev", iface, "scan"}, 20000, &g_stop);
            if (c.aborted) {
                break;
            }
            if (c.status == 0) {
                ok = true;
                return parse_iw_scan(c.out);
            }
            if (c.out.find("(-16)") == std::string::npos) {
                std::fprintf(stderr, "iw dev %s scan: %s", iface.c_str(),
                             c.out.empty() ? "failed\n" : c.out.substr(0, 200).c_str());
                break;
            }
            nap(500);
        }
        ok = false;
        return std::vector<ScanAp>{};
    };
    hooks.scan_ble = [&](int seconds, const std::function<bool(const std::string&)>& need_rssi,
                         ble::ScanResult& out) {
        ble::ParseOptions po;
        po.max_devices = 10000;
        po.only_seen_in_scan = true;
        // bluetoothctl prints a device's RSSI only when it changes, and BlueZ
        // drops it when discovery stops: for the devices the scan window
        // showed without one, `bluetoothctl info` (one process reading the
        // commands from stdin) runs while this scan still discovers.
        std::string window;
        std::map<std::string, int> looked_up;
        wd::CaptureProbe probe;
        probe.at_ms = seconds * 1000;
        probe.fn = [&](const std::string& so_far) {
            window = so_far;
            const ble::ScanResult seen = ble::parse_scan(so_far, po);
            std::string cmds;
            size_t n = 0;
            for (const auto& d : seen.devices) {
                if (d.rssi == 0 && n < kMaxRssiLookups && need_rssi(d.mac)) {
                    cmds += "info " + d.mac + "\n";
                    ++n;
                }
            }
            if (n == 0 || g_stop.load()) {
                return;
            }
            cmds += "quit\n";
            const wd::Capture c = wd::run_capture({"bluetoothctl"}, kRssiLookupMs, &g_stop, 100, cmds);
            if (c.input_incomplete || c.aborted) {
                std::fprintf(stderr, "bluetoothctl info: %s\n", c.aborted ? "lookup cut short" : "not all commands taken");
            }
            looked_up = ble::parse_info_batch(c.out); // a cut-short batch still gives what it printed
        };
        const wd::Capture c = wd::run_capture(
            {"bluetoothctl", "--timeout", std::to_string(seconds + kScanBackstopS), "scan", "le"},
            (seconds + kScanBackstopS + 3) * 1000, &g_stop, 100, {}, &probe);
        if (c.aborted) {
            return false;
        }
        out = ble::parse_scan(c.ended_early ? window : c.out, po);
        for (auto& d : out.devices) {
            if (d.rssi == 0) {
                if (const auto it = looked_up.find(d.mac); it != looked_up.end()) {
                    d.rssi = it->second;
                }
            }
        }
        // Only a scan that really started counts: cached devices listed by a
        // bluetoothctl whose discovery failed are not observations.
        const bool failed = out.error.find("Failed to start discovery") != std::string::npos;
        if (!out.discovery_started || failed || (c.status != 0 && out.devices.empty())) {
            std::fprintf(stderr, "bluetoothctl scan le: %s", out.error.empty() ? "no discovery\n" : out.error.c_str());
            out.devices.clear();
            return false;
        }
        return true;
    };
    hooks.draw = [&](const gps::ScreenFrame& f, bool waits, bool connected) {
        if (ui) {
            std::fputs(wd::encode_frame(f, waits, connected).c_str(), stdout);
            std::fflush(stdout);
            return;
        }
        if (f.version == shown) {
            return;
        }
        shown = f.version;
        std::string title = f.title;
        for (char& ch : title) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        std::printf("---- %s ----\n", title.c_str());
        for (const auto& l : f.lines) {
            std::printf("%s%s\n", l.padx >= 2 ? "  " : "", l.text.c_str());
        }
        if (f.popup != gps::PopupKind::None) {
            std::printf("[%s] %s\n", f.popup == gps::PopupKind::Message ? "popup" : "error", f.popup_text.c_str());
        }
    };
    hooks.serial = [](const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); };

    wd::Wardriver w(opt, hooks, storage);
    w.run();
    session.close(); // the GPS port first, whatever storage does

    // Rows of the last pass are synced by the storage worker; wait for it,
    // bounded, so a hung filesystem cannot keep the process (and the menu's
    // Back) waiting.
    bool storage_ok = true;
    if (!storage.wait_idle(2500)) {
        std::fprintf(stderr, "error: CSV sync unfinished at shutdown\n");
        storage_ok = false;
    }
    if (const std::string e = storage.take_error(); !e.empty()) {
        std::fprintf(stderr, "error: %s/%s: %s\n", storage.dir().c_str(), w.filename().c_str(), e.c_str());
        storage_ok = false;
        w.report_storage_error(e); // the screen shows it (OK) whatever ended the run
    }

    if (ui) {
        std::printf("@X %s\n", wd::end_reason_name(w.end_reason()));
    } else {
        std::printf("Stopped. WiFi: %d BLE: %d Alert: %d, %.2fkm%s%s\n", w.wifi_count(), w.ble_count(),
                    w.alert_count(), w.distance_m() / 1000, w.filename().empty() ? "" : " -> ",
                    w.filename().empty() ? "" : (storage.dir() + "/" + w.filename()).c_str());
    }
    if (!storage_ok) {
        std::fflush(stdout);
        _exit(1); // a worker may still be stuck in fdatasync(): do not join it
    }
    return w.end_reason() == wd::Wardriver::EndReason::Stopped ? 0 : 1;
}
#else
int wardrive_main(int, char**) {
    std::fputs("wardriving is only available on Linux builds.\n", stdout);
    return 1;
}
#endif

} // namespace platform
