/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Compat replacement for Bruce's src/modules/rf/rf_utils.h. The upstream header
 * drags in the whole ESP32/ELECHOUSE radio stack; the copied protocols/ tree
 * only needs the few declarations below. The radio-touching functions are
 * stubs (the real Sub-GHz RX/TX on the CM0 is RfRxCapture / rf_bitbang), while
 * the pure helpers find_pulse_index() / crc64_ecma() are faithful ports.
 */
#pragma once

#include <Arduino.h>
#include <driver/rmt_tx.h>
#include <cstdint>
#include <vector>

// RF module selector, mirroring Bruce's configPins.h enum values.
enum { M5_RF_MODULE = 0, CC1101_SPI_MODULE = 1 };

// Minimal stand-in for Bruce's BruceConfigPins, limited to the RF fields the
// protocols tree reads. Populated by the HAL before using the engine.
struct Cc1101BusPins {
    int io0 = -1; // CC1101 GDO0 (async data pin)
};
struct RfConfigPins {
    int rfModule = CC1101_SPI_MODULE;
    float rfFreq = 433.92f;
    int rfRx = -1;
    int rfTx = -1;
    Cc1101BusPins CC1101_bus;
};
extern RfConfigPins bruceConfigPins;

// Pure helpers (ported from Bruce rf_utils.cpp).
int find_pulse_index(const std::vector<int> &indexed_durations, int duration);
uint64_t crc64_ecma(const std::vector<int> &data);

// Radio glue — stubbed on Linux (ESP32 RMT/ELECHOUSE paths are never executed).
rmt_channel_handle_t setup_rf_rx(bool radioReady = false);
bool initRfModule(String mode = "", float frequency = 0);
void deinitRfModule();
