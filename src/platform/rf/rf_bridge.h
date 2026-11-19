/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Clean C++ bridge to the Bruce RF encode/decode engine (src/platform/rf/
 * protocols/, copied verbatim from upstream). This header is the ONLY surface
 * the rest of the app sees: it uses std types only, so the Arduino/ESP compat
 * shims that let the Bruce sources compile stay fully contained inside the
 * rf_protocols static library. Durations are signed microseconds (HIGH > 0,
 * LOW < 0), matching RfRxCapture / rf_bitbang_durations.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rfproto {

// Result of decoding a captured burst.
struct Decoded {
    bool ok = false;
    std::string protocol;    // registry / Flipper protocol name (e.g. "Princeton")
    std::string preset;      // radio preset name (e.g. "Ook650Async")
    uint64_t key = 0;        // decoded key / payload (raw)
    int bits = 0;            // number of payload bits
    int te = 0;              // base pulse length (µs)
    std::string decodedText; // extra human-readable detail for extended records
    bool keeloq = false;     // true when decoded by the KeeLoq state machine
};

// One entry of the static OOK protocol registry.
struct ProtoInfo {
    std::string name;
    int bits = 0;
    int te = 0;
    bool has_encoder = false;
    bool has_decoder = false;
};

// Try to identify a captured burst: runs the OOK registry decoder first, then
// the KeeLoq decoder. Returns a Decoded with ok=false if nothing matched.
Decoded decode(const std::vector<int> &durations);

// Build the signed-µs duration train for a named registry protocol. `bits`/`te`
// override the registry defaults when > 0; `repeat` frames are concatenated.
// Returns false for an unknown protocol or one without an encoder.
bool encode(const std::string &protocol, uint64_t key, int bits, int te, int repeat,
            std::vector<int> &out);

// Build the signed-µs duration train for a KeeLoq 64-bit key, `repeat` frames.
bool encode_keeloq(uint64_t key, int repeat, std::vector<int> &out);

// Static registry iteration (for enumerating "all protocols" in tests/UI).
int protocol_count();
ProtoInfo protocol_at(int index);

} // namespace rfproto
