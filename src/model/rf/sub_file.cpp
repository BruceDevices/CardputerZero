/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later). Flipper/Bruce `.sub` SubGhz file
 * format read/write. The ESP32/Arduino FS + RMT specifics are replaced by plain
 * std::string / std::fstream; the field semantics match Bruce's readSubFile /
 * rfSaveSignal / rf_raw_save.
 */

#include "sub_file.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace model {
namespace rf {
namespace {

uint8_t hex_char(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// Trim ASCII whitespace (incl. a trailing '\r') from both ends.
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// The value after the first ':' on a line, trimmed.
std::string value_of(const std::string& line) {
    size_t pos = line.find(':');
    if (pos == std::string::npos) return trim(line);
    return trim(line.substr(pos + 1));
}

long to_long(const std::string& s) {
    if (s.empty()) return 0;
    return std::strtol(s.c_str(), nullptr, 10);
}

} // namespace

uint64_t hex_to_u64(const std::string& s) {
    uint64_t value = 0;
    for (char c : s) {
        if (!is_hex(c)) continue; // skip spaces and any separators
        value = (value << 4) | static_cast<uint64_t>(hex_char(c));
    }
    return value;
}

uint32_t hex_bytes_to_u32(const std::string& s) {
    // Byte-triple stride ("11 22 AE FF"): two hex chars per byte + separator.
    uint32_t decimal = 0;
    int length = static_cast<int>(s.size());
    for (int i = 0; i + 1 < length; i += 3) {
        decimal <<= 8;
        decimal |= (hex_char(s[i]) << 4) | hex_char(s[i + 1]);
    }
    return decimal;
}

std::string u64_to_flipper_hex(uint64_t value) {
    static const char* digits = "0123456789ABCDEF";
    char temp[16];
    for (int i = 0; i < 16; i++) temp[i] = '0';
    int index = 15;
    while (value > 0 && index >= 0) {
        temp[index--] = digits[value % 16];
        value /= 16;
    }
    std::string out;
    out.reserve(23); // 16 digits + 7 spaces
    for (int i = 0; i < 16; i++) {
        out.push_back(temp[i]);
        if ((i % 2) == 1 && i != 15) out.push_back(' ');
    }
    return out;
}

bool sub_parse(const std::string& text, SubFile& out) {
    if (text.empty()) return false;
    out = SubFile{};

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string txt = value_of(line);
        if (starts_with(line, "Protocol:")) out.protocol = txt;
        else if (starts_with(line, "Preset:")) out.preset = txt;
        else if (starts_with(line, "Frequency:")) out.frequency = static_cast<uint32_t>(to_long(txt));
        else if (starts_with(line, "TE:")) out.te = static_cast<int>(to_long(txt));
        else if (starts_with(line, "Bit_RAW:")) out.bitRaw.push_back(static_cast<int>(to_long(txt)));
        else if (starts_with(line, "Bit:")) out.bits.push_back(static_cast<int>(to_long(txt)));
        else if (starts_with(line, "Key:")) out.keys.push_back(hex_to_u64(txt));
        else if (starts_with(line, "RAW_Data:") || starts_with(line, "Data_RAW:")) out.rawData.push_back(txt);
        // KeeLoq rolling-code fields; Flipper writes "Manufacture" (no r).
        else if (starts_with(line, "Manufacturer:") || starts_with(line, "Manufacture:")) out.mf_name = txt;
        else if (starts_with(line, "Serial:")) out.serial = hex_bytes_to_u32(txt);
        else if (starts_with(line, "Button:")) out.btn = static_cast<uint8_t>(to_long(txt));
        else if (starts_with(line, "Counter:")) out.cnt = static_cast<uint16_t>(to_long(txt));
        else if (starts_with(line, "Seed:")) out.seed = static_cast<uint32_t>(hex_to_u64(txt));
    }
    return true;
}

std::string sub_serialize(const SubFile& in) {
    std::string s;
    s += "Filetype: Bruce SubGhz File\n";
    s += "Version 1\n";
    s += "Frequency: " + std::to_string(in.frequency) + "\n";
    s += "Preset: " + (in.preset.empty() ? std::string("Ook650Async") : in.preset) + "\n";

    const bool isRaw = (in.protocol == "RAW") || (in.keys.empty() && !in.rawData.empty());

    if (isRaw) {
        s += "Protocol: RAW\n";
        // Flipper caps RAW_Data at 512 values per line; re-chunk on write.
        for (size_t i = 0; i < in.rawData.size(); i++) {
            if (i < in.bitRaw.size()) s += "Bit_RAW: " + std::to_string(in.bitRaw[i]) + "\n";

            std::istringstream vals(in.rawData[i]);
            std::string tok;
            int count = 0;
            std::string lineBuf = "RAW_Data: ";
            bool any = false;
            while (vals >> tok) {
                lineBuf += tok;
                lineBuf += ' ';
                any = true;
                if (++count % 512 == 0) {
                    s += lineBuf;
                    s += '\n';
                    lineBuf = "RAW_Data: ";
                }
            }
            if (any && count % 512 != 0) {
                s += lineBuf;
                s += '\n';
            }
        }
        return s;
    }

    s += "Protocol: " + (in.protocol.empty() ? std::string("RcSwitch") : in.protocol) + "\n";
    if (in.te > 0) s += "TE: " + std::to_string(in.te) + "\n";

    // KeeLoq rolling-code extras, written once when a manufacturer is known.
    if (!in.mf_name.empty()) {
        if (in.seed != 0) s += "Seed: " + u64_to_flipper_hex(in.seed) + "\n";
        s += "Manufacture: " + in.mf_name + "\n";
        s += "Serial: " + u64_to_flipper_hex(in.serial) + "\n";
        s += "Button: " + std::to_string(in.btn) + "\n";
        s += "Counter: " + std::to_string(in.cnt) + "\n";
    }

    for (size_t i = 0; i < in.keys.size(); i++) {
        if (i < in.bits.size()) s += "Bit: " + std::to_string(in.bits[i]) + "\n";
        s += "Key: " + u64_to_flipper_hex(in.keys[i]) + "\n";
    }
    return s;
}

bool sub_load(const std::string& path, SubFile& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buf;
    buf << file.rdbuf();
    if (!sub_parse(buf.str(), out)) return false;

    // Record the basename (informational, like Bruce's selected_code.filepath).
    const size_t slash = path.find_last_of("/\\");
    out.filename = (slash == std::string::npos) ? path : path.substr(slash + 1);
    return true;
}

bool sub_save(const std::string& path, const SubFile& in) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << sub_serialize(in);
    return static_cast<bool>(file);
}

} // namespace rf
} // namespace model
