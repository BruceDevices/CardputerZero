/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Implements the compat rf_utils.h surface for the Linux/CM0 port: the global
 * RF pin config, the LittleFS stand-in, the two pure helpers (ported verbatim
 * from Bruce rf_utils.cpp), and no-op radio stubs. The real Sub-GHz RX/TX lives
 * in RfRxCapture / rf_bitbang; nothing here touches hardware.
 */
#include "rf_utils.h"
#include <FS.h>
#include <cstdlib>

// Global RF pin config consumed by the copied protocols/ tree.
RfConfigPins bruceConfigPins;

// LittleFS global required by rf_keeloq.cpp (always misses on Linux).
FS LittleFS;

static const uint64_t CRC64_ECMA_POLY = 0x42F0E1EBA9EA3693ULL;
static const uint64_t CRC64_ECMA_INIT = 0xFFFFFFFFFFFFFFFFULL;

int find_pulse_index(const std::vector<int> &indexed_durations, int duration) {
    int abs_duration = std::abs(duration);
    int closest_index = -1;
    int closest_diff = 999999;
    for (size_t i = 0; i < indexed_durations.size(); i++) {
        int diff = std::abs(indexed_durations[i] - abs_duration);
        if (diff <= 50) return static_cast<int>(i);
        if (diff < closest_diff) {
            closest_diff = diff;
            closest_index = static_cast<int>(i);
        }
    }
    if (indexed_durations.size() < 4) return -1;
    return closest_index;
}

uint64_t crc64_ecma(const std::vector<int> &data) {
    uint64_t crc = CRC64_ECMA_INIT;
    for (int value : data) {
        crc ^= (uint64_t)value << 56;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x8000000000000000ULL) crc = (crc << 1) ^ CRC64_ECMA_POLY;
            else crc <<= 1;
        }
    }
    return crc;
}

rmt_channel_handle_t setup_rf_rx(bool) { return nullptr; }
bool initRfModule(String, float) { return false; }
void deinitRfModule() {}
