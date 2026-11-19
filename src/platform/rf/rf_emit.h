/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Sub-GHz TX (M9): encode a code to OOK durations and bit-bang it onto GDO0
 * while the CC1101 is in async TX. Port of Bruce's emit.cpp approach (drive the
 * modulation pin with microsecond timing), plus the RCSwitch protocol-1
 * (PT2262) encoder that Bruce's `subghz tx` uses, so a transmitted code decodes
 * back on a reference Bruce receiver.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace platform {

// RCSwitch protocol-1 (PT2262) encode: `bits` of `key` MSB-first, base pulse
// `te` µs, repeated `count` times, appended to `out` as signed-µs durations
// (+high / -low). Each bit: 1 -> (+3te,-1te), 0 -> (+1te,-3te); each repeat ends
// with the sync gap (+1te,-31te). Matches `subghz tx <key> <freq> <te> <count>`.
void rf_ook_encode_pt2262(uint64_t key, int bits, int te, int count, std::vector<int>& out);

// Claimed async-TX data line. Open it before strobing STX so a shared UART mux
// cannot present an unintended HIGH level (carrier on) while the radio settles.
struct RfTxLine {
    int fd = -1;
    int gpio = -1;
};

bool rf_tx_line_open(RfTxLine& line, int gpio,
                     const char* chip = "/dev/gpiochip0");
bool rf_tx_line_emit(const RfTxLine& line, const std::vector<int>& durations);
void rf_tx_line_close(RfTxLine& line);

// Bit-bang `durations` onto `gpio` (the CC1101 async TX data input): line HIGH
// for +d, LOW for -d, |d| µs each, busy-waiting on CLOCK_MONOTONIC for precision
// and running the thread at SCHED_FIFO (best-effort). Leaves the line LOW at the
// end. Convenience wrapper around the line lifecycle above.
bool rf_bitbang_durations(int gpio, const std::vector<int>& durations,
                          const char* chip = "/dev/gpiochip0");

} // namespace platform
