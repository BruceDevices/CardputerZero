/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later). OOK capture/decode state machine
 * DERIVED FROM rc-switch (LGPL-2.1-or-later), (C) 2011 Suat Ozgur and
 * contributors; KeeLoq frame decoder framing mirrors the Flipper Zero firmware
 * (GPL-3.0-or-later).
 *
 * Only the pure decode logic is ported here. The RMT/M5-ISR capture session and
 * the M5 glitch filter (which depend on ESP32 peripherals and the Bruce board
 * config) belong to the CM0 platform layer (src/platform/rf) instead.
 */

#include "rf_decoder.h"

#include "rf_math.h"
#include "rf_registry.h"

#include <cstdlib> // abs

namespace model {
namespace rf {
namespace {

// --- Decode tuning (mirrors the classic OOK receiver) ----------------------
constexpr int kSeparationLimit = 4300;   // µs: a longer low is an inter-frame gap
constexpr int kReceiveTolerance = 60;    // % tolerance on pulse-length matching
constexpr int kNominalTeTolerance = 20;  // %
constexpr unsigned int kMaxChanges = 131; // max transitions kept per frame

inline unsigned int udiff(int a, int b) { return static_cast<unsigned int>(std::abs(a - b)); }
inline unsigned int udiff_u(unsigned int a, unsigned int b) { return (a > b) ? (a - b) : (b - a); }

bool match_protocol(
    const RfProtocolDef* pro, unsigned int changeCount, const unsigned int* timings, RfCodes& out
) {
    if (pro == nullptr) return false;
    uint64_t code = 0;
    // The longer sync factor maps to the captured inter-frame gap in timings[0].
    unsigned int syncLen = (pro->sync.low > pro->sync.high) ? pro->sync.low : pro->sync.high;
    if (syncLen == 0) return false;
    unsigned int delay = timings[0] / syncLen;
    if (delay == 0) return false;
    unsigned int teTol = pro->te * kNominalTeTolerance / 100;
    if (teTol < 80) teTol = 80;
    if (udiff_u(delay, pro->te) > teTol) return false;

    unsigned int tol = delay * kReceiveTolerance / 100;
    // Protocols that start high have their first data timing filtered out.
    unsigned int first = pro->inverted ? 2 : 1;

    for (unsigned int i = first; i + 1 < changeCount; i += 2) {
        code <<= 1;
        if (udiff(timings[i], delay * pro->zero.high) < tol &&
            udiff(timings[i + 1], delay * pro->zero.low) < tol) {
            // zero bit
        } else if (udiff(timings[i], delay * pro->one.high) < tol &&
                   udiff(timings[i + 1], delay * pro->one.low) < tol) {
            code |= 1; // one bit
        } else {
            return false;
        }
    }

    if (changeCount > 7) { // ignore very short bursts: that would be noise
        int nbits = (changeCount - 1) / 2;
        // Fixed-length protocols only match a frame of exactly their length.
        if ((pro->flags & RF_PF_FIXED_LEN) && nbits != pro->bits) return false;
        out.key = code;
        out.Bit = nbits;
        out.te = static_cast<int>(delay);
        out.protocol = pro->name;
        return true;
    }
    return false;
}

bool decode_chamberlain_9bit(const std::vector<int>& durations, RfCodes& out) {
    const unsigned int te = 430;
    const unsigned int dataTol = 260;
    const unsigned int stopTol = 700;

    for (size_t stop = 18; stop + 1 < durations.size(); stop++) {
        unsigned int stop0 = static_cast<unsigned int>(std::abs(durations[stop]));
        unsigned int stop1 = static_cast<unsigned int>(std::abs(durations[stop + 1]));
        if (udiff_u(stop0, 3000) > stopTol || udiff_u(stop1, 1000) > stopTol) continue;

        uint64_t code = 0;
        bool ok = true;
        size_t start = stop - 18;
        for (size_t i = start; i < stop; i += 2) {
            unsigned int a = static_cast<unsigned int>(std::abs(durations[i]));
            unsigned int b = static_cast<unsigned int>(std::abs(durations[i + 1]));
            code <<= 1;

            if (udiff_u(a, te * 2) <= dataTol && udiff_u(b, te) <= dataTol) {
                // zero bit: long then short
            } else if (udiff_u(a, te) <= dataTol && udiff_u(b, te * 2) <= dataTol) {
                code |= 1;
            } else {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        out.key = code;
        out.Bit = 9;
        out.te = static_cast<int>(te);
        out.protocol = "Chamberlain_9bit";
        out.preset = "Ook270Async";
        return true;
    }
    return false;
}

// --- KeeLoq PWM state machine (te_short=400, te_long=800, te_delta=180) -----
constexpr int kKlTeShort = 400;
constexpr int kKlTeLong = 800;
constexpr int kKlTeDelta = 180;
constexpr int kKlMinBits = 64;

inline uint32_t kl_diff(uint32_t a, uint32_t b) { return (a > b) ? (a - b) : (b - a); }

} // namespace

bool rf_decode_ook(const std::vector<int>& durations, RfCodes& out) {
    if (decode_chamberlain_9bit(durations, out)) return true;

    unsigned int timings[kMaxChanges];
    unsigned int changeCount = 0;
    unsigned int repeatCount = 0;
    const int protoCount = rf_protocol_count();

    for (int d : durations) {
        unsigned int dur = (d < 0) ? static_cast<unsigned int>(-d) : static_cast<unsigned int>(d);

        if (dur > static_cast<unsigned int>(kSeparationLimit)) {
            // A long stretch without a level change: likely the gap between two
            // repeated transmissions. Two similar gaps bracket a full frame.
            if (repeatCount == 0 || udiff(dur, timings[0]) < 200) {
                repeatCount++;
                if (repeatCount == 2) {
                    for (int p = 0; p < protoCount; p++) {
                        if (match_protocol(rf_protocol_at(p), changeCount, timings, out)) {
                            out.preset = "Ook270Async";
                            return true;
                        }
                    }
                    repeatCount = 0;
                }
            }
            changeCount = 0;
        }

        if (changeCount >= kMaxChanges) {
            changeCount = 0;
            repeatCount = 0;
        }
        timings[changeCount++] = dur;
    }

    return false;
}

int rf_build_raw(
    const std::vector<int>& durations, std::string& dataOut, bool& hasCrc, uint64_t& crcOut,
    std::vector<int>& indexedOut, int& bitsOut, int& teOut
) {
    dataOut.clear();
    hasCrc = false;
    crcOut = 0;
    indexedOut.clear();
    bitsOut = 0;
    teOut = 0;

    std::vector<int> pulseIndexes; // sequence of distinct-pulse indexes, for CRC
    uint8_t repetition = 0;
    int transitions = 0;

    for (int duration : durations) {
        if (duration == 0) break;
        if (transitions > 0) dataOut += " ";

        if (duration < -5000 && repetition < 2) repetition += 1;
        dataOut += std::to_string(duration);
        if (teOut == 0 && duration > 0) teOut = duration;

        if (repetition == 1 && duration >= -5000) {
            int index = find_pulse_index(indexedOut, duration);
            if (index == -1) {
                indexedOut.push_back(std::abs(duration));
                index = static_cast<int>(indexedOut.size()) - 1;
            }
            pulseIndexes.push_back(index);
        }
        transitions++;
    }

    if (repetition >= 2 && !pulseIndexes.empty()) {
        crcOut = crc64_ecma(pulseIndexes);
        bitsOut = static_cast<int>(pulseIndexes.size());
        hasCrc = true;
    } else {
        indexedOut.clear(); // only meaningful alongside a CRC
    }

    return transitions;
}

bool rf_decode_keeloq(const std::vector<int>& durations, RfCodes& out) {
    enum { ST_RESET, ST_PREAMBLE, ST_SAVE, ST_CHECK } step = ST_RESET;
    int header = 0;
    uint64_t data = 0;
    int bits = 0;
    uint32_t te_last = 0;

    for (int raw : durations) {
        bool level = raw > 0;
        uint32_t dur = static_cast<uint32_t>(raw > 0 ? raw : -raw);

        switch (step) {
            case ST_RESET:
                if (level && kl_diff(dur, kKlTeShort) < kKlTeDelta) {
                    step = ST_PREAMBLE;
                    header++;
                }
                break;
            case ST_PREAMBLE:
                if (!level && kl_diff(dur, kKlTeShort) < kKlTeDelta) {
                    step = ST_RESET; // a header LOW: keep counting via next HIGH
                    break;
                }
                if (header > 2 && kl_diff(dur, kKlTeShort * 10) < kKlTeDelta * 10) {
                    step = ST_SAVE; // sync gap found
                    data = 0;
                    bits = 0;
                } else {
                    step = ST_RESET;
                    header = 0;
                }
                break;
            case ST_SAVE:
                if (level) {
                    te_last = dur;
                    step = ST_CHECK;
                }
                break;
            case ST_CHECK:
                if (!level) {
                    if (dur >= static_cast<uint32_t>(kKlTeShort * 2 + kKlTeDelta)) {
                        // End of transmission.
                        if (bits >= kKlMinBits && bits <= kKlMinBits + 2) {
                            out.key = data;
                            out.Bit = kKlMinBits;
                            out.te = kKlTeShort;
                            out.protocol = "KeeLoq";
                            out.preset = "Ook650Async";
                            return true;
                        }
                        step = ST_RESET;
                        header = 0;
                    } else if (kl_diff(te_last, kKlTeShort) < kKlTeDelta &&
                               kl_diff(dur, kKlTeLong) < kKlTeDelta * 2) {
                        if (bits < kKlMinBits) data = (data << 1) | 1ULL;
                        bits++;
                        step = ST_SAVE;
                    } else if (kl_diff(te_last, kKlTeLong) < kKlTeDelta * 2 &&
                               kl_diff(dur, kKlTeShort) < kKlTeDelta) {
                        if (bits < kKlMinBits) data = (data << 1);
                        bits++;
                        step = ST_SAVE;
                    } else {
                        step = ST_RESET;
                        header = 0;
                    }
                } else {
                    step = ST_RESET;
                    header = 0;
                }
                break;
        }
    }

    // Capture may end before the trailing gap; accept a full 64-bit payload.
    if (bits >= kKlMinBits && bits <= kKlMinBits + 2) {
        out.key = data;
        out.Bit = kKlMinBits;
        out.te = kKlTeShort;
        out.protocol = "KeeLoq";
        out.preset = "Ook650Async";
        return true;
    }
    return false;
}

} // namespace rf
} // namespace model
