/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Native ISO14443A (NFC-A) RFID driver for the ST25R3916: the M5 RF+NFC CAP
 * over spidev, or an I2C NFC unit at 0x50 over i2c-dev (same chip, same mode
 * bytes; the transport is auto-detected SPI-first). Register addresses,
 * direct-command opcodes and bitfields are taken
 * verbatim from ST's ST25R3916 definitions (lewisxhe/ST25R3916-fork, the same
 * source the ESP32 Bruce firmware builds on), so the framing is authoritative.
 *
 * Supports UID/ATQA/SAK reads, Type-2 page reads and writes, and NFC-A passive
 * target emulation with Type-2 page responses.
 *
 * The ISO14443A protocol logic here (CRC_A, BCC, anticollision cascade, SAK
 * decoding, hex parsing) is covered by tests/rfid_selfcheck.cpp. The RF
 * front-end bring-up uses the antenna and passive load modulation settings in
 * rfid_st25r.cpp.
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
// Each takes the args AFTER the flag; argv[0] (the spec arg below, after the
// positional args) may override the transport, else $ST25R_BUS / $ST25R_SPIDEV
// steer it, else full auto-detect. The spec is one of:
//   "auto"/""             - try SPI (the CAP CC1101's ST25R), then scan every
//                           I2C bus at 0x50
//   "spi" / "/dev/spidevX.Y" - force SPI (default node, or that spidev path)
//   "i2c"                 - scan all I2C buses at 0x50
//   "i2c:/dev/i2c-1"      - I2C on that bus at 0x50
//   "i2c:/dev/i2c-1@0x50" - I2C on that bus at that address
// All print a human-readable report and return 0 on success, non-zero otherwise.

// Quick, side-effect-light presence probe for an ST25R3916 wired on I2C at 0x50
// (an external NFC unit, not the CAP). Scans every I2C bus and reads
// IC_IDENTITY. The SPI/CAP case is reported by hal::has_st25r() instead, so the
// menu ORs the two. Returns false off Linux. Opens and closes its own handle.
bool st25r_present_i2c();

// ---- In-process reader session (for the Tag-O-Matic-style RfidScreen) -------
//
// The CLI entry points below print a one-shot report and exit; the on-device
// RFID screen instead drives the chip continuously from the LVGL timer, so it
// needs structured results rather than printed text. These three functions are
// that in-process surface: open the transport once, poll in a loop (each poll
// energizes the field, does REQA + anticollision, and optionally dumps the
// Type-2 pages), and close when leaving the screen. All are Linux-only no-ops
// elsewhere. Not thread-safe: call them only from the UI thread, like the
// screen does, and never while a --rfid-* CLI child is also touching the bus.

// One tag seen by a poll. `uid`/`atqa`/`sak` are the ISO14443A identity; `t2t`
// is true for a Type-2 tag (SAK 0x00, e.g. NTAG/Ultralight). When a dump was
// requested and the tag is Type-2, `pages` holds `page_count` 4-byte pages read
// from page 0 upward (so byte offset = page*4).
struct St25rScan {
    std::vector<uint8_t> uid;
    uint8_t atqa[2] = {0, 0};
    uint8_t sak = 0;
    std::string sak_text;
    bool t2t = false;
    std::vector<uint8_t> pages;
    unsigned page_count = 0;
};

// Open the ST25R reader (ST25R_BUS override, else SPI CAP then I2C @0x50). When
// `report` is non-null it receives the same human-readable bring-up text the
// CLI prints. Returns false (and leaves nothing open) if no chip answered.
bool st25r_session_open(std::string* report = nullptr);

// True while a session opened by st25r_session_open() is live.
bool st25r_session_ready();

// Energize the field and try to read one tag. Returns true and fills `out` when
// a tag answered; false on an empty field / anticollision miss. When
// `dump_pages` is true and the tag is Type-2, `out.pages` is also populated.
bool st25r_session_poll(St25rScan& out, bool dump_pages);

// Release the transport opened by st25r_session_open() (turns the field off).
void st25r_session_close();

// In-process tag emulation (Clone). begin() starts a listener on the open
// session. step() consumes one pending reader selection without accessing the
// bus. st25r_session_close() joins the listener and releases the session.
bool st25r_emulate_begin(const St25rScan& scan);
bool st25r_emulate_step();

// Write a read tag to a Flipper/Bruce-compatible `.nfc` file in `dir` (created
// if needed; a numeric suffix avoids clobbering an existing UID). Returns the
// saved path, or "" on failure. Includes the Type-2 data pages when the scan
// carries them. Portable (plain file IO); no hardware access.
std::string st25r_save_nfc(const St25rScan& scan, const std::string& dir);

// Load NFC-A identity and contiguous Type-2 pages from a Bruce/Flipper file.
bool st25r_load_nfc(const std::string& path, St25rScan& scan, std::string& error);

enum class St25rWriteResult { WaitingForTag, Written, Failed };

// Copy the stored Type-2 user pages to a presented writable tag. The report
// includes the number of pages already written if the operation fails.
St25rWriteResult st25r_session_write_tag(const St25rScan& scan, std::string& report);

int rfid_read_main(int argc, char** argv);           // --rfid-read [spidev]
int rfid_dump_main(int argc, char** argv);            // --rfid-dump [spidev]
int rfid_write_main(int argc, char** argv);           // --rfid-write <page> <hex4> [spidev]
int rfid_emulate_main(int argc, char** argv);         // --rfid-emulate <uid-hex> [bus]
int rfid_clone_main(int argc, char** argv);           // --rfid-clone [spidev] (read UID, then emulate it)

} // namespace platform
