/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "cc1101_regs.h"

#include <cmath>

namespace platform {
namespace rf {

FreqRegs cc1101_calc_freq(double freq_hz) {
    // FREQ = f_carrier * 2^16 / f_xosc (datasheet 21). Round to nearest.
    double f = freq_hz * 65536.0 / kCc1101XoscHz;
    uint32_t word = static_cast<uint32_t>(f + 0.5) & 0x00FFFFFF;
    return FreqRegs{
        static_cast<uint8_t>((word >> 16) & 0xFF),
        static_cast<uint8_t>((word >> 8) & 0xFF),
        static_cast<uint8_t>(word & 0xFF),
    };
}

double cc1101_freq_from_regs(FreqRegs r) {
    uint32_t word = (static_cast<uint32_t>(r.freq2) << 16) | (static_cast<uint32_t>(r.freq1) << 8) | r.freq0;
    return static_cast<double>(word) * kCc1101XoscHz / 65536.0;
}

DataRateRegs cc1101_calc_datarate(double baud_hz) {
    // R_DATA = (256 + DRATE_M) * 2^DRATE_E / 2^28 * f_xosc  (datasheet 12).
    // DRATE_E = floor(log2(R * 2^20 / f_xosc)); DRATE_M rounds the mantissa.
    if (baud_hz <= 0) return DataRateRegs{0, 0};
    double ratio = baud_hz * (1u << 20) / kCc1101XoscHz;
    int e = static_cast<int>(std::floor(std::log2(ratio)));
    if (e < 0) e = 0;
    if (e > 15) e = 15;

    double m = baud_hz * std::pow(2.0, 28 - e) / kCc1101XoscHz - 256.0;
    int mi = static_cast<int>(m + 0.5);
    if (mi > 255) { // mantissa overflow -> bump exponent
        mi = 0;
        if (e < 15) e++;
    }
    if (mi < 0) mi = 0;
    return DataRateRegs{static_cast<uint8_t>(e), static_cast<uint8_t>(mi)};
}

double cc1101_datarate_from_regs(DataRateRegs r) {
    return (256.0 + r.drate_m) * std::pow(2.0, r.drate_e) / std::pow(2.0, 28) * kCc1101XoscHz;
}

uint8_t cc1101_calc_rxbw_mdmcfg4_high(double bw_hz) {
    // BW = f_xosc / (8 * (4 + CHANBW_M) * 2^CHANBW_E). Pick the (E,M) whose BW is
    // >= target and closest (datasheet 13); both fields are 2 bits.
    int best_e = 0, best_m = 0;
    double best_bw = 1e18;
    for (int e = 0; e < 4; e++) {
        for (int m = 0; m < 4; m++) {
            double bw = kCc1101XoscHz / (8.0 * (4 + m) * std::pow(2.0, e));
            // Prefer the smallest bandwidth that still covers the request.
            if (bw >= bw_hz && bw < best_bw) {
                best_bw = bw;
                best_e = e;
                best_m = m;
            }
        }
    }
    if (best_bw > 1e17) { // nothing >= target: fall back to the widest (E=0,M=0)
        best_e = 0;
        best_m = 0;
    }
    return static_cast<uint8_t>((best_e << 6) | (best_m << 4));
}

double cc1101_rxbw_from_mdmcfg4_high(uint8_t high) {
    int e = (high >> 6) & 0x03;
    int m = (high >> 4) & 0x03;
    return kCc1101XoscHz / (8.0 * (4 + m) * std::pow(2.0, e));
}

uint8_t cc1101_calc_deviation(double dev_hz) {
    // dev = f_xosc / 2^17 * (8 + DEVIATION_M) * 2^DEVIATION_E (datasheet 16).
    // DEVIATION_M is 3 bits, DEVIATION_E is 3 bits.
    int best_e = 0, best_m = 0;
    double best_err = 1e18;
    for (int e = 0; e < 8; e++) {
        for (int m = 0; m < 8; m++) {
            double d = kCc1101XoscHz / std::pow(2.0, 17) * (8 + m) * std::pow(2.0, e);
            double err = std::fabs(d - dev_hz);
            if (err < best_err) {
                best_err = err;
                best_e = e;
                best_m = m;
            }
        }
    }
    return static_cast<uint8_t>((best_e << 4) | best_m);
}

double cc1101_deviation_from_reg(uint8_t reg) {
    int e = (reg >> 4) & 0x07;
    int m = reg & 0x07;
    return kCc1101XoscHz / std::pow(2.0, 17) * (8 + m) * std::pow(2.0, e);
}

} // namespace rf
} // namespace platform
