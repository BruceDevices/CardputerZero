/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later), DERIVED FROM the Flipper Zero firmware
 * KeeLoq cipher + keeloq_common learning schemes (GPL-3.0-or-later), (C) Flipper
 * Devices Inc. and contributors.
 */

#include "rf_keeloq.h"

namespace model {
namespace rf {
namespace {

constexpr uint32_t kKeeloqNlf = 0x3A5C742E;

inline uint32_t bit_at(uint64_t x, unsigned n) { return static_cast<uint32_t>((x >> n) & 1); }

// 5-tap index into the NLF, matching the reference g5() macro.
inline uint32_t g5(uint32_t x, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e) {
    return bit_at(x, a) + bit_at(x, b) * 2 + bit_at(x, c) * 4 + bit_at(x, d) * 8 + bit_at(x, e) * 16;
}

} // namespace

uint32_t keeloq_encrypt(uint32_t data, uint64_t key) {
    uint32_t x = data;
    for (uint32_t r = 0; r < 528; r++)
        x = (x >> 1) ^ ((bit_at(x, 0) ^ bit_at(x, 16) ^ bit_at(key, r & 63) ^
                         bit_at(kKeeloqNlf, g5(x, 1, 9, 20, 26, 31)))
                        << 31);
    return x;
}

uint32_t keeloq_decrypt(uint32_t data, uint64_t key) {
    uint32_t x = data;
    for (uint32_t r = 0; r < 528; r++)
        x = (x << 1) ^ bit_at(x, 31) ^ bit_at(x, 15) ^ bit_at(key, (15 - r) & 63) ^
            bit_at(kKeeloqNlf, g5(x, 0, 8, 19, 25, 30));
    return x;
}

uint64_t keeloq_normal_learning(uint32_t data, uint64_t key) {
    data &= 0x0FFFFFFF;
    data |= 0x20000000;
    uint32_t k1 = keeloq_decrypt(data, key);

    data &= 0x0FFFFFFF;
    data |= 0x60000000;
    uint32_t k2 = keeloq_decrypt(data, key);

    return (static_cast<uint64_t>(k2) << 32) | k1;
}

uint64_t keeloq_secure_learning(uint32_t data, uint32_t seed, uint64_t key) {
    data &= 0x0FFFFFFF;
    uint32_t k1 = keeloq_decrypt(data, key);
    uint32_t k2 = keeloq_decrypt(seed, key);
    return (static_cast<uint64_t>(k1) << 32) | k2;
}

uint64_t keeloq_magic_xor_type1_learning(uint32_t data, uint64_t xorv) {
    data &= 0x0FFFFFFF;
    return ((static_cast<uint64_t>(data) << 32) | data) ^ xorv;
}

uint64_t keeloq_magic_serial_type1_learning(uint32_t data, uint64_t man) {
    return (man & 0xFFFFFFFF) | (static_cast<uint64_t>(data) << 40) |
           (static_cast<uint64_t>(((data & 0xff) + ((data >> 8) & 0xFF)) & 0xFF) << 32);
}

uint64_t keeloq_magic_serial_type2_learning(uint32_t data, uint64_t man) {
    uint8_t* p = reinterpret_cast<uint8_t*>(&data);
    uint8_t* m = reinterpret_cast<uint8_t*>(&man);
    m[7] = p[0];
    m[6] = p[1];
    m[5] = p[2];
    m[4] = p[3];
    return man;
}

uint64_t keeloq_magic_serial_type3_learning(uint32_t data, uint64_t man) {
    return (man & 0xFFFFFFFFFF000000) | (data & 0xFFFFFF);
}

uint64_t keeloq_learning_aerf(uint32_t data, uint64_t key) {
    uint32_t d = data & 0x0FFFFFFFu;
    uint32_t k1 = keeloq_decrypt(d | 0x20000000u, key);
    uint32_t k2 = keeloq_decrypt(d | 0x60000000u, key);
    return (static_cast<uint64_t>(k2) << 32) | k1;
}

uint64_t keeloq_learning_erreka(uint32_t data, uint32_t mix, uint64_t key) {
    uint32_t d = data & 0x0FFFFFFFu;
    uint32_t k1 = keeloq_decrypt(d | 0x20000000u, key);
    uint32_t r4 = mix >> 4;
    uint32_t r1 = (mix << 4) & 0xF000F000u;
    r4 = (r4 & 0x0F000F00u) | r1;
    uint32_t r5 = mix & 0x00FF00FFu;
    uint32_t x = r4 | r5;
    x |= 0x60000000u;
    uint32_t k2 = keeloq_decrypt(x, key);
    return (static_cast<uint64_t>(k2) << 32) | k1;
}

uint64_t keeloq_learning_pujol(uint32_t data, uint64_t key) {
    uint32_t d = data & 0x0FFFFFFFu;
    uint32_t w1 = keeloq_decrypt(d | 0x20000000u, key);
    uint32_t w2 = keeloq_decrypt(d | 0x60000000u, key);
    uint32_t k1 = (w1 >> 16) | (w1 << 16);
    uint32_t k2 = (w2 >> 16) | (w2 << 16);
    return (static_cast<uint64_t>(k2) << 32) | k1;
}

uint64_t keeloq_derive_man(uint32_t type, uint32_t fix, uint32_t seed, uint64_t key) {
    switch (type) {
        case KEELOQ_NORMAL_LEARNING: return keeloq_normal_learning(fix, key);
        case KEELOQ_SECURE_LEARNING: return keeloq_secure_learning(fix, seed, key);
        case KEELOQ_MAGIC_XOR_TYPE1_LEARNING: return keeloq_magic_xor_type1_learning(fix, key);
        case KEELOQ_MAGIC_SERIAL_TYPE1_LEARNING: return keeloq_magic_serial_type1_learning(fix, key);
        case KEELOQ_MAGIC_SERIAL_TYPE2_LEARNING: return keeloq_magic_serial_type2_learning(fix, key);
        case KEELOQ_MAGIC_SERIAL_TYPE3_LEARNING: return keeloq_magic_serial_type3_learning(fix, key);
        case KEELOQ_AERF_LEARNING: return keeloq_learning_aerf(fix, key);
        case KEELOQ_ERREKA_LEARNING: return keeloq_learning_erreka(fix, seed, key);
        case KEELOQ_PUJOL_LEARNING: return keeloq_learning_pujol(fix, key);
        // SIMPLE, SIMPLE_JCM, UNKNOWN and any unhandled type encrypt with the
        // manufacturer key directly.
        default: return key;
    }
}

uint32_t keeloq_build_hop(const std::string& mf_name, uint8_t btn, uint32_t serial, uint16_t cnt) {
    const uint32_t b = static_cast<uint32_t>(btn) << 28;

    if (mf_name == "Aprimatic") {
        uint32_t apri_serial = serial;
        uint8_t apr1 = 0;
        for (uint16_t i = 1; i != 0b10000000000; i <<= 1) {
            if (apri_serial & i) apr1++;
        }
        apri_serial &= 0b00001111111111;
        if (apr1 % 2 == 0) apri_serial |= 0b110000000000;
        return b | (apri_serial & 0xFFF) << 16 | cnt;
    } else if (mf_name == "DTM_Neo" || mf_name == "FAAC_RC,XT" || mf_name == "Mutanco_Mutancode" ||
               mf_name == "Came_Space" || mf_name == "Genius_Bravo" || mf_name == "GSN" ||
               mf_name == "Rosh" || mf_name == "Rossi" || mf_name == "Pecinin" || mf_name == "Peccinin" ||
               mf_name == "Steelmate" || mf_name == "Cardin_S449") {
        return b | (serial & 0xFFF) << 16 | cnt;
    } else if (mf_name == "NICE_Smilo" || mf_name == "NICE_MHOUSE" || mf_name == "JCM_Tech") {
        return b | (serial & 0xFF) << 16 | cnt;
    } else if (mf_name == "Merlin") {
        return b | (0x000) << 16 | cnt;
    } else if (mf_name == "Centurion") {
        return b | (0x1CE) << 16 | cnt;
    } else if (mf_name == "Monarch") {
        return b | (0x100) << 16 | cnt;
    } else if (mf_name == "Dea_Mio") {
        uint8_t first_disc_num = (serial >> 8) & 0xF;
        uint8_t result_disc = (0xC + (first_disc_num % 4));
        uint32_t dea_serial = (serial & 0xFF) | (static_cast<uint32_t>(result_disc) << 8);
        return b | (dea_serial & 0xFFF) << 16 | cnt;
    }

    // Default route (Unknown and the plain manufacturers): 10-bit serial.
    return b | (serial & 0x3FF) << 16 | cnt;
}

bool keeloq_check_decrypt(uint32_t decrypt, uint8_t btn, uint32_t serial, uint16_t& cnt_out) {
    uint16_t end_serial = serial & 0xFF;
    if ((decrypt >> 28 == btn) &&
        ((((static_cast<uint16_t>(decrypt >> 16)) & 0xFF) == end_serial) ||
         (((static_cast<uint16_t>(decrypt >> 16)) & 0xFF) == 0))) {
        cnt_out = decrypt & 0xFFFF;
        return true;
    }
    return false;
}

bool keeloq_check_decrypt_centurion(uint32_t decrypt, uint8_t btn, uint16_t& cnt_out) {
    if ((decrypt >> 28 == btn) && ((static_cast<uint16_t>(decrypt >> 16) & 0x3FF) == 0x1CE)) {
        cnt_out = decrypt & 0xFFFF;
        return true;
    }
    return false;
}

} // namespace rf
} // namespace model
