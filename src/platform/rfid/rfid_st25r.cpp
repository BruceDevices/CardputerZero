/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * ST25R3916 NFC-A driver over spidev. Constants and SPI framing are taken from
 * ST's ST25R3916 definitions (lewisxhe/ST25R3916-fork). The SPI access is MODE1
 * / MSB-first; register reads auto-increment, so the four IRQ registers
 * (0x1A-0x1D) are read as one 32-bit word matching ST25R3916_IRQ_MASK_*.
 *
 * Protocol logic is tested in tests/rfid_selfcheck.cpp. The RF front-end
 * bring-up (kAnalog* below) is a sensible default, not ST's per-antenna analog
 * config table; treat it as a calibration knob and expect to tune it on the
 * device the first time.
 */

#include "rfid_st25r.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "cap_bringup.h"
#include "rfid_i2c.h"
#endif

#ifndef APP_ST25R_SPIDEV
#define APP_ST25R_SPIDEV "/dev/spidev0.1"
#endif

namespace platform {

// ===== Pure ISO14443A protocol helpers (unit-tested, no hardware) ============

void iso14443a_crc(const uint8_t* data, size_t len, uint8_t out_crc[2]) {
    // ISO14443-3 Type A CRC: polynomial 0x8408 (reflected), preset 0x6363.
    uint32_t crc = 0x6363;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i] ^ static_cast<uint8_t>(crc & 0xFF);
        b ^= static_cast<uint8_t>(b << 4);
        crc = (crc >> 8) ^ (static_cast<uint32_t>(b) << 8) ^ (static_cast<uint32_t>(b) << 3) ^
              (static_cast<uint32_t>(b) >> 4);
    }
    out_crc[0] = static_cast<uint8_t>(crc & 0xFF);
    out_crc[1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
}

uint8_t anticoll_bcc(const uint8_t id[4]) {
    return static_cast<uint8_t>(id[0] ^ id[1] ^ id[2] ^ id[3]);
}

std::string sak_describe(uint8_t sak) {
    if (sak & 0x04) {
        return "UID not complete (cascade)";
    }
    if (sak & 0x20) {
        if (sak == 0x20) return "ISO14443-4 (e.g. DESFire / bank card)";
        if (sak == 0x28) return "ISO14443-4 + legacy (SmartMX)";
        return "ISO14443-4 capable";
    }
    switch (sak) {
        case 0x00: return "MIFARE Ultralight / NTAG (Type 2)";
        case 0x08: return "MIFARE Classic 1K";
        case 0x09: return "MIFARE Mini";
        case 0x18: return "MIFARE Classic 4K";
        case 0x10: return "MIFARE Plus 2K";
        case 0x11: return "MIFARE Plus 4K";
        default:   return "unknown / proprietary";
    }
}

bool parse_hex(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    std::string clean;
    for (char c : text) {
        if (c == ':' || c == ' ' || c == '-') continue;
        clean.push_back(c);
    }
    if (clean.empty() || (clean.size() % 2) != 0) {
        return false;
    }
    auto nib = [](char c, int& v) {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') { v = 10 + (c - 'a'); return true; }
        return false;
    };
    for (size_t i = 0; i < clean.size(); i += 2) {
        int hi = 0, lo = 0;
        if (!nib(clean[i], hi) || !nib(clean[i + 1], lo)) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string to_hex(const uint8_t* data, size_t len, bool colons) {
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < len; ++i) {
        if (colons && i) s.push_back(':');
        s.push_back(digits[data[i] >> 4]);
        s.push_back(digits[data[i] & 0x0F]);
    }
    return s;
}

// ===== Hardware: ST25R3916 register/command constants ========================
namespace {

// Direct commands (sent as a single byte; already carry the 0b11 prefix).
constexpr uint8_t CMD_SET_DEFAULT        = 0xC1;
constexpr uint8_t CMD_STOP               = 0xC2;
constexpr uint8_t CMD_TX_WITH_CRC        = 0xC4;
constexpr uint8_t CMD_TX_WITHOUT_CRC     = 0xC5;
constexpr uint8_t CMD_TX_REQA            = 0xC6;
constexpr uint8_t CMD_TX_WUPA            = 0xC7;
constexpr uint8_t CMD_ADJUST_REGULATORS  = 0xD6;
constexpr uint8_t CMD_CLEAR_FIFO         = 0xDB;
constexpr uint8_t CMD_GOTO_SENSE         = 0xCD; // passive target -> Sense/Idle (listen)
constexpr uint8_t CMD_GOTO_SLEEP         = 0xCE; // passive target -> Sleep/Halt

// Registers (space A).
constexpr uint8_t REG_OP_CONTROL     = 0x02;
constexpr uint8_t REG_MODE           = 0x03;
constexpr uint8_t REG_BIT_RATE       = 0x04;
constexpr uint8_t REG_ISO14443A_NFC  = 0x05;
constexpr uint8_t REG_PASSIVE_TARGET = 0x08;
constexpr uint8_t REG_AUX            = 0x0A;
constexpr uint8_t REG_MASK_RX_TIMER  = 0x0F;
constexpr uint8_t REG_NO_RESP_TIMER1 = 0x10;
constexpr uint8_t REG_NO_RESP_TIMER2 = 0x11;
constexpr uint8_t REG_TIMER_EMV_CTRL = 0x12;
constexpr uint8_t REG_IRQ_MAIN       = 0x1A; // 0x1A..0x1D auto-increment on read
constexpr uint8_t REG_FIFO_STATUS1   = 0x1E;
constexpr uint8_t REG_FIFO_STATUS2   = 0x1F;
constexpr uint8_t REG_NUM_TX_BYTES1  = 0x22;
constexpr uint8_t REG_NUM_TX_BYTES2  = 0x23;
constexpr uint8_t REG_TX_DRIVER      = 0x28;
constexpr uint8_t REG_AUX_DISPLAY    = 0x31; // osc/field status (OSC_OK on bit 4)
constexpr uint8_t REG_IC_IDENTITY    = 0x3F;

// Register bit fields.
constexpr uint8_t OP_CONTROL_EN         = 1 << 7;
constexpr uint8_t OP_CONTROL_RX_EN      = 1 << 6;
constexpr uint8_t OP_CONTROL_TX_EN      = 1 << 3;
constexpr uint8_t OP_CONTROL_EN_FD_AUTO = 3 << 0;
constexpr uint8_t MODE_TARG_INIT        = 0 << 7;
constexpr uint8_t MODE_TARG_TARG        = 1 << 7;
constexpr uint8_t MODE_OM_ISO14443A     = 0x1 << 3;
constexpr uint8_t MODE_OM_TARG_NFCA     = 0x1 << 3;
constexpr uint8_t AUX_NO_CRC_RX         = 1 << 7;
constexpr uint8_t FIFO_STATUS2_B_MASK   = 3 << 6;
constexpr uint8_t AUX_DISPLAY_OSC_OK    = 1 << 4; // crystal oscillator stable
constexpr uint8_t AUX_NFC_ID_7BYTES     = 1 << 4; // REG_AUX: UID length for auto-AC
// PASSIVE_TARGET register bits.
constexpr uint8_t PT_D_106_AC_A         = 1 << 0; // 1 = manual data (disable auto-AC)
constexpr uint8_t PT_D_212_424_1R       = 1 << 2; // disable NFC-F 212/424 single-slot
constexpr uint8_t PT_D_AC_AP2P          = 1 << 3; // disable active P2P
constexpr uint8_t PT_FDEL_0             = 1 << 4;
constexpr uint8_t PT_FDEL_2             = 1 << 6;
// Listen-A config (Bruce ST25R3916::_setupListenMode): fdel=5, NFC-F/AP2P off,
// auto NFC-A anticollision enabled (d_106_ac_a = 0). After selection the code
// sets d_106_ac_a to take over the data exchange by hand.
constexpr uint8_t PASSIVE_TARGET_LISTEN_A =
    PT_FDEL_2 | PT_FDEL_0 | PT_D_AC_AP2P | PT_D_212_424_1R; // 0x5C

// IRQ bits (32-bit map across REG_IRQ_MAIN..IRQ_TARGET).
constexpr uint32_t IRQ_RXS = 0x00000020;
constexpr uint32_t IRQ_RXE = 0x00000010;
constexpr uint32_t IRQ_TXE = 0x00000008;
constexpr uint32_t IRQ_COL = 0x00000004;
constexpr uint32_t IRQ_NRE = 0x00004000;
constexpr uint32_t IRQ_CRC = 0x00800000;
constexpr uint32_t IRQ_PAR = 0x00400000;
constexpr uint32_t IRQ_ERR2 = 0x00200000;
constexpr uint32_t IRQ_ERR1 = 0x00100000;
constexpr uint32_t IRQ_ERRORS = IRQ_CRC | IRQ_PAR | IRQ_ERR2 | IRQ_ERR1;
constexpr uint32_t IRQ_WU_A = 0x01000000;   // passive target: Active reached (selected)
constexpr uint32_t IRQ_WU_AX = 0x02000000;  // passive target: Active* reached
constexpr uint32_t IRQ_EOF = 0x00000800;    // external field off (reader left)

// SPI mode bytes.
constexpr uint8_t SPI_READ_MODE  = 0x40;
constexpr uint8_t SPI_WRITE_MODE = 0x00;
constexpr uint8_t SPI_FIFO_LOAD  = 0x80;
constexpr uint8_t SPI_FIFO_READ  = 0x9F;
constexpr uint8_t SPI_PT_LOAD_A  = 0xA0; // load the NFC-A passive-target memory

// ST25R3916 IC_IDENTITY ic_type values (ic_type field = id & 0xF8).
constexpr uint8_t IC_TYPE_MASK   = 0xF8;
constexpr uint8_t IC_TYPE_3916   = 0x28; // 5 << 3
constexpr uint8_t IC_TYPE_3916B  = 0x30; // 6 << 3

// Calibration knob: TX driver resistance (0 = strongest). ST's per-antenna
// analog config lives in a big table we do not vendor; this and the init below
// are a reasonable starting point to tune on the device.
// ponytail: naive analog bring-up; replace with ST analog-config table if reads
// are flaky on your antenna.
constexpr uint8_t kAnalogTxDriver = 0x00;

// The ST25R3916's default 7-bit I2C address (0xA0 >> 1); same chip, same mode
// bytes as SPI, so the transport below just swaps the physical link.
constexpr uint8_t kI2cAddr = 0x50;

#if defined(__linux__)
enum class Bus { Spi, I2c };

Bus g_bus = Bus::Spi;
int g_fd = -1;         // spidev fd (SPI) or i2c-dev fd (I2C)
int g_cs_fd = -1;      // SPI only: GPIO line handle for the cap's NFC_CS (active low)
unsigned g_cs_line = 22;
uint8_t g_i2c_addr = kI2cAddr;

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && v[0] != '\0') ? v : fallback;
}

// CS framing: ST25R3916 latches the access per chip-select frame, and each
// spi_xfer() is exactly one access. tCSN setup is nanoseconds at 2 MHz; the
// ioctl round-trip provides ample slack, no delay needed.
void cs_set(bool high) {
    if (g_cs_fd < 0) return;
    gpiohandle_data data{};
    data.values[0] = high ? 1 : 0;
    ioctl(g_cs_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

void sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

bool spi_xfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<uintptr_t>(tx);
    tr.rx_buf = reinterpret_cast<uintptr_t>(rx);
    tr.len = static_cast<uint32_t>(len);
    tr.speed_hz = 2000000; // 2 MHz - conservative for ST25R3916 (max ~6 MHz)
    tr.bits_per_word = 8;
    cs_set(false);
    const bool ok = ioctl(g_fd, SPI_IOC_MESSAGE(1), &tr) >= 0;
    cs_set(true);
    return ok;
}

// ---- Transport primitives (SPI frame OR I2C transaction) --------------------
//
// The ST25R3916 uses identical mode bytes on both links (READ=0x40|addr,
// WRITE=0x00|addr, FIFO_LOAD=0x80, FIFO_READ=0x9F; register reads
// auto-increment). On SPI an access is one CS-framed full-duplex transfer; on
// I2C it is a write then (for reads) a repeated-start read. These two helpers
// hide that difference; everything above the FIFO/register layer is bus-blind.

// Send `whdr`+`wbody` (wn bytes) and, when rn>0, read rn bytes back.
void bus_write_then_read(const uint8_t* wbuf, size_t wn, uint8_t* rbuf, size_t rn) {
    if (g_bus == Bus::I2c) {
        if (rn == 0) {
            i2c_write(g_fd, wbuf, wn);
        } else {
            i2c_write_read(g_fd, g_i2c_addr, wbuf, wn, rbuf, rn);
        }
        return;
    }
    // SPI: one full-duplex frame of wn + rn bytes; the first wn carry the
    // header/data, the trailing rn clock out the response (tx stuffed 0x00).
    uint8_t tx[1 + 256] = {0};
    uint8_t rx[1 + 256] = {0};
    const size_t total = wn + rn;
    if (total > sizeof(tx)) return;
    std::memcpy(tx, wbuf, wn);
    spi_xfer(tx, rx, total);
    if (rn) std::memcpy(rbuf, &rx[wn], rn);
}

uint8_t reg_read(uint8_t addr) {
    uint8_t hdr = static_cast<uint8_t>(SPI_READ_MODE | addr);
    uint8_t v = 0;
    bus_write_then_read(&hdr, 1, &v, 1);
    return v;
}

void reg_write(uint8_t addr, uint8_t val) {
    uint8_t w[2] = {static_cast<uint8_t>(SPI_WRITE_MODE | addr), val};
    bus_write_then_read(w, 2, nullptr, 0);
}

void cmd(uint8_t c) {
    bus_write_then_read(&c, 1, nullptr, 0);
}

void reg_set_bits(uint8_t addr, uint8_t bits) {
    reg_write(addr, static_cast<uint8_t>(reg_read(addr) | bits));
}

void reg_clr_bits(uint8_t addr, uint8_t bits) {
    reg_write(addr, static_cast<uint8_t>(reg_read(addr) & ~bits));
}

void fifo_write(const uint8_t* data, size_t n) {
    uint8_t w[1 + 256];
    if (n > 255) n = 255;
    w[0] = SPI_FIFO_LOAD;
    std::memcpy(&w[1], data, n);
    bus_write_then_read(w, n + 1, nullptr, 0);
}

size_t fifo_count() {
    const uint8_t hi = (reg_read(REG_FIFO_STATUS2) & FIFO_STATUS2_B_MASK) >> 6;
    const uint8_t lo = reg_read(REG_FIFO_STATUS1);
    return (static_cast<size_t>(hi) << 8) | lo;
}

size_t fifo_read(uint8_t* out, size_t cap) {
    size_t n = fifo_count();
    if (n > cap) n = cap;
    const size_t clamped = (n > 255) ? 255 : n;
    uint8_t hdr = SPI_FIFO_READ;
    bus_write_then_read(&hdr, 1, out, clamped);
    return clamped;
}

// Read the four consecutive IRQ registers in one transaction (clears them).
uint32_t read_irqs() {
    uint8_t hdr = static_cast<uint8_t>(SPI_READ_MODE | REG_IRQ_MAIN);
    uint8_t r[4] = {0, 0, 0, 0};
    bus_write_then_read(&hdr, 1, r, 4);
    return static_cast<uint32_t>(r[0]) | (static_cast<uint32_t>(r[1]) << 8) |
           (static_cast<uint32_t>(r[2]) << 16) | (static_cast<uint32_t>(r[3]) << 24);
}

// Poll the IRQ registers (no IRQ GPIO on the CAP) until any `mask` bit latches
// or `timeout_ms` elapses. Bits are accumulated because reading clears them.
uint32_t wait_irq(uint32_t mask, long timeout_ms) {
    uint32_t acc = 0;
    for (long t = 0; t < timeout_ms; ++t) {
        acc |= read_irqs();
        if (acc & mask) break;
        sleep_ms(1);
    }
    return acc;
}

void set_num_tx_bits(uint16_t bits) {
    reg_write(REG_NUM_TX_BYTES2, static_cast<uint8_t>(bits & 0xFF));
    reg_write(REG_NUM_TX_BYTES1, static_cast<uint8_t>((bits >> 8) & 0xFF));
}

// Bring the chip up as an ISO14443A (NFC-A) poller and turn the field on.
void poller_init() {
    cmd(CMD_SET_DEFAULT);
    sleep_ms(1);
    reg_write(REG_OP_CONTROL, OP_CONTROL_EN | OP_CONTROL_EN_FD_AUTO);
    cmd(CMD_ADJUST_REGULATORS);
    sleep_ms(1);
    reg_write(REG_MODE, MODE_TARG_INIT | MODE_OM_ISO14443A);
    reg_write(REG_BIT_RATE, 0x00);          // 106 kbit/s both ways
    reg_write(REG_ISO14443A_NFC, 0x00);     // parity on, standard framing
    reg_write(REG_TX_DRIVER, kAnalogTxDriver);
    // No-response timer: ~1 ms window so a missing tag times out quickly.
    reg_write(REG_TIMER_EMV_CTRL, 0x00);
    reg_write(REG_NO_RESP_TIMER1, 0x01);
    reg_write(REG_NO_RESP_TIMER2, 0x00);
    // Field on: enable TX; guard time for the tag to power up.
    reg_write(REG_OP_CONTROL, OP_CONTROL_EN | OP_CONTROL_RX_EN | OP_CONTROL_TX_EN | OP_CONTROL_EN_FD_AUTO);
    sleep_ms(6);
    read_irqs(); // clear anything latched during bring-up
}

void field_off() {
    reg_write(REG_OP_CONTROL, OP_CONTROL_EN);
    cmd(CMD_STOP);
}

// Transceive a frame and capture the response. `with_crc` appends/checks CRC in
// hardware. Returns the number of received bytes, or 0 on no-response/error.
size_t transceive(const uint8_t* tx, size_t txlen, bool with_crc, uint8_t* rx, size_t rxcap,
                  bool strip_crc) {
    read_irqs();
    cmd(CMD_CLEAR_FIFO);
    fifo_write(tx, txlen);
    set_num_tx_bits(static_cast<uint16_t>(txlen * 8));
    cmd(with_crc ? CMD_TX_WITH_CRC : CMD_TX_WITHOUT_CRC);

    const uint32_t irqs = wait_irq(IRQ_RXE | IRQ_NRE | IRQ_ERRORS, 20);
    if (!(irqs & IRQ_RXE) || (irqs & IRQ_NRE)) {
        return 0; // no response / timeout
    }
    size_t n = fifo_read(rx, rxcap);
    if (strip_crc && with_crc && n >= 2) {
        n -= 2; // drop the received CRC bytes
    }
    return n;
}

// REQA short frame via the dedicated command; returns ATQA (2 bytes) or false.
bool reqa(uint8_t atqa[2]) {
    read_irqs();
    cmd(CMD_CLEAR_FIFO);
    cmd(CMD_TX_REQA);
    const uint32_t irqs = wait_irq(IRQ_RXE | IRQ_NRE | IRQ_ERRORS, 20);
    if (!(irqs & IRQ_RXE)) {
        return false;
    }
    uint8_t buf[8] = {0};
    const size_t n = fifo_read(buf, sizeof(buf));
    if (n < 2) return false;
    atqa[0] = buf[0];
    atqa[1] = buf[1];
    return true;
}

// Full anticollision: fills uid (up to 10 bytes) and sak; returns uid length.
size_t anticollision(uint8_t* uid, size_t uidcap, uint8_t& sak) {
    static const uint8_t kSel[3] = {0x93, 0x95, 0x97}; // cascade levels 1..3
    size_t uidlen = 0;
    sak = 0;

    for (int level = 0; level < 3; ++level) {
        // Anticollision: SEL + NVB=0x20, no CRC, expect 5 bytes (4 id + BCC).
        uint8_t ac[2] = {kSel[level], 0x20};
        uint8_t resp[8] = {0};
        size_t n = transceive(ac, 2, /*crc*/ false, resp, sizeof(resp), /*strip*/ false);
        if (n < 5) {
            return 0; // anticollision failed
        }
        uint8_t cln[4] = {resp[0], resp[1], resp[2], resp[3]};
        if (anticoll_bcc(cln) != resp[4]) {
            return 0; // BCC mismatch
        }

        // SELECT: SEL + NVB=0x70 + CLn + BCC, with CRC, expect SAK (1 byte).
        uint8_t sel[7] = {kSel[level], 0x70, cln[0], cln[1], cln[2], cln[3], resp[4]};
        uint8_t sresp[4] = {0};
        n = transceive(sel, 7, /*crc*/ true, sresp, sizeof(sresp), /*strip*/ true);
        if (n < 1) {
            return 0;
        }
        sak = sresp[0];

        if (sak & 0x04) {
            // Cascade: first byte is CT (0x88); real UID bytes are cln[1..3].
            if (uidlen + 3 > uidcap) return 0;
            uid[uidlen++] = cln[1];
            uid[uidlen++] = cln[2];
            uid[uidlen++] = cln[3];
        } else {
            if (uidlen + 4 > uidcap) return 0;
            for (int i = 0; i < 4; ++i) uid[uidlen++] = cln[i];
            return uidlen; // UID complete
        }
    }
    return uidlen;
}

bool t2t_read_page(uint8_t page, uint8_t out16[16]) {
    uint8_t rdcmd[2] = {0x30, page};
    uint8_t resp[32] = {0};
    const size_t n = transceive(rdcmd, 2, true, resp, sizeof(resp), true);
    if (n < 16) return false;
    std::memcpy(out16, resp, 16); // T2T READ returns 16 bytes (4 pages)
    return true;
}

bool t2t_write_page(uint8_t page, const uint8_t in4[4]) {
    uint8_t wr[6] = {0xA2, page, in4[0], in4[1], in4[2], in4[3]};
    uint8_t ack[4] = {0};
    // T2T WRITE answers with a 4-bit ACK (0x0A); accept any short response.
    const size_t n = transceive(wr, 6, true, ack, sizeof(ack), false);
    return n >= 1 && (ack[0] & 0x0F) == 0x0A;
}

// SET_DEFAULT then read IC_IDENTITY; true when an ST25R3916(B) answers on the
// link currently open in g_bus/g_fd. Appends a one-line verdict to `report`.
bool probe_identity(std::string& report) {
    cmd(CMD_SET_DEFAULT);
    sleep_ms(1);
    const uint8_t id = reg_read(REG_IC_IDENTITY);
    const uint8_t ic = id & IC_TYPE_MASK;
    if (ic != IC_TYPE_3916 && ic != IC_TYPE_3916B) {
        char line[96];
        std::snprintf(line, sizeof(line), "  no ST25R3916 here (IC_IDENTITY=0x%02X)\n", id);
        report += line;
        return false;
    }
    report += std::string("ST25R3916") + (ic == IC_TYPE_3916B ? "B" : "") + " detected (" +
              (g_bus == Bus::I2c ? "I2C" : "SPI") + ").\n";
    return true;
}

// Open the ST25R3916 over spidev (MODE1, MSB-first) and verify it answers.
//
// The cap's NFC_CS is a plain GPIO on this board (G22, HAT_P0/P1 bus) - not a
// spidev chip select. The spidev node is therefore opened with SPI_NO_CS (so
// no internal CS - on this board they are the LCD's G8 or the CC1101's G7 -
// glitches during NFC traffic) and CS is bit-banged around every access.
bool try_open_spi(const std::string& path, std::string& report) {
    g_bus = Bus::Spi;
    g_cs_fd = -1;
    const std::string cs_chip = env_or("ST25R_CS_CHIP", "/dev/gpiochip0");
    g_cs_line = static_cast<unsigned>(std::atoi(env_or("ST25R_CS_GPIO", "22").c_str()));
    if (!gpio_claim_output(cs_chip.c_str(), g_cs_line, true, &g_cs_fd)) {
        report += "error: cannot claim NFC CS GPIO" + std::to_string(g_cs_line) + " on " +
                  cs_chip + " (" + std::strerror(errno) + ")\n";
        return false;
    }

    g_fd = open(path.c_str(), O_RDWR);
    if (g_fd < 0) {
        report += "error: cannot open spidev " + path + " (" + std::strerror(errno) + ")\n";
        if (errno == ENOENT) {
            report += "hint: SPI is off on this system. Run: sudo raspi-config nonint do_spi 0 "
                      "and reboot, then retry.\n";
        }
        return false;
    }
    uint8_t mode = SPI_MODE_1 | SPI_NO_CS; // CS is the GPIO above
    uint8_t bits = 8;
    uint32_t speed = 2000000;
    ioctl(g_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(g_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(g_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    report += "NFC CS: GPIO" + std::to_string(g_cs_line) + " (manual, SPI_NO_CS on " + path + ")\n";

    if (probe_identity(report)) return true;
    close(g_fd);
    g_fd = -1;
    return false;
}

// Open the ST25R3916 over i2c-dev at `addr` on `dev` and verify it answers.
bool try_open_i2c(const std::string& dev, uint8_t addr, std::string& report) {
    g_bus = Bus::I2c;
    g_i2c_addr = addr;
    g_fd = i2c_open(dev.c_str(), addr);
    if (g_fd < 0) {
        report += "error: cannot open " + dev + " (" + std::strerror(errno) + ")\n";
        return false;
    }
    char head[64];
    std::snprintf(head, sizeof(head), "NFC I2C: %s @ 0x%02X\n", dev.c_str(), addr);
    report += head;
    if (probe_identity(report)) return true;
    i2c_close(g_fd);
    g_fd = -1;
    return false;
}

// Resolve the transport from `spec` and bring the chip up. `spec` is one of:
//   ""/"auto"            - try SPI (cap CC1101), then scan every I2C bus at 0x50
//   "/dev/spidevX.Y"     - force SPI on that node
//   "spi"                - SPI on the picked spidev node
//   "i2c"                - scan I2C buses at 0x50
//   "i2c:/dev/i2c-1"     - I2C on that bus at 0x50
//   "i2c:/dev/i2c-1@0x50"- I2C on that bus at that address
bool open_chip(const std::string& spec, std::string& report) {
    report += cap_bringup().text;

    auto pick_spidev = []() {
        const char* env = std::getenv("ST25R_SPIDEV");
        if (env && env[0] != '\0') return std::string(env);
        const std::string p =
            spidev_pick({APP_ST25R_SPIDEV, "/dev/spidev0.1", "/dev/spidev0.0"});
        return p.empty() ? std::string(APP_ST25R_SPIDEV) : p;
    };
    auto scan_i2c = [&report](uint8_t addr) -> bool {
        for (const auto& bus : i2c_buses()) {
            std::string r;
            if (try_open_i2c(bus, addr, r)) {
                report += r;
                return true;
            }
            report += r;
        }
        return false;
    };

    std::string s = spec;
    if (s == "auto") s.clear();

    if (s.rfind("i2c", 0) == 0) {
        std::string dev = "/dev/i2c-1";
        uint8_t addr = kI2cAddr;
        const auto colon = s.find(':');
        if (colon != std::string::npos) {
            dev = s.substr(colon + 1);
            const auto at = dev.find('@');
            if (at != std::string::npos) {
                addr = static_cast<uint8_t>(std::strtol(dev.c_str() + at + 1, nullptr, 0));
                dev = dev.substr(0, at);
            }
        } else {
            return scan_i2c(addr); // "i2c" with no bus -> scan all
        }
        return try_open_i2c(dev, addr, report);
    }

    if (!s.empty() && s != "spi" && s.rfind("/dev/spidev", 0) == 0) {
        return try_open_spi(s, report); // explicit spidev path
    }
    if (s == "spi") {
        return try_open_spi(pick_spidev(), report);
    }

    // auto: SPI first (the CAP CC1101's ST25R), then scan I2C modules at 0x50.
    std::string spiReport;
    if (try_open_spi(pick_spidev(), spiReport)) {
        report += spiReport;
        return true;
    }
    report += spiReport;
    report += "SPI had no ST25R3916; scanning I2C buses at 0x50 ...\n";
    if (scan_i2c(kI2cAddr)) return true;
    report += "error: no ST25R3916 on SPI or I2C. Check the CAP/module seating, power and wiring.\n";
    return false;
}

void close_chip() {
    if (g_fd >= 0) {
        field_off();
        if (g_bus == Bus::I2c) {
            i2c_close(g_fd);
        } else {
            close(g_fd);
        }
        g_fd = -1;
    }
    // SPI uses a GPIO line handle for the CAP's manual chip-select; it must be
    // released too, or the next open_chip() cannot re-claim the line (the line
    // stays busy and the reader reports "no ST25R" on every reopen).
    if (g_cs_fd >= 0) {
        close(g_cs_fd);
        g_cs_fd = -1;
    }
}

// The subcommands pass argv[path_arg] (if any) as the transport spec; otherwise
// $ST25R_SPIDEV / $ST25R_BUS steer it, defaulting to full auto-detect.
std::string resolve_spec(int argc, char** argv, int path_arg) {
    if (argc > path_arg && argv[path_arg] && argv[path_arg][0] != '\0') {
        return argv[path_arg];
    }
    const char* bus = std::getenv("ST25R_BUS");
    if (bus && bus[0] != '\0') return bus; // "spi" | "i2c" | "i2c:/dev/i2c-1" ...
    const char* env = std::getenv("ST25R_SPIDEV");
    if (env && env[0] != '\0') return env;
    return "auto";
}
#endif // __linux__

} // namespace

// ===== Subcommand entry points ===============================================

#if defined(__linux__)
namespace {

// Shared: open, poll for a tag, print UID/ATQA/SAK. Returns uid length (0 = none).
size_t scan_once(std::string& report, uint8_t* uid, size_t uidcap, uint8_t& sak, uint8_t atqa[2]) {
    poller_init();
    if (!reqa(atqa)) {
        report += "No tag in field (present a card and hold it on the antenna).\n";
        return 0;
    }
    char line[64];
    std::snprintf(line, sizeof(line), "ATQA: %02X %02X\n", atqa[0], atqa[1]);
    report += line;
    const size_t uidlen = anticollision(uid, uidcap, sak);
    if (uidlen == 0) {
        report += "Anticollision failed (tag moved? unsupported type?)\n";
        return 0;
    }
    report += "UID:  " + to_hex(uid, uidlen, true) + "\n";
    std::snprintf(line, sizeof(line), "SAK:  %02X  ", sak);
    report += line;
    report += sak_describe(sak) + "\n";
    return uidlen;
}

// One in-process poll cycle for st25r_session_poll(): energize the field, do
// REQA + anticollision, and (when asked, for a Type-2 tag) dump the data pages.
// Fills `out` and returns true on a tag; false on an empty field. Each call
// re-runs poller_init() so a tag presented after the previous one was removed
// is detected again (REQA only answers tags in the IDLE state).
bool poll_tag(St25rScan& out, bool dump_pages) {
    poller_init();
    uint8_t atqa[2] = {0, 0};
    if (!reqa(atqa)) {
        return false;
    }
    uint8_t uid[10] = {0}, sak = 0;
    const size_t uidlen = anticollision(uid, sizeof(uid), sak);
    if (uidlen == 0) {
        return false;
    }
    out.uid.assign(uid, uid + uidlen);
    out.atqa[0] = atqa[0];
    out.atqa[1] = atqa[1];
    out.sak = sak;
    out.sak_text = sak_describe(sak);
    out.t2t = (sak == 0x00);
    out.pages.clear();
    out.page_count = 0;
    if (dump_pages && out.t2t) {
        for (uint8_t page = 0; page < 44; page += 4) {
            uint8_t block[16] = {0};
            if (!t2t_read_page(page, block)) {
                break;
            }
            out.pages.insert(out.pages.end(), block, block + 16);
            out.page_count += 4;
        }
    }
    return true;
}

// Load the NFC-A passive-target memory (PT_Memory) the chip answers REQA /
// anticollision / SELECT from automatically. 15 bytes follow the 0xA0 op:
//   [0..9]  NFCID1 (the 4- or 7-byte UID, in order)
//   [10,11] SENS_RES (ATQA)
//   [12]    SEL_RES level 1 (SAK; cascade bit 0x04 set for a 7-byte UID)
//   [13]    SEL_RES level 2 (SAK, 7-byte UID only)
//   [14]    RFU (0)
// Layout per DS13541 Table 8 (via BatteryJuice4/st25r3916b-driver).
void pt_load_a(const uint8_t* uid, size_t uidlen, const uint8_t atqa[2], uint8_t sak) {
    uint8_t w[1 + 15] = {0};
    w[0] = SPI_PT_LOAD_A;
    for (size_t i = 0; i < uidlen && i < 10; ++i) {
        w[1 + i] = uid[i];
    }
    w[1 + 10] = atqa[0];
    w[1 + 11] = atqa[1];
    if (uidlen >= 7) {
        w[1 + 12] = static_cast<uint8_t>(sak | 0x04); // L1 is a cascade
        w[1 + 13] = sak;                              // L2 completes the UID
    } else {
        w[1 + 12] = sak;                              // single level
    }
    bus_write_then_read(w, sizeof(w), nullptr, 0);
}

// Put the (already-open) chip into NFC-A passive-target (card-emulation) mode
// advertising the given UID/ATQA/SAK. The chip then auto-answers a reader from
// PT_Memory; st25r_emulate_step()/emulate_loop() only watch for the activation
// IRQ. Shared by the blocking CLI loop and the in-process st25r_emulate_* API.
void emulate_setup(const uint8_t* uid, size_t uidlen, const uint8_t atqa[2], uint8_t sak) {
    cmd(CMD_SET_DEFAULT);
    sleep_ms(1);
    reg_write(REG_OP_CONTROL, OP_CONTROL_EN); // power the oscillator
    for (int i = 0; i < 50; ++i) {
        if (reg_read(REG_AUX_DISPLAY) & AUX_DISPLAY_OSC_OK) break;
        sleep_ms(1);
    }
    reg_write(REG_MODE, MODE_TARG_TARG | MODE_OM_TARG_NFCA); // 0x88 target NFC-A
    reg_write(REG_BIT_RATE, 0x00);            // 106 kbit/s both ways
    reg_write(REG_PASSIVE_TARGET, PASSIVE_TARGET_LISTEN_A);  // fdel, NFC-F/AP2P off, auto-AC on
    reg_write(REG_MASK_RX_TIMER, 0x02);       // mask-receive timer (Bruce _setupListenMode)
    cmd(CMD_STOP);
    // UID length for the chip's automatic anticollision.
    if (uidlen >= 7) {
        reg_set_bits(REG_AUX, AUX_NFC_ID_7BYTES);
    } else {
        reg_clr_bits(REG_AUX, AUX_NFC_ID_7BYTES);
    }
    pt_load_a(uid, uidlen, atqa, sak);
    reg_clr_bits(REG_PASSIVE_TARGET, PT_D_106_AC_A); // enable automatic NFC-A anticollision
    reg_write(REG_OP_CONTROL, OP_CONTROL_EN | OP_CONTROL_RX_EN | OP_CONTROL_EN_FD_AUTO);
    read_irqs();                              // clear anything latched during bring-up
    cmd(CMD_GOTO_SENSE);                      // enter Sense/Idle: ready to be read
}

// --- Listen-mode responder (Bruce ST25R3916::_handleListenLoop) --------------
//
// After the chip auto-handles REQA/anticollision/SELECT from PT_Memory and
// raises WU_A/WU_A_X (selected), d_106_ac_a is set so the host takes over the
// data exchange. The reader's higher-layer command then arrives in the FIFO
// (RXE); we answer it here. Minimal ISO14443-3A Type-2 set, enough for a phone
// to read an NTAG/Ultralight clone.

std::vector<uint8_t> g_emu_pages;   // served T2T page bytes (multiple of 4)
unsigned g_emu_page_count = 0;

// Send a response frame (hardware appends CRC_A), then wait for TX-end.
void listen_respond(const uint8_t* resp, size_t len) {
    cmd(CMD_CLEAR_FIFO);
    fifo_write(resp, len);
    set_num_tx_bits(static_cast<uint16_t>(len * 8));
    cmd(CMD_TX_WITH_CRC);
    wait_irq(IRQ_TXE, 20);
}

// Parse one reader command (already in the FIFO) and answer it.
void handle_listen_rx() {
    uint8_t fifo[64] = {0};
    const size_t n = fifo_read(fifo, sizeof(fifo));
    if (n < 1) return;
    const uint8_t c = fifo[0];
    if (c == 0x30 && n >= 2) {                 // READ: 16 bytes (4 pages from addr)
        const uint8_t addr = fifo[1];
        uint8_t resp[16] = {0};
        for (int i = 0; i < 4; ++i) {
            const unsigned page = g_emu_page_count ? (addr + i) % g_emu_page_count : (addr + i);
            for (int b = 0; b < 4; ++b) {
                const size_t off = static_cast<size_t>(page) * 4 + b;
                resp[i * 4 + b] = (off < g_emu_pages.size()) ? g_emu_pages[off] : 0;
            }
        }
        listen_respond(resp, sizeof(resp));
    } else if (c == 0x50) {                     // HALT
        cmd(CMD_GOTO_SLEEP);
    }
    // Other commands (WRITE, GET_VERSION, ...) are left unanswered: a read-only
    // UID/NDEF clone is enough for the phone to detect and read the tag.
}

// One non-blocking service pass of the listen loop. Returns true when the
// reader interacted (selected us or sent a frame), for the UI activity counter.
bool emulate_service(long timeout_ms) {
    const uint32_t irqs = wait_irq(IRQ_WU_A | IRQ_WU_AX | IRQ_RXE | IRQ_EOF, timeout_ms);
    bool interacted = false;
    if (irqs & (IRQ_WU_A | IRQ_WU_AX)) {        // selected: take over data handling
        reg_set_bits(REG_PASSIVE_TARGET, PT_D_106_AC_A);
        interacted = true;
    }
    if (irqs & IRQ_RXE) {                        // a command frame is waiting
        handle_listen_rx();
        interacted = true;
    }
    if (irqs & IRQ_EOF) {                        // reader field dropped: re-arm auto-AC
        reg_clr_bits(REG_PASSIVE_TARGET, PT_D_106_AC_A);
        cmd(CMD_GOTO_SENSE);
    }
    return interacted;
}

// Default ATQA for a UID of this length when the caller has no measured value
// (the CLI --rfid-emulate takes only a UID): single-size -> 0x0004, double -> 0x0044.
void default_atqa(size_t uidlen, uint8_t out[2]) {
    out[0] = (uidlen >= 7) ? 0x44 : 0x04;
    out[1] = 0x00;
}

// NFC-A passive-target / listen loop advertising `uid`. Same register path the
// ESP32 firmware uses; runs until the process is killed (SIGTERM from the UI).
// ponytail: relies on the chip's automatic target mode; robust T2T emulation
// ultimately wants ST's full listen state machine. Experimental.
[[noreturn]] void emulate_loop(const uint8_t* uid, size_t uidlen) {
    uint8_t atqa[2];
    default_atqa(uidlen, atqa);
    emulate_setup(uid, uidlen, atqa, /*sak=*/0x00); // CLI: advertise as Type-2

    std::fputs("Listening as a tag. Present a reader. Ctrl-C / Back to stop.\n", stdout);
    std::fflush(stdout);

    // ST25R_EMU_DEBUG=1 traces every service pass (raw IRQ word + registers) so
    // a captured log shows exactly what the reader triggers.
    const bool dbg = std::getenv("ST25R_EMU_DEBUG") != nullptr;
    unsigned long activations = 0;
    for (;;) {
        if (dbg) {
            const uint32_t irqs = wait_irq(IRQ_WU_A | IRQ_WU_AX | IRQ_RXE | IRQ_EOF, 1000);
            std::printf("tick irq=%08X aux=%02X pt=%02X\n", irqs, reg_read(REG_AUX_DISPLAY),
                        reg_read(REG_PASSIVE_TARGET));
            std::fflush(stdout);
            if (irqs & (IRQ_WU_A | IRQ_WU_AX)) reg_set_bits(REG_PASSIVE_TARGET, PT_D_106_AC_A);
            if (irqs & IRQ_RXE) {
                uint8_t fifo[64] = {0};
                const size_t n = fifo_read(fifo, sizeof(fifo));
                std::printf("  rx n=%zu cmd=%02X %02X\n", n, n > 0 ? fifo[0] : 0,
                            n > 1 ? fifo[1] : 0);
                std::fflush(stdout);
                // Still answer so a phone can actually read during debug.
                if (n >= 2 && fifo[0] == 0x30) {
                    uint8_t resp[16] = {0};
                    for (int i = 0; i < 4; ++i) {
                        const unsigned page =
                            g_emu_page_count ? (fifo[1] + i) % g_emu_page_count : (fifo[1] + i);
                        for (int b = 0; b < 4; ++b) {
                            const size_t off = static_cast<size_t>(page) * 4 + b;
                            resp[i * 4 + b] = (off < g_emu_pages.size()) ? g_emu_pages[off] : 0;
                        }
                    }
                    listen_respond(resp, sizeof(resp));
                } else if (n >= 1 && fifo[0] == 0x50) {
                    cmd(CMD_GOTO_SLEEP);
                }
            }
            if (irqs & IRQ_EOF) {
                reg_clr_bits(REG_PASSIVE_TARGET, PT_D_106_AC_A);
                cmd(CMD_GOTO_SENSE);
            }
            continue;
        }
        if (emulate_service(1000)) {
            std::printf("reader interaction #%lu\n", ++activations);
            std::fflush(stdout);
        }
    }
}

} // namespace
#endif

bool st25r_present_i2c() {
#if defined(__linux__)
    for (const auto& bus : i2c_buses()) {
        std::string discard;
        if (try_open_i2c(bus, kI2cAddr, discard)) {
            close_chip();
            return true;
        }
    }
    return false;
#else
    return false;
#endif
}

bool st25r_session_open(std::string* report) {
#if defined(__linux__)
    std::string local;
    const bool ok = open_chip("auto", local);
    if (report) *report = local;
    if (!ok) close_chip();
    return ok;
#else
    (void)report;
    return false;
#endif
}

bool st25r_session_ready() {
#if defined(__linux__)
    return g_fd >= 0;
#else
    return false;
#endif
}

bool st25r_session_poll(St25rScan& out, bool dump_pages) {
#if defined(__linux__)
    if (g_fd < 0) return false;
    return poll_tag(out, dump_pages);
#else
    (void)out; (void)dump_pages;
    return false;
#endif
}

void st25r_session_close() {
#if defined(__linux__)
    close_chip();
#endif
}

bool st25r_emulate_begin(const St25rScan& scan) {
#if defined(__linux__)
    if (g_fd < 0 || scan.uid.empty()) return false;
    uint8_t atqa[2] = {scan.atqa[0], scan.atqa[1]};
    if (atqa[0] == 0 && atqa[1] == 0) default_atqa(scan.uid.size(), atqa);
    // Serve the tag's own pages to the reader (READ responder) when we have them.
    g_emu_pages = scan.pages;
    g_emu_page_count = scan.page_count;
    emulate_setup(scan.uid.data(), scan.uid.size(), atqa, scan.sak);
    return true;
#else
    (void)scan;
    return false;
#endif
}

bool st25r_emulate_step() {
#if defined(__linux__)
    if (g_fd < 0) return false;
    // Short window so the UI timer stays responsive; services one reader
    // command (selection, READ, HALT, ...) per call.
    return emulate_service(40);
#else
    return false;
#endif
}

std::string st25r_save_nfc(const St25rScan& scan, const std::string& dir) {
    if (scan.uid.empty()) return "";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return "";

    const std::string base = to_hex(scan.uid.data(), scan.uid.size());
    std::string path;
    FILE* f = nullptr;
    for (int i = 0; i <= 100000; ++i) {
        path = dir + "/" + base + (i ? "_" + std::to_string(i) : "") + ".nfc";
        f = std::fopen(path.c_str(), "wbx");
        if (f || errno != EEXIST) break;
    }
    if (!f) return "";

    const bool t2t = scan.t2t && scan.page_count > 0;
    std::fputs("Filetype: Flipper NFC device\n", f);
    std::fputs("Version: 4\n", f);
    std::fputs("# Saved by Bruce CardputerZero (ST25R3916)\n", f);
    std::fprintf(f, "Device type: %s\n", t2t ? "NTAG/Ultralight" : "ISO14443-3A");
    std::fputs("# UID, ATQA and SAK are common for all formats\n", f);
    std::fprintf(f, "UID: %s\n", to_hex(scan.uid.data(), scan.uid.size(), true).c_str());
    std::fprintf(f, "ATQA: %02X %02X\n", scan.atqa[0], scan.atqa[1]);
    std::fprintf(f, "SAK: %02X\n", scan.sak);
    if (t2t) {
        std::fputs("Data format version: 2\n", f);
        std::fputs("# Mifare Ultralight / NTAG data pages\n", f);
        std::fprintf(f, "Pages total: %u\n", scan.page_count);
        for (unsigned p = 0; p < scan.page_count; ++p) {
            std::fprintf(f, "Page %u: %s\n", p, to_hex(&scan.pages[p * 4], 4, true).c_str());
        }
    }
    std::fclose(f);
    return path;
}

int rfid_read_main(int argc, char** argv) {
#if defined(__linux__)
    std::string report = "RFID read (ISO14443A)\n";
    if (!open_chip(resolve_spec(argc, argv, 0), report)) {
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    uint8_t uid[10] = {0}, sak = 0, atqa[2] = {0};
    const size_t uidlen = scan_once(report, uid, sizeof(uid), sak, atqa);
    close_chip();
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return uidlen ? 0 : 1;
#else
    (void)argc; (void)argv;
    std::fputs("RFID is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

int rfid_dump_main(int argc, char** argv) {
#if defined(__linux__)
    std::string report = "RFID dump (Type-2 tag)\n";
    if (!open_chip(resolve_spec(argc, argv, 0), report)) {
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    uint8_t uid[10] = {0}, sak = 0, atqa[2] = {0};
    const size_t uidlen = scan_once(report, uid, sizeof(uid), sak, atqa);
    if (uidlen == 0) {
        close_chip();
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    if (sak & 0x04 || (sak != 0x00)) {
        report += "Note: dump supports Type-2 tags (SAK 0x00, e.g. NTAG/Ultralight).\n";
    }
    // READ returns 4 pages at a time; step by 4 up to 44 pages (NTAG215-ish).
    for (uint8_t page = 0; page < 44; page += 4) {
        uint8_t block[16] = {0};
        if (!t2t_read_page(page, block)) {
            break;
        }
        for (int i = 0; i < 4; ++i) {
            char line[48];
            std::snprintf(line, sizeof(line), "pg %02u: %s\n", page + i,
                          to_hex(&block[i * 4], 4, true).c_str());
            report += line;
        }
    }
    close_chip();
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return 0;
#else
    (void)argc; (void)argv;
    std::fputs("RFID is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

int rfid_write_main(int argc, char** argv) {
#if defined(__linux__)
    if (argc < 2) {
        std::fputs("usage: --rfid-write <page> <8-hex-digits> [spidev]\n", stdout);
        return 2;
    }
    const int page = std::atoi(argv[0]);
    std::vector<uint8_t> data;
    if (page < 0 || page > 255 || !parse_hex(argv[1], data) || data.size() != 4) {
        std::fputs("error: page must be 0-255 and data exactly 4 bytes (8 hex digits).\n", stdout);
        return 2;
    }
    std::string report = "RFID write (Type-2 tag)\n";
    if (!open_chip(resolve_spec(argc, argv, 2), report)) {
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    uint8_t uid[10] = {0}, sak = 0, atqa[2] = {0};
    const size_t uidlen = scan_once(report, uid, sizeof(uid), sak, atqa);
    bool ok = false;
    if (uidlen) {
        char line[64];
        std::snprintf(line, sizeof(line), "Writing page %d = %s ...\n", page,
                      to_hex(data.data(), 4, true).c_str());
        report += line;
        ok = t2t_write_page(static_cast<uint8_t>(page), data.data());
        report += ok ? "Write OK (tag ACKed).\n" : "Write FAILED (no ACK / read-only page).\n";
    }
    close_chip();
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return ok ? 0 : 1;
#else
    (void)argc; (void)argv;
    std::fputs("RFID is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

int rfid_emulate_main(int argc, char** argv) {
#if defined(__linux__)
    if (argc < 1) {
        std::fputs("usage: --rfid-emulate <uid-hex> [spidev]  (4, 7 or 10 byte UID)\n", stdout);
        return 2;
    }
    std::vector<uint8_t> uid;
    if (!parse_hex(argv[0], uid) || (uid.size() != 4 && uid.size() != 7 && uid.size() != 10)) {
        std::fputs("error: UID must be 4, 7 or 10 bytes of hex.\n", stdout);
        return 2;
    }
    std::string report = "RFID emulate (NFC-A Type-2 target)\nUID: " + to_hex(uid.data(), uid.size(), true) + "\n";
    if (!open_chip(resolve_spec(argc, argv, 1), report)) {
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    std::fputs(report.c_str(), stdout);
    emulate_loop(uid.data(), uid.size()); // never returns; SIGTERM ends it
#else
    (void)argc; (void)argv;
    std::fputs("RFID is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

int rfid_clone_main(int argc, char** argv) {
#if defined(__linux__)
    std::string report = "RFID clone (read a tag, then emulate its UID)\n";
    if (!open_chip(resolve_spec(argc, argv, 0), report)) {
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    uint8_t uid[10] = {0}, sak = 0, atqa[2] = {0};
    const size_t uidlen = scan_once(report, uid, sizeof(uid), sak, atqa);
    std::fputs(report.c_str(), stdout);
    if (uidlen == 0) {
        close_chip();
        return 1;
    }
    std::fputs("Now emulating that tag...\n", stdout);
    std::fflush(stdout);
    emulate_loop(uid, uidlen); // never returns; SIGTERM ends it
#else
    (void)argc; (void)argv;
    std::fputs("RFID is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

} // namespace platform
