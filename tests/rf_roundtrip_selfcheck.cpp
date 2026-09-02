/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Round-trip self-check for the ported RF logic layer. Not part of the CMake
 * build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/model/rf tests/rf_roundtrip_selfcheck.cpp \
 *       src/model/rf/rf_encoder.cpp src/model/rf/rf_decoder.cpp \
 *       src/model/rf/rf_registry.cpp src/model/rf/rf_math.cpp \
 *       -o /tmp/rf_rt_check && /tmp/rf_rt_check
 *
 * Encodes known payloads with the encoder, then feeds the produced durations
 * back through the decoder and asserts the same protocol + key come out. This is
 * the real integration guard: encoder and decoder are independent ports of two
 * different reference state machines, so a round-trip agreement means the whole
 * capture->decode->replay pipeline is self-consistent. Also checks the KeeLoq
 * PWM path and the RAW repeated-frame CRC builder.
 */

#include "rf_decoder.h"
#include "rf_encoder.h"
#include "rf_registry.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace model::rf;

namespace {

int g_fail = 0;

// The decoder needs the frame bracketed by two inter-frame gaps to lock, so
// encode >= 3 repetitions.
bool roundtrip_ook(const char* proto, uint64_t key, unsigned bits) {
    const RfProtocolDef* def = rf_find_protocol(proto);
    if (!def) {
        std::printf("roundtrip %-14s FAIL (no def)\n", proto);
        return false;
    }
    std::vector<int> durs;
    if (!rf_encode_protocol(key, bits, 0, def, 3, durs)) {
        std::printf("roundtrip %-14s FAIL (encode)\n", proto);
        return false;
    }
    RfCodes out;
    if (!rf_decode_ook(durs, out)) {
        std::printf("roundtrip %-14s FAIL (no decode)\n", proto);
        return false;
    }
    if (out.protocol != proto || out.key != key || static_cast<unsigned>(out.Bit) != bits) {
        std::printf(
            "roundtrip %-14s FAIL got proto=%s key=%llX bits=%d\n", proto, out.protocol.c_str(),
            static_cast<unsigned long long>(out.key), out.Bit
        );
        return false;
    }
    std::printf("roundtrip %-14s PASS (key=%llX bits=%u)\n", proto, static_cast<unsigned long long>(key), bits);
    return true;
}

void check(bool ok) {
    if (!ok) g_fail++;
}

} // namespace

int main() {
    // A mix of non-inverted, inverted, and fixed-length protocols.
    check(roundtrip_ook("Princeton", 0xABCDEF, 24));
    check(roundtrip_ook("NICE_FLO", 0x5A5, 12));
    check(roundtrip_ook("CAME", 0xC3, 12));
    check(roundtrip_ook("Ansonic", 0x9A9, 12));
    check(roundtrip_ook("Holtek_HT12", 0xF0F, 12));

    // KeeLoq: encode -> decode must return the same 64-bit key. Needs >= 2
    // frames so the trailing-gap end condition fires.
    {
        uint64_t key = 0x1234567890ABCDEFULL;
        std::vector<int> frame, durs;
        rf_keeloq_durations(key, frame);
        for (int r = 0; r < 2; r++) durs.insert(durs.end(), frame.begin(), frame.end());
        RfCodes out;
        bool ok = rf_decode_keeloq(durs, out) && out.key == key && out.Bit == 64 && out.protocol == "KeeLoq";
        std::printf("roundtrip %-14s %s\n", "KeeLoq", ok ? "PASS" : "FAIL");
        check(ok);
    }

    // RAW builder: a repeated frame (>= 2 big gaps) must be flagged with a CRC.
    {
        const RfProtocolDef* def = rf_find_protocol("Princeton");
        std::vector<int> durs;
        rf_encode_protocol(0xABCDEF, 24, 0, def, 3, durs);
        std::string data;
        bool hasCrc = false;
        uint64_t crc = 0;
        std::vector<int> indexed;
        int bitsOut = 0, teOut = 0;
        int transitions = rf_build_raw(durs, data, hasCrc, crc, indexed, bitsOut, teOut);
        bool ok = transitions > 0 && hasCrc && crc != 0 && teOut > 0 && !data.empty();
        std::printf("raw       %-14s %s (transitions=%d te=%d crc=%llx)\n", "Princeton", ok ? "PASS" : "FAIL",
                    transitions, teOut, static_cast<unsigned long long>(crc));
        check(ok);
    }

    std::printf("rf roundtrip self-check: %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
