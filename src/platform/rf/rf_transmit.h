/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <string>
#include <vector>

namespace platform {
namespace rf {

// Transmits an OOK frame by bit-banging the CC1101 GDO0 line (async-serial
// modulator input) with libgpiod. Replaces the ESP32 RMT TX peripheral: the
// caller puts the radio in async TX (Cc1101::enter_tx_async), then this drives
// GDO0 HIGH/LOW to key the carrier per a signed-µs duration list (+ HIGH = key
// on, - LOW = key off), exactly the format the encoder produces.
//
// Timing note: this is userspace bit-banging. Absolute clock_nanosleep deadlines
// keep cumulative drift low, but scheduler jitter still perturbs individual
// edges - acceptable for most remotes; pin the process to a CPU / raise priority
// if a picky receiver rejects frames. Requires libgpiod (APP_HAVE_LIBGPIOD);
// without it transmit() is a no-op returning false so desktop builds link.
class GpioBitBangTx {
public:
    GpioBitBangTx() = default;
    ~GpioBitBangTx();

    GpioBitBangTx(const GpioBitBangTx&) = delete;
    GpioBitBangTx& operator=(const GpioBitBangTx&) = delete;

    // Request GDO0 as an output (idle LOW). `line_offset` is the kernel line
    // number for GDO0 (not the silkscreen G#).
    bool open(const std::string& chip, unsigned int line_offset);
    void close();
    bool is_open() const;

    // Drive `durations` (signed µs) once, leaving the line LOW. Blocks until the
    // whole frame is sent. Returns false if not open or the list is empty.
    bool transmit(const std::vector<int>& durations);

private:
    void* chip_ = nullptr;
    void* line_ = nullptr;
};

} // namespace rf
} // namespace platform
