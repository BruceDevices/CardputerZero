/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstdint>

namespace platform {
namespace rf {

// CC1101 register math (datasheet section "Frequency / Data rate / Bandwidth").
// Pure - no SPI, no hardware - so the deterministic radio arithmetic can be unit
// tested on the host. The driver (cc1101_driver) applies the results over spidev.
//
// The M5 CAP CC1101 uses a 26 MHz crystal.
constexpr double kCc1101XoscHz = 26000000.0;

// --- Config register addresses (write: addr; burst: addr|0x40) --------------
enum : uint8_t {
    CC1101_IOCFG2 = 0x00,
    CC1101_IOCFG1 = 0x01,
    CC1101_IOCFG0 = 0x02,
    CC1101_FIFOTHR = 0x03,
    CC1101_SYNC1 = 0x04,
    CC1101_SYNC0 = 0x05,
    CC1101_PKTLEN = 0x06,
    CC1101_PKTCTRL1 = 0x07,
    CC1101_PKTCTRL0 = 0x08,
    CC1101_ADDR = 0x09,
    CC1101_CHANNR = 0x0A,
    CC1101_FSCTRL1 = 0x0B,
    CC1101_FSCTRL0 = 0x0C,
    CC1101_FREQ2 = 0x0D,
    CC1101_FREQ1 = 0x0E,
    CC1101_FREQ0 = 0x0F,
    CC1101_MDMCFG4 = 0x10,
    CC1101_MDMCFG3 = 0x11,
    CC1101_MDMCFG2 = 0x12,
    CC1101_MDMCFG1 = 0x13,
    CC1101_MDMCFG0 = 0x14,
    CC1101_DEVIATN = 0x15,
    CC1101_MCSM2 = 0x16,
    CC1101_MCSM1 = 0x17,
    CC1101_MCSM0 = 0x18,
    CC1101_FOCCFG = 0x19,
    CC1101_BSCFG = 0x1A,
    CC1101_AGCCTRL2 = 0x1B,
    CC1101_AGCCTRL1 = 0x1C,
    CC1101_AGCCTRL0 = 0x1D,
    CC1101_FREND1 = 0x21,
    CC1101_FREND0 = 0x22,
    CC1101_FSCAL3 = 0x23,
    CC1101_FSCAL2 = 0x24,
    CC1101_FSCAL1 = 0x25,
    CC1101_FSCAL0 = 0x26,
    CC1101_TEST2 = 0x2C,
    CC1101_TEST1 = 0x2D,
    CC1101_TEST0 = 0x2E,
    CC1101_PATABLE = 0x3E,
    CC1101_FIFO = 0x3F,
};

// --- Command strobes --------------------------------------------------------
enum : uint8_t {
    CC1101_SRES = 0x30,  // reset
    CC1101_SFSTXON = 0x31,
    CC1101_SXOFF = 0x32,
    CC1101_SCAL = 0x33,  // calibrate frequency synth
    CC1101_SRX = 0x34,   // enable RX
    CC1101_STX = 0x35,   // enable TX
    CC1101_SIDLE = 0x36, // exit RX/TX, go IDLE
    CC1101_SPWD = 0x39,
    CC1101_SFRX = 0x3A,  // flush RX FIFO
    CC1101_SFTX = 0x3B,  // flush TX FIFO
    CC1101_SNOP = 0x3D,
};

// --- Status registers (read: addr|0xC0) -------------------------------------
enum : uint8_t {
    CC1101_PARTNUM = 0x30,
    CC1101_VERSION = 0x31,
    CC1101_RSSI = 0x34,
    CC1101_MARCSTATE = 0x35,
};

// GDO0 (IOCFG0) setting for asynchronous serial data. In RX the pin outputs the
// demodulated OOK stream; in async TX PKTCTRL0 makes it the modulator input.
constexpr uint8_t CC1101_GDO0_ASYNC_SERIAL = 0x0D;

// Three FREQ register bytes for a carrier.
struct FreqRegs {
    uint8_t freq2;
    uint8_t freq1;
    uint8_t freq0;
};

// FREQ = round(f_carrier * 2^16 / f_xosc), packed big-endian into FREQ2/1/0.
FreqRegs cc1101_calc_freq(double freq_hz);
// Inverse: the carrier a FreqRegs triple actually programs (for validation).
double cc1101_freq_from_regs(FreqRegs r);

// MDMCFG4[3:0]=DRATE_E, MDMCFG3=DRATE_M for a target data rate (kbps -> Hz here).
// Returns the (E,M) pair; E is 4 bits, M is a full byte.
struct DataRateRegs {
    uint8_t drate_e; // 0..15
    uint8_t drate_m; // 0..255
};
DataRateRegs cc1101_calc_datarate(double baud_hz);
double cc1101_datarate_from_regs(DataRateRegs r);

// MDMCFG4[7:4] = CHANBW_E<<6 | CHANBW_M<<4 for a target RX bandwidth (Hz).
// Returns the raw high nibble already shifted into MDMCFG4 bit positions [7:4].
uint8_t cc1101_calc_rxbw_mdmcfg4_high(double bw_hz);
double cc1101_rxbw_from_mdmcfg4_high(uint8_t high);

// DEVIATN = DEVIATION_E<<4 | DEVIATION_M for a target FSK deviation (Hz).
uint8_t cc1101_calc_deviation(double dev_hz);
double cc1101_deviation_from_reg(uint8_t reg);

} // namespace rf
} // namespace platform
