/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace model {
namespace rf {

// One decoded (or raw) Sub-GHz signal, ported from Bruce's RfCodes with the
// Arduino String fields replaced by std::string. This is the platform-agnostic
// interchange type between the decoder/encoder (model), the CC1101 driver
// (platform), and the screens (view). No hardware, no LVGL.
//
// The ESP32-only RMT capture structs (RawRecording/RawRecordingStatus) are NOT
// ported here - on the CM0 the equivalent is a plain list of GDO0 edge
// durations (see `indexed_durations`), filled by src/platform/rf.
struct RfCodes {
    uint32_t frequency = 0; // Hz
    uint32_t serial = 0;
    uint64_t key = 0;
    uint16_t cnt = 0;
    uint32_t fix = 0;
    uint32_t hop = 0;
    uint32_t encrypted = 0;
    uint32_t seed = 0; // secure/erreka learning; 0 = derive from serial
    uint8_t btn = 0;
    std::string mf_name = "Unknown";
    std::string protocol;
    std::string preset;
    std::string data;
    int te = 0; // base pulse length (microseconds)
    std::vector<int> indexed_durations;
    std::string filepath;
    int Bit = 0;
    int BitRAW = 0;
};

// Best-RSSI candidate from a frequency sweep.
struct FreqFound {
    float freq = 0.f;
    int rssi = 0;
};

// Timing ratios for a de Bruijn/PWM-style protocol, in units of pulseLength.
struct HighLow {
    uint8_t high = 0;
    uint8_t low = 0;
};

// A classic rc-switch style protocol definition (Bruce's Protocol).
struct Protocol {
    uint16_t pulseLength = 0; // base pulse length, microseconds
    HighLow syncFactor;
    HighLow zero;
    HighLow one;
    bool invertedSignal = false;
};

// A manufacturer KeeLoq key entry.
struct KeeloqKey {
    std::string mf_name;
    uint64_t key = 0;
    uint32_t type = 0;
};

} // namespace rf
} // namespace model
