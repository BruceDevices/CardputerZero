/*
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Ported from Bruce (AGPL-3.0-or-later). Flipper/Bruce `.sub` SubGhz file
 * format: https://github.com/flipperdevices/flipperzero-firmware
 * (documentation/file_formats/SubGhzFileFormats.md).
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace model {
namespace rf {

// Parsed contents of a `.sub` file. One file can carry several keyed codes
// (paired Key/Bit lines) OR one or more RAW streams. KeeLoq rolling-code
// metadata is optional. Pure data - the (de)serialization below is hardware and
// filesystem free so it unit-tests without a device.
struct SubFile {
    uint32_t frequency = 0; // Hz
    std::string preset;     // e.g. "Ook650Async" (canonical or Furi alias)
    std::string protocol;   // registry/Flipper name, "RAW", or "KeeLoq"
    int te = 0;             // base pulse length (µs), 0 = unset

    std::vector<uint64_t> keys; // keyed codes (up to 64-bit each)
    std::vector<int> bits;      // bit length paired with keys[i]

    std::vector<std::string> rawData; // "+a -b +c ..." streams
    std::vector<int> bitRaw;          // Bit_RAW paired with rawData[i]

    // KeeLoq rolling-code extras (written only when present).
    std::string mf_name;
    uint32_t serial = 0;
    uint8_t btn = 0;
    uint16_t cnt = 0;
    uint32_t seed = 0;

    std::string filename; // basename, informational only
};

// --- Pure (de)serialization (no filesystem) --------------------------------

// Parse the full text of a `.sub` file. Unknown lines are ignored; repeated
// Key/Bit/RAW_Data lines accumulate. Returns false only on empty input.
bool sub_parse(const std::string& text, SubFile& out);

// Render `in` back to `.sub` text (Bruce SubGhz File, Version 1). RAW streams
// are chunked to 512 values per line to stay Flipper-compatible.
std::string sub_serialize(const SubFile& in);

// Hex helpers shared with the `.sub` format (Flipper key encoding).
// Accumulate every hex nibble found (separators ignored): parses both the
// spaced "AA BB CC .." key format and a compact "AABBCC..".
uint64_t hex_to_u64(const std::string& s);
// Encode a 64-bit key as Flipper's 8-byte spaced hex: "00 00 00 00 12 34 56 78".
std::string u64_to_flipper_hex(uint64_t value);
// Parse a byte-triple hex string ("11 22 AE FF") into a 32-bit value (Serial).
uint32_t hex_bytes_to_u32(const std::string& s);

// --- Filesystem I/O (thin wrappers over the pure functions) ----------------

// Read `path` and parse it. Returns false if the file cannot be opened/read.
bool sub_load(const std::string& path, SubFile& out);
// Serialize `in` and write it to `path`. Returns false on write failure.
bool sub_save(const std::string& path, const SubFile& in);

} // namespace rf
} // namespace model
