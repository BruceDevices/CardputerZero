/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Self-check for the `.sub` (Flipper/Bruce SubGhz) parser/serializer. Not part
 * of the CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/model/rf tests/rf_sub_selfcheck.cpp \
 *       src/model/rf/sub_file.cpp -o /tmp/rf_sub_check && /tmp/rf_sub_check
 *
 * Pins the two things a `.sub` port silently gets wrong: the Flipper key hex
 * encoding (8-byte spaced, nibble-accumulating parse) and the round-trip of a
 * real multi-field file (frequency, preset, protocol, keyed codes, KeeLoq
 * rolling-code extras, and RAW streams).
 */

#include "sub_file.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace model::rf;

int main() {
    // --- Flipper key hex encoding round-trip ---
    assert(u64_to_flipper_hex(0x12345678ULL) == "00 00 00 00 12 34 56 78");
    assert(hex_to_u64("00 00 00 00 12 34 56 78") == 0x12345678ULL);
    assert(hex_to_u64("AABBCCDD") == 0xAABBCCDDULL);           // compact form
    assert(hex_to_u64(u64_to_flipper_hex(0xDEADBEEFCAFEULL)) == 0xDEADBEEFCAFEULL);
    // Serial byte-triple parse ("11 22 AE FF" -> 0x1122AEFF).
    assert(hex_bytes_to_u32("11 22 AE FF") == 0x1122AEFFULL);

    // --- Parse a realistic Flipper KeeLoq .sub ---
    const std::string keeloq =
        "Filetype: Flipper SubGhz Key File\n"
        "Version: 1\n"
        "Frequency: 433920000\n"
        "Preset: FuriHalSubGhzPresetOok650Async\n"
        "Protocol: KeeLoq\n"
        "Bit: 64\n"
        "Key: 11 22 33 44 55 66 77 88\n"
        "Manufacture: Nice\n"
        "Serial: 00 12 34 56\n"
        "Button: 2\n"
        "Counter: 42\n";
    SubFile kf;
    assert(sub_parse(keeloq, kf));
    assert(kf.frequency == 433920000u);
    assert(kf.preset == "FuriHalSubGhzPresetOok650Async");
    assert(kf.protocol == "KeeLoq");
    assert(kf.keys.size() == 1 && kf.keys[0] == 0x1122334455667788ULL);
    assert(kf.bits.size() == 1 && kf.bits[0] == 64);
    assert(kf.mf_name == "Nice");
    assert(kf.serial == 0x00123456u);
    assert(kf.btn == 2 && kf.cnt == 42);

    // --- Serialize -> parse round-trip preserves the keyed code ---
    SubFile rt;
    assert(sub_parse(sub_serialize(kf), rt));
    assert(rt.frequency == kf.frequency);
    assert(rt.keys.size() == 1 && rt.keys[0] == kf.keys[0]);
    assert(rt.bits.size() == 1 && rt.bits[0] == 64);
    assert(rt.mf_name == "Nice" && rt.serial == kf.serial && rt.cnt == 42);

    // --- RAW file round-trip ---
    SubFile raw;
    raw.frequency = 315000000;
    raw.preset = "Ook270Async";
    raw.protocol = "RAW";
    raw.rawData.push_back("350 -1050 350 -350 1050 -350");
    std::string rawText = sub_serialize(raw);
    assert(rawText.find("Protocol: RAW") != std::string::npos);
    assert(rawText.find("RAW_Data: 350 -1050 350 -350 1050 -350") != std::string::npos);
    SubFile rawRt;
    assert(sub_parse(rawText, rawRt));
    assert(rawRt.protocol == "RAW");
    assert(rawRt.rawData.size() == 1);
    assert(rawRt.frequency == 315000000u);

    // --- RAW 512-value chunking spills to a second RAW_Data line ---
    {
        SubFile big;
        big.protocol = "RAW";
        std::string many;
        for (int i = 0; i < 600; i++) many += std::to_string((i % 2 ? -350 : 350)) + " ";
        big.rawData.push_back(many);
        std::string txt = sub_serialize(big);
        size_t first = txt.find("RAW_Data:");
        size_t second = txt.find("RAW_Data:", first + 1);
        assert(first != std::string::npos && second != std::string::npos); // 600 > 512 -> 2 lines
    }

    std::puts("rf .sub self-check OK");
    return 0;
}
