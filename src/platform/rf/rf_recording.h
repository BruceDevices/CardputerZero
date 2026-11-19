/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * RawRecording — accumulates captured Sub-GHz frames and serializes them to a
 * Flipper/Bruce SubGhz RAW `.sub` file (M8).
 *
 * Linux-neutral port of Bruce's RawRecording (no rmt_symbol_word_t): each frame
 * is the signed-µs duration list straight from RfRxCapture::poll(). Between two
 * frames a "gap" is recorded (wall-clock silence, in ms) exactly as Bruce does,
 * so the written stream matches `src/modules/rf/save.cpp`: a long negative
 * duration separating the bursts. The `.sub` format and the 512-values-per-line
 * rule follow the Flipper SubGhz RAW spec.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace platform {

struct RawRecording {
    double frequency_hz = 0.0;              // carrier, in Hz (mhz * 1e6)
    std::vector<std::vector<int>> frames;   // each = signed-µs durations
    std::vector<int> gaps_ms;               // gap AFTER frame i (size = frames-1)

    // Append a captured frame. `arrival_ms` is a monotonic timestamp (ms) at
    // capture; the gap from the previous frame is computed the way Bruce does
    // (receivedTime - lastSignalTime - frameDuration - 5), clamped to >= 0.
    void add_frame(std::vector<int> durations, uint64_t arrival_ms);

    bool empty() const { return frames.empty(); }
    size_t frame_count() const { return frames.size(); }
    size_t total_durations() const;

private:
    bool _have_prev = false;
    uint64_t _last_arrival_ms = 0;
    int _last_frame_dur_ms = 0;
};

// Monotonic clock in milliseconds (CLOCK_MONOTONIC), for frame arrival stamps.
uint64_t rf_monotonic_ms();

// Write `rec` as a Flipper/Bruce SubGhz RAW file into `dir` (created if needed),
// picking the first free `raw_<n>.sub`. Returns the full path, or "" on error /
// empty recording. Header + RAW_Data wrapping mirror Bruce's save.cpp.
std::string rf_save_sub(const RawRecording& rec, const std::string& dir);

// Load a Flipper/Bruce RAW `.sub` file. All RAW_Data lines are concatenated in
// file order into one frame, including any long negative inter-frame gaps.
// Returns false and fills `error` for malformed/unsupported input.
bool rf_load_sub(const std::string& path, RawRecording& rec, std::string& error);

} // namespace platform
