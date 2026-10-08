/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Native ISO14443A (NFC-A) RFID driver for the M5 RF+NFC CAP's ST25R3916 over
 * spidev. Register addresses, direct-command opcodes and bitfields are taken
 * verbatim from ST's ST25R3916 definitions (lewisxhe/ST25R3916-fork, the same
 * source the ESP32 Bruce firmware builds on), so the framing is authoritative.
 *
 * Feature parity with the firmware's RFID module for the ST25R chip: read a
 * tag's UID/ATQA/SAK, dump a Type-2 tag, write a Type-2 block, and emulate a
 * Type-2 tag with a chosen UID (NFC-A passive-target / listen mode, as the
 * firmware does - the "card emulation not wired" note in its header is stale).
 *
 * The ISO14443A protocol logic here (CRC_A, BCC, anticollision cascade, SAK
 * decoding, hex parsing) is covered by tests/rfid_selfcheck.cpp. The RF
 * front-end bring-up values are sensible defaults exposed as a calibration knob
 * (see kTxDriverResistance / analog init in the .cpp): real antennas vary, so
 * expect to tune them on the device the first time.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace platform {

// ---- Pure ISO14443A protocol helpers (no hardware; unit-tested) -------------

// ISO14443-3 CRC_A over `data` (poly x^16+x^12+x^5+1, init 0x6363), little-endian
// result appended as {lo, hi}.
void iso14443a_crc(const uint8_t* data, size_t len, uint8_t out_crc[2]);

// Block Check Character for a 4-byte anticollision chunk: XOR of all four bytes.
uint8_t anticoll_bcc(const uint8_t id[4]);

// Human-readable tag type guessed from a SAK byte (ISO14443-3/-4 coding).
std::string sak_describe(uint8_t sak);

// Parse a hex string ("04A1B2" or "04:a1:b2") into bytes. Returns false on any
// non-hex / odd-length input.
bool parse_hex(const std::string& text, std::vector<uint8_t>& out);

// Format bytes as uppercase hex, optionally colon-separated.
std::string to_hex(const uint8_t* data, size_t len, bool colons = false);

// ---- Subcommand entry points (re-exec'd by the menu) ------------------------
//
// Each takes the args AFTER the flag; argv[0] may override the spidev path
// (else $ST25R_SPIDEV, else APP_ST25R_SPIDEV). All print a human-readable
// report and return 0 on success, non-zero otherwise.

int rfid_read_main(int argc, char** argv);           // --rfid-read [spidev]
int rfid_dump_main(int argc, char** argv);            // --rfid-dump [spidev]
int rfid_write_main(int argc, char** argv);           // --rfid-write <page> <hex4> [spidev]
int rfid_emulate_main(int argc, char** argv);         // --rfid-emulate <uid-hex> [spidev]
int rfid_clone_main(int argc, char** argv);           // --rfid-clone [spidev] (read UID, then emulate it)

} // namespace platform
