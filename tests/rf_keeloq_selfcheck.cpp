/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce's RF_DEBUG KeeLoq golden self-test (AGPL-3.0-or-later).
 * Standalone; not part of the CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/model/rf tests/rf_keeloq_selfcheck.cpp \
 *       src/model/rf/rf_keeloq.cpp -o /tmp/rf_kl_check && /tmp/rf_kl_check
 *
 * The cipher core is pinned by code-equivalence to the reference (NLF, 528
 * rounds, taps). This validates the *routes*: for each manufacturer family build
 * the hop, encrypt it (simple AND normal learning) as the TX side does, decrypt
 * it back through the real check as keeloq_identify does, and confirm the
 * button/serial/counter survive. Then round-trip every supported learning type.
 */

#include "rf_keeloq.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace model::rf;

namespace {

// Arbitrary manufacturer key (value is irrelevant to a round-trip; encrypt and
// decrypt just have to use the same derived key).
constexpr uint64_t kTestKey = 0x5cec6701b79fd949ULL;

int g_fail = 0;

bool kl_check(const std::string& mf, uint32_t dec, uint8_t btn, uint32_t serial, uint16_t expectCnt) {
    uint16_t cnt = 0;
    bool ok = (mf == "Centurion") ? keeloq_check_decrypt_centurion(dec, btn, cnt)
                                  : keeloq_check_decrypt(dec, btn, serial, cnt);
    return ok && cnt == expectCnt;
}

void check(bool ok) {
    if (!ok) g_fail++;
}

} // namespace

int main() {
    const uint8_t btn = 0x5;
    const uint16_t cnt = 0x1234;
    const uint64_t key = kTestKey;

    struct KlCase {
        const char* mf;
        uint32_t serial;
    };
    const KlCase cases[] = {
        {"Unknown", 0x4D5E6},    {"Rossi", 0x4D5E6},   {"Pecinin", 0x4D5E6},   {"Peccinin", 0x4D5E6},
        {"FAAC_RC,XT", 0x4D5E6}, {"Aprimatic", 0x4D5E6}, {"NICE_Smilo", 0x4D5E6}, {"Merlin", 0x4D5E6},
        {"Monarch", 0x4D5E6},    {"Dea_Mio", 0x4D5E6}, {"Centurion", 0x4D5E6},
    };

    for (const auto& c : cases) {
        uint32_t fix = (static_cast<uint32_t>(btn) << 28) | c.serial;
        uint32_t hop = keeloq_build_hop(c.mf, btn, c.serial, cnt);

        // Simple learning: key used directly on both sides.
        uint32_t enc_s = keeloq_encrypt(hop, key);
        bool simple_ok = kl_check(c.mf, keeloq_decrypt(enc_s, key), btn, c.serial, cnt);

        // Normal learning: man derived from fix on both sides (reference-correct).
        uint64_t man = keeloq_normal_learning(fix, key);
        uint32_t enc_n = keeloq_encrypt(hop, man);
        bool normal_ok = kl_check(c.mf, keeloq_decrypt(enc_n, man), btn, c.serial, cnt);

        bool ok = simple_ok && normal_ok;
        check(ok);
        std::printf("keeloq %-12s simple=%s normal=%s\n", c.mf, simple_ok ? "PASS" : "FAIL",
                    normal_ok ? "PASS" : "FAIL");
    }

    // Every supported learning type must round-trip through derive_man.
    struct LtCase {
        const char* name;
        uint32_t type;
    };
    const LtCase lts[] = {
        {"Simple", KEELOQ_SIMPLE_LEARNING},
        {"Normal", KEELOQ_NORMAL_LEARNING},
        {"Secure", KEELOQ_SECURE_LEARNING},
        {"MagicXor1", KEELOQ_MAGIC_XOR_TYPE1_LEARNING},
        {"MagicSer1", KEELOQ_MAGIC_SERIAL_TYPE1_LEARNING},
        {"MagicSer2", KEELOQ_MAGIC_SERIAL_TYPE2_LEARNING},
        {"MagicSer3", KEELOQ_MAGIC_SERIAL_TYPE3_LEARNING},
        {"AERF", KEELOQ_AERF_LEARNING},
        {"Erreka", KEELOQ_ERREKA_LEARNING},
        {"Pujol", KEELOQ_PUJOL_LEARNING},
        {"SimpleJCM", KEELOQ_SIMPLE_JCM_LEARNING},
    };

    const uint32_t serial = 0x4D5E6;
    const uint32_t fix = (static_cast<uint32_t>(btn) << 28) | serial;
    const uint32_t seed = fix & 0x0FFFFFFF;
    const uint32_t hop = keeloq_build_hop("Unknown", btn, serial, cnt);

    for (const auto& lt : lts) {
        uint64_t man = keeloq_derive_man(lt.type, fix, seed, key);
        uint32_t dec = keeloq_decrypt(keeloq_encrypt(hop, man), man);
        bool ok = kl_check("Unknown", dec, btn, serial, cnt);
        check(ok);
        std::printf("keeloq learning %-10s round-trip=%s\n", lt.name, ok ? "PASS" : "FAIL");
    }

    std::printf("rf keeloq self-check: %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
