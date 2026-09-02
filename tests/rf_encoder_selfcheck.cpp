/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce's RF_DEBUG encoder self-test (AGPL-3.0-or-later). Standalone
 * self-check for the ported OOK/KeeLoq pulse-train builders. Not part of the
 * CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/model/rf tests/rf_encoder_selfcheck.cpp \
 *       src/model/rf/rf_encoder.cpp src/model/rf/rf_registry.cpp \
 *       -o /tmp/rf_enc_check && /tmp/rf_enc_check
 *
 * For each reference-derived protocol, encode key=0xA / 4 bits / 1 repetition
 * and compare the produced durations against the EXPECTED absolute timings,
 * written straight from the protocol spec (not re-derived from the registry
 * factors). A mismatch catches a wrong factor, te, or pulse order in a registry
 * def - the spec-fidelity gap that a self-consistent loopback would hide. Also
 * pins the KeeLoq frame length + header.
 */

#include "rf_encoder.h"
#include "rf_registry.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace model::rf;

namespace {

constexpr int kGoldenTol = 16; // µs (covers protocols whose te_long != k*te_short)

// key=0xA -> bits 1,0,1,0 (MSB first) -> one,zero,one,zero,sync
const int g_linear[]     = {1500, -500, 500, -1500, 1500, -500, 500, -1500, 1500, -21000};
const int g_clemsa[]     = {2695, -385, 385, -2695, 2695, -385, 385, -2695, 2695, -19250};
const int g_mastercode[] = {2145, -1072, 1072, -2145, 2145, -1072, 1072, -2145, 2145, -15008};
const int g_came[]       = {-320, 640, -640, 320, -320, 640, -640, 320, -11520, 320};
const int g_nice12[]     = {-1400, 700, -700, 1400, -1400, 700, -700, 1400, -25200, 700};
const int g_ansonic[]    = {-1111, 555, -555, 1111, -1111, 555, -555, 1111, -19425, 555};
const int g_gatetx[]     = {-700, 350, -350, 700, -700, 350, -350, 700, -17150, 700};
const int g_holtek[]     = {-870, 430, -430, 870, -870, 430, -430, 870, -15480, 430};
const int g_phoenixv2[]  = {-853, 427, -427, 853, -853, 427, -427, 853, -25620, 2562};

struct Case {
    const char* name;
    uint64_t key;
    unsigned bits;
    const int* exp;
    size_t n;
};

bool check_protocol(const Case& c) {
    const RfProtocolDef* def = rf_find_protocol(c.name);
    std::vector<int> durs;
    if (def == nullptr || !rf_encode_protocol(c.key, c.bits, 0, def, 1, durs)) {
        std::printf("selftest %-12s FAIL (no def/encode)\n", c.name);
        return false;
    }
    if (durs.size() != c.n) {
        std::printf("selftest %-12s FAIL (len got=%zu exp=%zu)\n", c.name, durs.size(), c.n);
        return false;
    }
    for (size_t i = 0; i < c.n; i++) {
        if (std::abs(durs[i] - c.exp[i]) > kGoldenTol) {
            std::printf("selftest %-12s FAIL @%zu got=%d exp=%d\n", c.name, i, durs[i], c.exp[i]);
            return false;
        }
    }
    std::printf("selftest %-12s PASS\n", c.name);
    return true;
}

} // namespace

int main() {
    const Case cases[] = {
        {"Linear", 0xA, 4, g_linear, sizeof(g_linear) / sizeof(int)},
        {"Clemsa", 0xA, 4, g_clemsa, sizeof(g_clemsa) / sizeof(int)},
        {"Mastercode", 0xA, 4, g_mastercode, sizeof(g_mastercode) / sizeof(int)},
        {"CAME", 0xA, 4, g_came, sizeof(g_came) / sizeof(int)},
        {"Nice_12bit", 0xA, 4, g_nice12, sizeof(g_nice12) / sizeof(int)},
        {"Ansonic", 0xA, 4, g_ansonic, sizeof(g_ansonic) / sizeof(int)},
        {"GateTX", 0xA, 4, g_gatetx, sizeof(g_gatetx) / sizeof(int)},
        {"Holtek", 0xA, 4, g_holtek, sizeof(g_holtek) / sizeof(int)},
        {"Holtek_12bit", 0xA, 4, g_holtek, sizeof(g_holtek) / sizeof(int)},
        {"PhoenixV2", 0xA, 4, g_phoenixv2, sizeof(g_phoenixv2) / sizeof(int)},
    };

    bool allok = true;
    for (const auto& c : cases) allok = check_protocol(c) && allok;

    // KeeLoq frame: 11-pair header + 1 sync pair + 64 bit pairs + 2 trailing
    // pairs = (11 + 1 + 64 + 2) * 2 = 156 durations. Header is short HIGH/LOW.
    std::vector<int> kl;
    bool klok = rf_keeloq_durations(0x1234567890ABCDEFULL, kl);
    if (!klok || kl.size() != 156 || kl[0] != 400 || kl[1] != -400) {
        std::printf("selftest KeeLoq       FAIL (ok=%d size=%zu)\n", klok, kl.size());
        allok = false;
    } else {
        std::puts("selftest KeeLoq       PASS");
    }

    std::printf("rf encoder self-check: %s\n", allok ? "ALL PASS" : "FAILURES");
    return allok ? 0 : 1;
}
