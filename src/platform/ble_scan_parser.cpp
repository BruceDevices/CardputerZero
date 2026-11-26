/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "ble_scan_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace platform::ble {
namespace {

std::string ltrim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool valid_mac(const std::string& mac) {
    if (mac.size() != 17) {
        return false;
    }
    for (size_t i = 0; i < mac.size(); ++i) {
        if (i % 3 == 2) {
            if (mac[i] != ':') {
                return false;
            }
        } else if (!std::isxdigit(static_cast<unsigned char>(mac[i]))) {
            return false;
        }
    }
    return true;
}

// bluetoothctl uses the hyphenated MAC as the "name" when the device has no
// name; the firmware treats that as no name.
std::string normalize_name(std::string name, const std::string& mac) {
    name = trim(name);
    if (name.empty() || name == "(unknown)") {
        return {};
    }
    std::string hyphen = mac;
    for (char& c : hyphen) {
        if (c == ':') {
            c = '-';
        }
    }
    if (name == mac || name == hyphen) {
        return {};
    }
    return name;
}

bool error_line(const std::string& line) {
    return starts_with(line, "Failed ") || starts_with(line, "Unable ") ||
           starts_with(line, "No default controller") || starts_with(line, "bluetoothctl:") ||
           starts_with(line, "sudo:") || starts_with(line, "sh:") || starts_with(line, "sed:") ||
           starts_with(line, "timeout:") || line.find("org.bluez.Error") != std::string::npos;
}

// `[CHG] Device AA:BB:.. RSSI: 0xffffffb4 (-76)` -> -76
bool parse_rssi(const std::string& rest, int& rssi) {
    const size_t open = rest.find('(');
    if (open == std::string::npos) {
        return false;
    }
    const size_t close = rest.find(')', open + 1);
    if (close == std::string::npos) {
        return false;
    }
    rssi = std::atoi(rest.substr(open + 1, close - open - 1).c_str());
    return true;
}

// BlueZ's "no RSSI" values (0, and 127 = HCI "not available").
bool usable_rssi(int rssi) {
    return rssi != 0 && rssi != 127 && rssi >= -127 && rssi < 20;
}

// `ManufacturerData.Key: 0x004c (76)` (BlueZ >= 5.65) or
// `ManufacturerData Key: 0x004c` (older) -> 0x004c
bool parse_mfgr_key(const std::string& rest, uint16_t& id) {
    const char* const kKeys[] = {"ManufacturerData.Key: ", "ManufacturerData Key: "};
    for (const char* key : kKeys) {
        if (starts_with(rest, key)) {
            const std::string v = trim(rest.substr(std::string(key).size()));
            char* end = nullptr;
            const unsigned long n = std::strtoul(v.c_str(), &end, 0);
            if (end == v.c_str() || n > 0xffff) {
                return false;
            }
            id = static_cast<uint16_t>(n);
            return true;
        }
    }
    return false;
}

} // namespace

std::string strip_escapes(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x1b) {
            if (i + 1 < in.size() && in[i + 1] == '[') {
                i += 2;
                while (i < in.size() && !(in[i] >= '@' && in[i] <= '~')) {
                    ++i;
                }
            } else if (i + 1 < in.size()) {
                ++i;
            }
            continue;
        }
        if (c == 0x01 || c == 0x02 || c == '\r') {
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t') {
            continue;
        }
        out += static_cast<char>(c);
    }
    return out;
}

ScanResult parse_scan(const std::string& output, const ParseOptions& options) {
    ScanResult result;
    struct Entry {
        Device dev;
        bool seen = false;
    };
    std::vector<Entry> entries;
    std::unordered_map<std::string, size_t> index;
    bool discovering = false;
    bool any_marker = false;
    size_t over_limit = 0;

    const size_t cap = options.only_seen_in_scan ? std::max<size_t>(options.max_devices * 4, 1000)
                                                 : options.max_devices;
    auto touch = [&](const std::string& mac) -> Entry* {
        if (!valid_mac(mac)) {
            return nullptr;
        }
        auto it = index.find(mac);
        if (it == index.end()) {
            if (entries.size() >= cap) {
                ++over_limit; // bounded while parsing, like the screen's old parser
                return nullptr;
            }
            index[mac] = entries.size();
            entries.push_back(Entry{});
            entries.back().dev.mac = mac;
            it = index.find(mac);
        }
        Entry& e = entries[it->second];
        if (discovering) {
            e.seen = true;
        }
        return &e;
    };
    auto set_name = [](Entry& e, const std::string& name) {
        const std::string clean = normalize_name(name, e.dev.mac);
        if (e.dev.name.empty() && !clean.empty()) {
            e.dev.name = clean;
        }
    };

    std::istringstream stream(strip_escapes(output));
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (line == "Discovery started" ||
            (starts_with(line, "[CHG] Controller ") && line.find("Discovering: yes") != std::string::npos)) {
            discovering = true;
            any_marker = true;
            continue;
        }
        if (starts_with(line, "[NEW] Device ") || starts_with(line, "[CHG] Device ")) {
            const bool is_new = starts_with(line, "[NEW]");
            std::istringstream ls(line);
            std::string tag, kind, mac;
            ls >> tag >> kind >> mac;
            std::string rest;
            std::getline(ls, rest);
            rest = ltrim(rest);
            Entry* e = touch(mac);
            if (!e) {
                continue;
            }
            if (is_new) {
                set_name(*e, rest);
            } else if (starts_with(rest, "Name: ")) {
                set_name(*e, rest.substr(6));
            } else if (starts_with(rest, "Alias: ")) {
                set_name(*e, rest.substr(7));
            } else if (starts_with(rest, "RSSI: ")) {
                int rssi = 0;
                if (parse_rssi(rest, rssi) && usable_rssi(rssi)) { // an unknown value keeps the last one
                    e->dev.rssi = rssi;
                }
            } else {
                uint16_t id = 0;
                if (parse_mfgr_key(rest, id)) {
                    e->dev.mfgr_id = id;
                }
            }
            continue;
        }
        if (error_line(line)) {
            result.error += line;
            result.error += '\n';
        }
        // Discovery status and property-change noise are intentionally ignored.
    }

    result.discovery_started = any_marker;
    for (auto& e : entries) {
        const bool filter = options.only_seen_in_scan;
        if (filter && !e.seen) {
            continue;
        }
        if (result.devices.size() >= options.max_devices) {
            ++over_limit;
            continue;
        }
        result.devices.push_back(std::move(e.dev));
    }
    result.truncated = over_limit > 0;
    return result;
}

namespace {
// "RSSI: 0xffffffb4 (-76)" or "RSSI: -76" (a trimmed line)
bool parse_rssi_line(const std::string& line, int& rssi) {
    if (!starts_with(line, "RSSI: ")) {
        return false;
    }
    if (parse_rssi(line, rssi)) {
        return true;
    }
    char* end = nullptr;
    const long v = std::strtol(line.c_str() + 6, &end, 10);
    if (end != line.c_str() + 6) {
        rssi = static_cast<int>(v);
        return true;
    }
    return false;
}

} // namespace

bool parse_info_rssi(const std::string& output, int& rssi) {
    std::istringstream stream(strip_escapes(output));
    std::string line;
    while (std::getline(stream, line)) {
        if (parse_rssi_line(trim(line), rssi)) {
            return true;
        }
    }
    return false;
}

std::map<std::string, int> parse_info_batch(const std::string& output) {
    std::map<std::string, int> rssi_of;
    std::string current; // the MAC of the "Device ... (...)" block being read
    std::istringstream stream(strip_escapes(output));
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        // An event, possibly printed right after a prompt.
        if (const size_t chg = line.find("[CHG] Device "); chg != std::string::npos) {
            std::istringstream ls(line.substr(chg));
            std::string tag, kind, mac;
            ls >> tag >> kind >> mac;
            std::string rest;
            std::getline(ls, rest);
            int v = 0;
            if (valid_mac(mac) && parse_rssi_line(ltrim(rest), v) && usable_rssi(v)) {
                rssi_of[mac] = v;
            }
            continue;
        }
        if (line.size() > 1 && line[0] == '[') { // "[bluetoothctl]> info <mac>"
            const size_t prompt = line.find("]> ");
            if (prompt != std::string::npos) {
                line = trim(line.substr(prompt + 3));
            }
        }
        if (starts_with(line, "info ") || starts_with(line, "[")) {
            current.clear();
            continue;
        }
        if (starts_with(line, "Device ")) {
            std::istringstream ls(line);
            std::string kind, mac, type;
            ls >> kind >> mac >> type;
            current = valid_mac(mac) && !type.empty() && type[0] == '(' ? mac : std::string();
            continue;
        }
        int v = 0;
        if (!current.empty() && parse_rssi_line(line, v) && usable_rssi(v)) {
            rssi_of[current] = v;
        }
    }
    return rssi_of;
}

} // namespace platform::ble
