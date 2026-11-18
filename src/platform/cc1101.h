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

// A CC1101 reports VERSION as one of these known silicon revisions. A floating
// or absent MISO reads 0x00/0xFF, so the whitelist rejects a missing chip.
constexpr bool cc1101_version_valid(uint8_t v) {
    return v == 0x04 || v == 0x14 || v == 0x17;
}

// Low-level CC1101 probe primitive, parameterized by the chip-select GPIO so
// the CAP auto-detector (M3) can reuse it on whichever CS a given CAP wires.
//   cs_gpio == 7  -> G7 = SPI0_CE1 = /dev/spidev0.1 hardware CS (kernel drives
//                    CS; e.g. this is where the CC1101 CAP's ST25R lives, not
//                    the CC1101 - see the CAP profiles).
//   cs_gpio != 7  -> open the node with SPI_NO_CS and bit-bang that GPIO as CS
//                    (so CE0/G8 = the internal LCD never glitches). The CC1101
//                    CAP wires CC1101_CS = G22; Pingequa wires it to G15.
//   rst_gpio >= 0 -> drive that GPIO high first to release the module's reset
//                    (per-module RST; the known CAPs wire it to G26). -1 = none.
// Strobes SRES, reads VERSION(0x31) with the burst header, stores it in
// `version`, and returns true only when `cc1101_version_valid(version)`.
bool probe_cc1101(const char* spidev_path, int cs_gpio, int rst_gpio, uint8_t& version);

// Probe the M5 CAP CC1101 over spidev and return a human-readable report. Pins
// come from the BruceModule registry (default profile: CAP CC1101 -> CC1101 on
// G22 bit-banged). `detected` is set true only when a CC1101 answered.
std::string cc1101_detect_report(const std::string& spidev_path, bool& detected);

// Entry point for the app's `--cc1101-detect [spidev]` subcommand. `argc`/`argv`
// are the args AFTER the flag; argv[0], if present, overrides the spidev path
// (else $CC1101_SPIDEV, else the APP_CC1101_SPIDEV compile default). Prints the
// report to stdout and returns 0 when a CC1101 responded, 1 otherwise.
int cc1101_detect_main(int argc, char** argv);

// --- ST25R3916 (NFC/RFID half of the M5 RF+NFC CAP) ---------------------------

// SPI header byte to READ an ST25R3916 register in space A (0x00-0x3F): the top
// two mode bits must be 0b01, i.e. addr | 0x40. IC_IDENTITY lives at 0x3F, so
// the identity read header is 0x7F. constexpr so the self-check can pin framing
// without hardware. (Space-B registers, not needed for detection, use a 0xFB
// prefix - deliberately out of scope here.)
constexpr uint8_t st25r_reg_read_header(uint8_t addr) {
    return static_cast<uint8_t>((addr & 0x3F) | 0x40);
}

// Low-level ST25R3916 probe primitive, parameterized by the chip-select GPIO.
//   cs_gpio == 7  -> G7 = SPI0_CE1 = /dev/spidev0.1 hardware CS. The CC1101 CAP
//                    wires ST25R3916_CS = G7, so this is the normal path.
//   cs_gpio != 7  -> SPI_NO_CS + bit-bang that GPIO (kept for a differently
//                    wired CAP). SPI mode 1 either way.
//   rst_gpio >= 0 -> drive high first to release the module reset (-1 = none).
// Reads IC_IDENTITY(0x3F) into `ic_identity`; returns true only when the ic_type
// field matches ST25R3916 (0x28) or ST25R3916B (0x30).
bool probe_st25r(const char* spidev_path, int cs_gpio, int rst_gpio, uint8_t& ic_identity);

// Low-level SX1262 (LoRa) presence probe. The CAP LORA wires SX1262_CS = G7 (HW
// CS), BUSY = G22, RST = G26. SPI mode 0 (distinguishes it from the ST25R, which
// is mode 1 on the same G7). The SX126x has no IC_IDENTITY register, so presence
// is confirmed by a read/write scratch test on the LoRa sync-word register
// (0x0740): we pulse RST, wait BUSY low, write two patterns and read them back.
//   busy_gpio >= 0 -> poll this line low before each SPI op (-1 = fixed delay).
//   rst_gpio  >= 0 -> pulse this line low->high to reset the chip (-1 = none).
// `scratch` returns the last register readback and `status` the GetStatus byte
// (both for diagnostics/logging). Returns true when the scratch test matched.
bool probe_sx1262(const char* spidev_path, int cs_gpio, int busy_gpio, int rst_gpio,
                  uint16_t& scratch, uint8_t& status);

// Low-level nRF24L01(+) presence probe. Pingequa wires nRF24_CS = G22, CE = G23.
// SPI mode 0, CS active-low (bit-banged). The nRF24 has no whoami register, so
// presence is confirmed by a scratch write/read of the 5-byte TX_ADDR register
// (0x10): write a pattern with W_REGISTER (0x20|addr), read it back with
// R_REGISTER (addr). `status` returns the STATUS byte and `config` the CONFIG
// register (0x00), both for diagnostics. CE is held low (standby) if ce_gpio>=0.
// Returns true when the TX_ADDR scratch readback matched.
bool probe_nrf24(const char* spidev_path, int cs_gpio, int ce_gpio,
                 uint8_t& status, uint8_t& config);

// Probe the M5 CAP ST25R3916 over spidev and return a human-readable report.
// Pins come from the BruceModule registry (default profile: CAP CC1101 ->
// ST25R3916 on G7 hardware CS). `detected` is read-only (static ID register).
std::string st25r_detect_report(const std::string& spidev_path, bool& detected);

// Entry point for the app's `--st25r-detect [spidev]` subcommand. Same arg rules
// as cc1101_detect_main but with $ST25R_SPIDEV / APP_ST25R_SPIDEV. Returns 0 when
// an ST25R3916 responded, 1 otherwise.
int st25r_detect_main(int argc, char** argv);

} // namespace platform
