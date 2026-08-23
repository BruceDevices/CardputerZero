/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstdint>
#include <string>

namespace platform {

// SPI header byte to READ a CC1101 status register (0x30-0x3D). Those addresses
// alias command strobes unless the burst bit is set, so the header must be
// addr | READ(0x80) | BURST(0x40) = addr | 0xC0. Reading VERSION without the
// burst bit fires a strobe instead and returns garbage - the classic trap.
// constexpr so the self-check can pin the framing without hardware.
constexpr uint8_t cc1101_status_read_header(uint8_t addr) {
    return static_cast<uint8_t>(addr | 0xC0);
}

// Probe the M5 CAP CC1101 over spidev and return a human-readable report.
// `detected` is set true only when a CC1101 actually answered (plausible
// VERSION register). No GPIO is needed for detection - the spidev native CS
// drives the chip select; GDO0/CS lines only matter for RX/TX.
std::string cc1101_detect_report(const std::string& spidev_path, bool& detected);

// Entry point for the app's `--cc1101-detect [spidev]` subcommand. `argc`/`argv`
// are the args AFTER the flag; argv[0], if present, overrides the spidev path
// (else $CC1101_SPIDEV, else the APP_CC1101_SPIDEV compile default). Prints the
// report to stdout and returns 0 when a CC1101 responded, 1 otherwise.
int cc1101_detect_main(int argc, char** argv);

} // namespace platform
