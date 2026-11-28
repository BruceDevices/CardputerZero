/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Lightweight native presence probe for an NXP PN532 NFC controller wired on
 * I2C (0x24) or SPI. It issues the PN532 GetFirmwareVersion command (frame
 * 00 00 FF 02 FE D4 02 2A 00) and checks for the 00 00 FF 00 FF 00 ACK, which
 * is enough to decide whether the PN532 rows should be shown in the RFID menu.
 *
 * Reading tags on the PN532 is still done through libnfc (nfc-list / nfc-poll /
 * nfc-mfclassic, the existing menu rows); this file only answers "is a PN532
 * here, and on which bus?" without needing libnfc installed.
 *
 * SPI note: the PN532 talks LSB-first, which RPi spidev does not do in hardware,
 * so each byte is bit-reversed in software (mode 0, MSB-first on the wire). The
 * SPI probe defaults to /dev/spidev0.0 (CE0) so it never disturbs the CAP's
 * ST25R on spidev0.1; override with $PN532_SPIDEV.
 */

#pragma once

#include <string>

namespace platform {

// True if a PN532 answers GetFirmwareVersion on I2C (0x24) or SPI. When it does
// and `where` is non-null, `*where` is set to a short bus tag ("I2C /dev/i2c-1"
// or "SPI /dev/spidev0.0"). Opens and closes its own handles; false off Linux.
bool pn532_present(std::string* where = nullptr);

int pn532_detect_main(int argc, char** argv); // --pn532-detect [i2c|spi|<dev>]

} // namespace platform
