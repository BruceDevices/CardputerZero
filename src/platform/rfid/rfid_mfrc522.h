/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Native ISO14443A reader driver for the M5Stack RFID2 unit (NXP MFRC522 /
 * Fudan WS1850S) over Linux i2c-dev. Register map, command set and the
 * REQA/anticollision/SELECT and MIFARE auth/read/write sequences are the
 * standard MFRC522 flows (as implemented by OSSLibraries/Arduino_MFRC522v2,
 * which the ESP32 Bruce firmware's RFID2 driver builds on), reimplemented here
 * directly on i2c-dev with no Arduino layer.
 *
 * Feature parity with the firmware's RFID2 module: read a tag's UID/ATQA/SAK,
 * dump MIFARE Ultralight / NTAG (Type-2) pages and MIFARE Classic 1K/4K blocks
 * (authenticating each sector with a key dictionary), and write a Classic block
 * or an Ultralight/NTAG page. Crypto1 authentication runs in the MFRC522's own
 * hardware (PCD_MFAuthent), so no software cipher is needed for Classic here.
 *
 * The MFRC522 is a reader only (no card emulation / listen mode), matching the
 * firmware's RFID2 driver which likewise implements read/write but not emulate.
 */

#pragma once

namespace platform {

// Subcommand entry points (re-exec'd by the menu). Each takes the args AFTER
// the flag; argv[0] may override the transport ("i2c", "/dev/i2c-1",
// "/dev/i2c-1@0x28"), else $RFID2_I2C / auto-scan. All print a human-readable
// report and return 0 on success, non-zero otherwise.

// Quick presence probe: scan every I2C bus for an MFRC522/WS1850S at 0x28 (or
// $RFID2_I2C_ADDR) and confirm its VersionReg. Opens/closes its own handle;
// returns false off Linux. Used by the menu to show the RFID2 rows only when
// the reader is actually connected.
bool rfid2_present();

int rfid2_detect_main(int argc, char** argv);  // --rfid2-detect [spec]
int rfid2_read_main(int argc, char** argv);    // --rfid2-read   [spec]
int rfid2_dump_main(int argc, char** argv);    // --rfid2-dump   [spec]
int rfid2_write_main(int argc, char** argv);   // --rfid2-write <block> <hex16|hex4> [spec]

} // namespace platform
