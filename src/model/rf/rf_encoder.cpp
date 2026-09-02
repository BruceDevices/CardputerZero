/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later). OOK send state machine DERIVED FROM
 * rc-switch (LGPL-2.1-or-later), (C) 2011 Suat Ozgur and contributors; KeeLoq
 * OOK framing mirrors the Flipper Zero firmware (GPL-3.0-or-later).
 */

#include "rf_encoder.h"

namespace model {
namespace rf {

bool rf_encode_protocol(
    uint64_t data, unsigned int bits, int te, const RfProtocolDef* def, int repeat, std::vector<int>& out
) {
    out.clear();
    if (def == nullptr || bits == 0) return false;
    int base = (te > 0) ? te : def->te;
    if (base <= 0) return false;
    if (repeat < 1) repeat = 1;

    // Faithful port of the classic OOK send: for each repetition emit the data
    // bits MSB first, then the sync/pilot pulse. Each pulse is firstLevel for
    // high*te µs then secondLevel for low*te µs; inverted protocols swap levels.
    const bool inv = def->inverted;
    const bool hasSync = (def->sync.high != 0 || def->sync.low != 0);

    out.reserve(static_cast<size_t>(repeat) * (bits + 1) * 2);

    auto emitPulse = [&](const HighLow& p) {
        long first = static_cast<long>(p.high) * base;
        long second = static_cast<long>(p.low) * base;
        // sign: + HIGH, - LOW. Non-inverted -> first half HIGH, second LOW.
        out.push_back(inv ? -static_cast<int>(first) : static_cast<int>(first));
        out.push_back(inv ? static_cast<int>(second) : -static_cast<int>(second));
    };

    for (int r = 0; r < repeat; r++) {
        for (int i = static_cast<int>(bits) - 1; i >= 0; i--) {
            const HighLow& p = ((data >> i) & 1ULL) ? def->one : def->zero;
            emitPulse(p);
        }
        if (hasSync) emitPulse(def->sync);
    }
    return true;
}

// KeeLoq OOK framing (te_short=400, te_long=800), ported from the reference
// encoder: 11x{short,short} header, sync {short HIGH, 10*short LOW}, then 64 data
// bits MSB-first (bit 1 = short HIGH + long LOW; bit 0 = long HIGH + short LOW),
// a trailing status bit and a large inter-frame gap (40*short).
namespace {
constexpr int kKlShort = 400;
constexpr int kKlLong = 800;
} // namespace

bool rf_keeloq_durations(uint64_t key, std::vector<int>& out) {
    out.clear();
    out.reserve(11 * 2 + 2 + 64 * 2 + 4);

    // Header: 11 short HIGH/LOW pairs.
    for (int i = 0; i < 11; i++) {
        out.push_back(kKlShort);
        out.push_back(-kKlShort);
    }
    // Sync: short HIGH then 10*short LOW.
    out.push_back(kKlShort);
    out.push_back(-kKlShort * 10);

    // 64 data bits, MSB first.
    for (int i = 63; i >= 0; i--) {
        if ((key >> i) & 1ULL) { // bit 1: short HIGH, long LOW
            out.push_back(kKlShort);
            out.push_back(-kKlLong);
        } else { // bit 0: long HIGH, short LOW
            out.push_back(kKlLong);
            out.push_back(-kKlShort);
        }
    }
    // Trailing status bit + end pulse + large inter-frame gap.
    out.push_back(kKlShort);
    out.push_back(-kKlLong);
    out.push_back(kKlShort);
    out.push_back(-kKlShort * 40);
    return true;
}

bool rf_encode_raw_bits(const std::string& bits, int te, std::vector<int>& out) {
    out.clear();
    if (bits.empty() || te <= 0) return false;
    // Each bit is one half-pulse of `te` µs at the matching level. Matches the
    // legacy bit sender, which walked the string from the end toward the start.
    out.reserve(bits.size());
    for (int i = static_cast<int>(bits.size()) - 1; i >= 0; i--) {
        char c = bits[i];
        if (c == '1') out.push_back(te);
        else if (c == '0') out.push_back(-te);
        // any other char is skipped (as before)
    }
    return !out.empty();
}

} // namespace rf
} // namespace model
