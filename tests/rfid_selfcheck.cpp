/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the ISO14443A protocol helpers. Not part of the
 * CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/platform -Isrc/platform/rfid tests/rfid_selfcheck.cpp \
 *       src/platform/rfid/rfid_st25r.cpp src/platform/rfid/rfid_i2c.cpp \
 *       src/platform/cap_bringup.cpp -o /tmp/rfid_check && /tmp/rfid_check
 *
 * CRC_A is the part that silently breaks every write/select if wrong, so it is
 * checked against an independent bit-by-bit reference over random inputs rather
 * than a single memorized vector.
 */

#include "rfid_st25r.h"
#include "cap_bringup.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

// Independent reference: ISO14443-3 CRC_A, bitwise, poly 0x8408, preset 0x6363.
static void crc_ref(const uint8_t* d, size_t n, uint8_t out[2]) {
    uint16_t crc = 0x6363;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408) : static_cast<uint16_t>(crc >> 1);
        }
    }
    out[0] = static_cast<uint8_t>(crc & 0xFF);
    out[1] = static_cast<uint8_t>(crc >> 8);
}

int main() {
    using namespace platform;

    // --- CRC_A: match the independent reference on many random frames ---
    std::srand(1234);
    for (int iter = 0; iter < 5000; ++iter) {
        std::vector<uint8_t> buf(static_cast<size_t>(std::rand() % 32) + 1);
        for (auto& b : buf) b = static_cast<uint8_t>(std::rand());
        uint8_t a[2], r[2];
        iso14443a_crc(buf.data(), buf.size(), a);
        crc_ref(buf.data(), buf.size(), r);
        assert(a[0] == r[0] && a[1] == r[1]);
    }

    // --- BCC: XOR of the four UID bytes ---
    {
        const uint8_t id[4] = {0x04, 0xA1, 0xB2, 0xC3};
        assert(anticoll_bcc(id) == (0x04 ^ 0xA1 ^ 0xB2 ^ 0xC3));
    }

    // --- SAK decoding ---
    assert(sak_describe(0x00).find("Type 2") != std::string::npos);
    assert(sak_describe(0x08).find("Classic 1K") != std::string::npos);
    assert(sak_describe(0x20).find("ISO14443-4") != std::string::npos);
    assert(sak_describe(0x04).find("cascade") != std::string::npos);

    // --- hex parse / format round-trip and edge cases ---
    {
        std::vector<uint8_t> v;
        assert(parse_hex("04:A1:b2:C3", v) && v.size() == 4);
        assert(v[0] == 0x04 && v[1] == 0xA1 && v[2] == 0xB2 && v[3] == 0xC3);
        assert(to_hex(v.data(), v.size()) == "04A1B2C3");
        assert(to_hex(v.data(), v.size(), true) == "04:A1:B2:C3");
        assert(parse_hex("DEADBEEF", v) && v.size() == 4);
        assert(!parse_hex("ABC", v));   // odd length
        assert(!parse_hex("ZZ", v));    // non-hex
        assert(!parse_hex("", v));      // empty
    }

    // --- M5IOE1 register-bank math (cap bring-up) ---
    // P1 (PYG1_HAT_SW) -> L bank bit 0; P13 (PYG13_HAT_EN) -> H bank bit 4.
    assert(!cap_ioe_high_bank(1) && cap_ioe_bit(1) == 0x01);
    assert(!cap_ioe_high_bank(8) && cap_ioe_bit(8) == 0x80);
    assert(cap_ioe_high_bank(9) && cap_ioe_bit(9) == 0x01);
    assert(cap_ioe_high_bank(13) && cap_ioe_bit(13) == 0x10);
    assert(cap_ioe_high_bank(14) && cap_ioe_bit(14) == 0x20);
    assert(cap_ioe_bit(0) == 0 && cap_ioe_bit(15) == 0);
    assert(!cap_ioe_high_bank(0) && !cap_ioe_high_bank(15));

    // Load files produced by Save and Bruce firmware, including partial dumps.
    char temp_dir[] = "/tmp/bruce-nfc-check.XXXXXX";
    if (!mkdtemp(temp_dir)) return 1;
    St25rScan saved;
    saved.uid = {4, 1, 2, 3, 4, 5, 6};
    saved.atqa[0] = 0x44;
    saved.t2t = true;
    saved.pages = {4, 1, 2, 0x8F, 3, 4, 5, 6, 4, 0x48, 0, 0, 0xE1, 0x10, 0x12, 0};
    saved.page_count = static_cast<unsigned>(saved.pages.size() / 4);
    const auto dump_path = st25r_save_nfc(saved, temp_dir);
    St25rScan loaded;
    std::string error;
    assert(st25r_load_nfc(dump_path, loaded, error));
    assert(loaded.uid == saved.uid && loaded.atqa[0] == 0x44 && loaded.atqa[1] == 0);
    assert(loaded.t2t && loaded.page_count == 4 && loaded.pages == saved.pages);

    saved.pages.clear();
    saved.page_count = 0;
    assert(st25r_load_nfc(st25r_save_nfc(saved, temp_dir), loaded, error));
    assert(loaded.uid == saved.uid && loaded.t2t && loaded.pages.empty() && loaded.page_count == 0);

    const auto imported = std::filesystem::path(temp_dir) / "imported.nfc";
    const char* header = "Filetype: Flipper NFC device\r\nVersion: 4\r\n"
                         "UID: 04 01 02 03 04 05 06\r\nATQA: 44 00\r\nSAK: 00\r\n"
                         "Pages total: 45\r\nPages read: 2\r\n";
    std::ofstream(imported) << header << "Page 1: 03 04 05 06\r\nPage 0: 04 01 02 8F\r\n";
    assert(st25r_load_nfc(imported.string(), loaded, error));
    assert(loaded.page_count == 2 && loaded.pages[0] == 4 && loaded.pages[4] == 3);
    std::ofstream(imported) << header << "Page 1: 03 04 05 06\r\n";
    assert(!st25r_load_nfc(imported.string(), loaded, error) && !error.empty());
    assert(loaded.page_count == 2);
    std::filesystem::remove_all(temp_dir);

    std::puts("rfid self-check OK");
    return 0;
}
