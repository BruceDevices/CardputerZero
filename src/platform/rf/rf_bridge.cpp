/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Implementation of the clean rf_bridge.h surface. This translation unit is the
 * boundary: it includes the Arduino-flavoured Bruce engine headers (and thus
 * sees the compat shims) but exposes only std types. Compiled inside the
 * rf_protocols library, where the compat include dirs are PRIVATE.
 */
#include "rf_bridge.h"

#include "protocols/rf_codec.h"    // rf_decode_ook/keeloq, rf_encode_protocol, rf_keeloq_durations
#include "protocols/rf_registry.h" // rf_find_protocol, rf_protocol_at/count
#include "structs.h"               // RfCodes (via protocols/rf_codes.h)

namespace rfproto {

Decoded decode(const std::vector<int> &durations) {
    Decoded d;
    RfCodes c;
    if (rf_decode_ook(durations, c)) {
        d.ok = true;
    } else {
        RfCodes k;
        if (rf_decode_keeloq(durations, k)) {
            d.ok = true;
            d.keeloq = true;
            c = k;
        }
    }
    if (!d.ok) return d;
    d.protocol = c.protocol.c_str();
    d.preset = c.preset.c_str();
    d.key = c.key;
    d.bits = c.Bit;
    d.te = c.te;
    d.decodedText = c.decodedText.c_str();
    return d;
}

bool encode(const std::string &protocol, uint64_t key, int bits, int te, int repeat,
            std::vector<int> &out) {
    const RfProtocolDef *def = rf_find_protocol(String(protocol.c_str()));
    if (!def) return false;
    const unsigned int nbits = bits > 0 ? (unsigned int)bits : def->bits;
    if (def->encode) {
        RfCodes c;
        c.protocol = def->name;
        c.key = key;
        c.Bit = (int)nbits;
        c.te = te > 0 ? te : def->te;
        out.clear();
        for (int r = 0; r < (repeat > 0 ? repeat : 1); ++r) {
            if (!def->encode(c, out)) return false;
        }
        return !out.empty();
    }
    return rf_encode_protocol(key, nbits, te, def, repeat > 0 ? repeat : 1, out);
}

bool encode_keeloq(uint64_t key, int repeat, std::vector<int> &out) {
    std::vector<int> one;
    if (!rf_keeloq_durations(key, one) || one.empty()) return false;
    out.clear();
    for (int r = 0; r < (repeat > 0 ? repeat : 1); ++r)
        out.insert(out.end(), one.begin(), one.end());
    return true;
}

int protocol_count() { return rf_protocol_count(); }

ProtoInfo protocol_at(int index) {
    ProtoInfo info;
    const RfProtocolDef *def = rf_protocol_at(index);
    if (!def) return info;
    info.name = def->name ? def->name : "";
    info.bits = def->bits;
    info.te = def->te;
    info.has_encoder = (def->encode != nullptr) || (def->te > 0);
    info.has_decoder = (def->decode != nullptr) || (def->flags & RF_PF_HAS_DECODER);
    return info;
}

} // namespace rfproto
