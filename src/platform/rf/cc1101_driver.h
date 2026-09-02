/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include "cc1101_regs.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace platform {
namespace rf {

// CC1101 driver over Linux spidev for the M5 CAP. Owns the spidev fd and offers
// register access + the OOK async-serial configuration the Sub-GHz capture/TX
// paths need. Register math is in cc1101_regs (unit-tested); this file is the
// I/O half and can only be exercised on the device.
//
// Async-serial model (no RMT peripheral on the CM0): the chip does modulation/
// demodulation, and the raw OOK bitstream crosses on GDO0. RX -> GDO0 outputs
// the sliced data (read as edges by rf_capture, libgpiod); TX -> GDO0 is the
// modulator input driven by rf_transmit. This driver only sets the radio up and
// switches state; the GDO0 line itself is handled by the capture/transmit
// modules so SPI and GPIO stay decoupled.
class Cc1101 {
public:
    Cc1101() = default;
    ~Cc1101();

    Cc1101(const Cc1101&) = delete;
    Cc1101& operator=(const Cc1101&) = delete;

    // Open the spidev node (mode 0, 8 bits). Returns false if it cannot be
    // opened/configured. Defaults to APP_CC1101_SPIDEV.
    bool open(const std::string& spidev_path = {});
    void close();
    bool is_open() const { return fd_ >= 0; }

    // --- Low-level register access ------------------------------------------
    bool strobe(uint8_t cmd);
    bool write_reg(uint8_t addr, uint8_t value);
    bool write_burst(uint8_t addr, const uint8_t* data, size_t n);
    uint8_t read_reg(uint8_t addr);    // config register (addr | 0x80)
    uint8_t read_status(uint8_t addr); // status register (addr | 0xC0)

    // --- Lifecycle ----------------------------------------------------------
    bool reset();       // SRES + settle
    bool detected();    // plausible VERSION register (0x04/0x14/0x17)
    int marcstate();    // MARCSTATE & 0x1F, or -1 if not open

    // --- Configuration ------------------------------------------------------
    // Program the carrier (Hz). Chip should be IDLE; a SCAL follows on RX/TX.
    void set_frequency_hz(double hz);
    // Write the baseline OOK async-serial register block (GDO0 = async serial,
    // PKTCTRL0 async, MOD_FORMAT = ASK/OOK, no sync/CRC). Call once after reset.
    void configure_ook_async();
    // Apply a model preset: modulation (0=2-FSK,1=GFSK,2=ASK/OOK,4=MSK) plus
    // optional deviation / RX bandwidth / data rate in Hz (0 = keep default).
    void apply_preset(uint8_t modulation, double deviation_hz, double rxbw_hz, double datarate_hz);
    // Set the PA_TABLE for OOK: entry 0 = off, entry 1 = `power` (0xC0 ~ +10dBm).
    void set_ook_patable(uint8_t power = 0xC0);

    // --- State switching ----------------------------------------------------
    void idle();
    void flush_rx();
    void flush_tx();
    // Put GDO0 into async-serial output and enter RX (for edge capture).
    void enter_rx_async();
    // Put GDO0 into async-serial input and enter TX (for bit-bang transmit).
    void enter_tx_async();

private:
    bool xfer(const uint8_t* tx, uint8_t* rx, size_t len);
    int fd_ = -1;
};

} // namespace rf
} // namespace platform
