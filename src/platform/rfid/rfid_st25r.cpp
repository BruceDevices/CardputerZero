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

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

#if defined(__linux__)
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
        if (c == ':' || c == '-' || std::isspace(static_cast<unsigned char>(c))) continue;
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
constexpr uint8_t CMD_UNMASK_RX          = 0xD1;
constexpr uint8_t CMD_ADJUST_REGULATORS  = 0xD6;
constexpr uint8_t CMD_CLEAR_FIFO         = 0xDB;
constexpr uint8_t CMD_GOTO_SENSE         = 0xCD; // passive target -> Sense/Idle (listen)
constexpr uint8_t CMD_GOTO_SLEEP         = 0xCE; // passive target -> Sleep/Halt
constexpr uint8_t CMD_RESET_RXGAIN       = 0xD5;
constexpr uint8_t CMD_MEAS_PWR           = 0xDF;
constexpr uint8_t CMD_SPACE_B_ACCESS     = 0xFB; // prefix: next op targets Space-B

// Space-B register offsets (accessed via the 0xFB prefix; see reg_write_b()).
constexpr uint8_t B_EMD_SUP   = 0x05;
constexpr uint8_t B_CORR1     = 0x0C;
constexpr uint8_t B_CORR2     = 0x0D;
constexpr uint8_t B_RES_AM    = 0x2A;
constexpr uint8_t B_OVER1     = 0x30;
constexpr uint8_t B_OVER2     = 0x31;
constexpr uint8_t B_UNDER1    = 0x32;
constexpr uint8_t B_UNDER2    = 0x33;

// Registers (space A).
constexpr uint8_t REG_OP_CONTROL     = 0x02;
constexpr uint8_t REG_MODE           = 0x03;
constexpr uint8_t REG_BIT_RATE       = 0x04;
constexpr uint8_t REG_ISO14443A_NFC  = 0x05;
constexpr uint8_t REG_PASSIVE_TARGET = 0x08;
constexpr uint8_t REG_IO_CFG1        = 0x00;
constexpr uint8_t REG_IO_CFG2        = 0x01;
constexpr uint8_t REG_AUX            = 0x0A;
constexpr uint8_t REG_RX_CONF1       = 0x0B; // receiver/demodulator config 1
constexpr uint8_t REG_RX_CONF2       = 0x0C; // receiver/demodulator config 2
constexpr uint8_t REG_RX_CONF3       = 0x0D;
constexpr uint8_t REG_RX_CONF4       = 0x0E;
constexpr uint8_t REG_MASK_RX_TIMER  = 0x0F;
constexpr uint8_t REG_NO_RESP_TIMER1 = 0x10;
constexpr uint8_t REG_NO_RESP_TIMER2 = 0x11;
constexpr uint8_t REG_TIMER_EMV_CTRL = 0x12;
constexpr uint8_t REG_IRQ_MASK_MAIN  = 0x16;
constexpr uint8_t REG_IRQ_MAIN       = 0x1A; // 0x1A..0x1D auto-increment on read
constexpr uint8_t REG_FIFO_STATUS1   = 0x1E;
constexpr uint8_t REG_TARGET_STATUS  = 0x21;
constexpr uint8_t REG_NUM_TX_BYTES1  = 0x22;
constexpr uint8_t REG_ANT_TUNE_A     = 0x26; // antenna tuning (listen vs poll differ)
constexpr uint8_t REG_ANT_TUNE_B     = 0x27;
constexpr uint8_t REG_TX_DRIVER      = 0x28;
constexpr uint8_t REG_PT_MOD         = 0x29; // passive-target modulation
constexpr uint8_t REG_EFD_ON         = 0x2A; // external field detector on threshold
constexpr uint8_t REG_EFD_OFF        = 0x2B;
constexpr uint8_t REG_AUX_DISPLAY    = 0x31; // osc/field status (OSC_OK on bit 4)
constexpr uint8_t REG_IC_IDENTITY    = 0x3F;

// Register bit fields.
constexpr uint8_t OP_CONTROL_EN         = 1 << 7;
constexpr uint8_t OP_CONTROL_RX_EN      = 1 << 6;
constexpr uint8_t OP_CONTROL_TX_EN      = 1 << 3;
constexpr uint8_t OP_CONTROL_EN_FD_AUTO = 3 << 0;
constexpr uint8_t AUX_DIS_CORR          = 0x04;   // REG_AUX: disable correlator
constexpr uint8_t MODE_TARG_INIT        = 0 << 7;
constexpr uint8_t MODE_TARG_TARG        = 1 << 7;
constexpr uint8_t MODE_OM_ISO14443A     = 0x1 << 3;
constexpr uint8_t MODE_OM_TARG_NFCA     = 0x1 << 3;
constexpr uint8_t AUX_NO_CRC_RX         = 1 << 7;
constexpr uint8_t FIFO_STATUS2_B_MASK   = 3 << 6;
constexpr uint8_t AUX_DISPLAY_OSC_OK    = 1 << 4; // crystal oscillator stable
constexpr uint8_t AUX_NFC_ID_MASK       = 3 << 4;
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
// IRQ registers are packed with the main register in the low byte.
constexpr uint32_t IRQ_WU_A  = 0x01000000;
constexpr uint32_t IRQ_WU_AX = 0x02000000;
constexpr uint32_t IRQ_EOF   = 0x00000800;
constexpr uint32_t IRQ_EON   = 0x00001000;
constexpr uint32_t IRQ_DCT   = 0x00008000;

// SPI mode bytes.
constexpr uint8_t SPI_READ_MODE  = 0x40;
constexpr uint8_t SPI_WRITE_MODE = 0x00;
constexpr uint8_t SPI_FIFO_LOAD  = 0x80;
constexpr uint8_t SPI_FIFO_READ  = 0x9F;
constexpr uint8_t SPI_PT_LOAD_A  = 0xA0; // load the NFC-A passive-target memory
constexpr uint8_t SPI_PT_READ    = 0xBF;

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
bool bus_write_then_read(const uint8_t* wbuf, size_t wn, uint8_t* rbuf, size_t rn) {
    if (g_bus == Bus::I2c) {
        return rn == 0 ? i2c_write(g_fd, wbuf, wn)
                       : i2c_write_read(g_fd, g_i2c_addr, wbuf, wn, rbuf, rn);
    }
    // SPI: one full-duplex frame of wn + rn bytes; the first wn carry the
    // header/data, the trailing rn clock out the response (tx stuffed 0x00).
    uint8_t tx[1 + 256] = {0};
    uint8_t rx[1 + 256] = {0};
    const size_t total = wn + rn;
    if (total > sizeof(tx)) return false;
    std::memcpy(tx, wbuf, wn);
    if (!spi_xfer(tx, rx, total)) return false;
    if (rn) std::memcpy(rbuf, &rx[wn], rn);
    return true;
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

void reg_clr_bits(uint8_t addr, uint8_t bits) {
    reg_write(addr, static_cast<uint8_t>(reg_read(addr) & ~bits));
}

// Masked write: keep bits outside `mask`, set bits inside `mask` from `val`.
void reg_mask(uint8_t addr, uint8_t mask, uint8_t val) {
    reg_write(addr, static_cast<uint8_t>((reg_read(addr) & ~mask) | (val & mask)));
}

// Space-B register access: the 0xFB prefix selects the Space-B window for the
// following write (offset is the low 6 bits).
void reg_write_b(uint8_t off, uint8_t val) {
    uint8_t w[3] = {CMD_SPACE_B_ACCESS, static_cast<uint8_t>(SPI_WRITE_MODE | (off & 0x3F)), val};
    bus_write_then_read(w, 3, nullptr, 0);
}

void fifo_write(const uint8_t* data, size_t n) {
    uint8_t w[1 + 256];
    if (n > 256) n = 256;
    w[0] = SPI_FIFO_LOAD;
    std::memcpy(&w[1], data, n);
    bus_write_then_read(w, n + 1, nullptr, 0);
}

size_t fifo_count() {
    const uint8_t hdr = SPI_READ_MODE | REG_FIFO_STATUS1;
    uint8_t status[2]{};
    bus_write_then_read(&hdr, 1, status, sizeof(status));
    const uint8_t hi = (status[1] & FIFO_STATUS2_B_MASK) >> 6;
    return (static_cast<size_t>(hi) << 8) | status[0];
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
    const uint8_t frame[] = {
        REG_NUM_TX_BYTES1, static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits)
    };
    bus_write_then_read(frame, sizeof(frame), nullptr, 0);
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
    // At 100 kHz an I2C target can take more than 5 ms from reader TX to
    // response. Use 2048 ticks of 64/fc (~9.7 ms) for the reader timeout.
    reg_write(REG_TIMER_EMV_CTRL, 0x00);
    reg_write(REG_NO_RESP_TIMER1, 0x08);
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
// With the CardputerZero NFC overlay, the kernel controls GPIO22 as SPI0 CS2.
// Older setups use a GPIO handle and suppress the spidev node's own CS.
bool try_open_spi(const std::string& path, std::string& report) {
    g_bus = Bus::Spi;
    g_cs_fd = -1;
    const bool kernel_cs = path == "/dev/spidev0.2";
    const std::string cs_chip = env_or("ST25R_CS_CHIP", "/dev/gpiochip0");
    const auto cs_line = static_cast<unsigned>(std::atoi(env_or("ST25R_CS_GPIO", "22").c_str()));
    if (!kernel_cs && !gpio_claim_output(cs_chip.c_str(), cs_line, true, &g_cs_fd)) {
        report += "error: cannot claim NFC CS GPIO" + std::to_string(cs_line) + " on " +
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
        if (g_cs_fd >= 0) close(g_cs_fd);
        g_cs_fd = -1;
        return false;
    }
    uint8_t mode = SPI_MODE_1 | (kernel_cs ? 0 : SPI_NO_CS);
    uint8_t bits = 8;
    uint32_t speed = 2000000;
    if (ioctl(g_fd, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(g_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(g_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        report += "error: cannot configure SPI on " + path + " (" + std::strerror(errno) + ")\n";
    } else {
        report += kernel_cs ? "NFC CS: kernel-managed on " + path + "\n"
                            : "NFC CS: GPIO" + std::to_string(cs_line) +
                                  " (manual, SPI_NO_CS on " + path + ")\n";
        if (probe_identity(report)) return true;
    }

    close(g_fd);
    g_fd = -1;
    if (g_cs_fd >= 0) close(g_cs_fd);
    g_cs_fd = -1;
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

    auto open_spi = [&report]() {
        const char* env = std::getenv("ST25R_SPIDEV");
        if (env && env[0] != '\0') return try_open_spi(env, report);
        if (access("/dev/spidev0.2", F_OK) == 0 && try_open_spi("/dev/spidev0.2", report)) {
            return true;
        }
        const std::string p =
            spidev_pick({APP_ST25R_SPIDEV, "/dev/spidev0.1", "/dev/spidev0.0"});
        return try_open_spi(p.empty() ? APP_ST25R_SPIDEV : p, report);
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
        return open_spi();
    }

    // auto: SPI first (the CAP CC1101's ST25R), then scan I2C modules at 0x50.
    if (open_spi()) return true;
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

// PT memory A contains NFCID1, ATQA and the SAK for each cascade level.
// The ST25R3916 auto-anticollision engine supports four and seven byte UIDs.
bool pt_load_a(const St25rScan& scan) {
    uint8_t frame[1 + 15]{};
    frame[0] = SPI_PT_LOAD_A;
    std::memcpy(frame + 1, scan.uid.data(), scan.uid.size());
    frame[11] = scan.atqa[0];
    frame[12] = scan.atqa[1];
    if (frame[11] == 0 && frame[12] == 0) {
        frame[11] = (scan.uid.size() == 7) ? 0x44 : 0x04;
    }
    const uint8_t sak = scan.sak & ~0x04;
    frame[13] = (scan.uid.size() == 7) ? 0x04 : sak;
    frame[14] = (scan.uid.size() == 7) ? sak : 0;
    if (!bus_write_then_read(frame, sizeof(frame), nullptr, 0)) return false;

    // PT memory reads prepend one dummy byte on both buses.
    uint8_t readback[sizeof(frame)]{};
    const uint8_t op = SPI_PT_READ;
    return bus_write_then_read(&op, 1, readback, sizeof(readback)) &&
           std::memcmp(frame + 1, readback + 1, sizeof(frame) - 1) == 0;
}

std::vector<uint8_t> g_emu_pages; // Only the listener accesses the pages while it runs.
bool g_emu_type2 = false;
bool g_emu_test_tag = false;
bool g_emu_selected = false;
bool g_emu_field = false;
bool g_emu_debug = false;
std::thread g_emu_worker;
std::atomic<bool> g_emu_stop{false};
std::atomic<unsigned> g_emu_selections{0};
unsigned g_emu_reported = 0;
volatile std::sig_atomic_t g_emu_interrupted = 0;

bool emulate_hw_init() {
    cmd(CMD_STOP);
    reg_write(REG_OP_CONTROL, 0);
    sleep_ms(2);
    cmd(CMD_SET_DEFAULT);

    // M5 CAP antenna tuning and passive load modulation.
    reg_write(REG_IO_CFG1, 0x07);
    reg_write(REG_IO_CFG2, 0x3C);
    reg_write(REG_TX_DRIVER, 0xD0);
    reg_write_b(B_RES_AM, 0x80);
    reg_write_b(B_RES_AM, 0x00);
    reg_write(REG_EFD_ON, 0x13);
    reg_write(REG_EFD_OFF, 0x02);
    reg_write(REG_PT_MOD, 0x5F);
    reg_write_b(B_EMD_SUP, 0x40);
    reg_write(REG_ANT_TUNE_A, 0x82);
    reg_write(REG_ANT_TUNE_B, 0x82);

    reg_write(REG_OP_CONTROL, OP_CONTROL_EN);
    bool oscillator_ready = false;
    for (int i = 0; i < 50; ++i) {
        oscillator_ready = (reg_read(REG_AUX_DISPLAY) & AUX_DISPLAY_OSC_OK) != 0;
        if (oscillator_ready) break;
        sleep_ms(1);
    }
    if (!oscillator_ready) return false;

    read_irqs();
    cmd(CMD_MEAS_PWR);
    if (!(wait_irq(IRQ_DCT, 10) & IRQ_DCT)) return false;
    cmd(CMD_ADJUST_REGULATORS);
    if (!(wait_irq(IRQ_DCT, 20) & IRQ_DCT)) return false;

    reg_write(REG_BIT_RATE, 0x00);
    reg_write(REG_ISO14443A_NFC, 0x00);
    reg_clr_bits(REG_AUX, AUX_DIS_CORR);
    reg_write_b(B_OVER1, 0x40);
    reg_write_b(B_OVER2, 0x03);
    reg_write_b(B_UNDER1, 0x40);
    reg_write_b(B_UNDER2, 0x03);
    reg_write_b(B_CORR1, 0x47);
    reg_write_b(B_CORR2, 0x00);
    reg_write(REG_RX_CONF1, 0x08);
    reg_write(REG_RX_CONF2, 0x2D);
    reg_write(REG_RX_CONF3, 0xD8);
    reg_write(REG_RX_CONF4, 0x22);
    cmd(CMD_RESET_RXGAIN);
    return true;
}

bool emulate_setup(const St25rScan& scan, bool test_tag = false) {
    if ((scan.uid.size() != 4 && scan.uid.size() != 7) || !emulate_hw_init()) return false;

    constexpr uint8_t mode = MODE_TARG_TARG | MODE_OM_TARG_NFCA;
    reg_write(REG_MODE, mode);
    reg_write(REG_PASSIVE_TARGET, PASSIVE_TARGET_LISTEN_A);
    reg_write(REG_MASK_RX_TIMER, 0x02);
    reg_mask(REG_AUX, AUX_NFC_ID_MASK, scan.uid.size() == 7 ? AUX_NFC_ID_7BYTES : 0);
    cmd(CMD_STOP);
    if (!pt_load_a(scan) || reg_read(REG_MODE) != mode) return false;

    constexpr uint32_t mask = ~(IRQ_RXE | IRQ_ERRORS | IRQ_EON | IRQ_EOF | IRQ_WU_A | IRQ_WU_AX);
    const uint8_t mask_frame[] = {
        REG_IRQ_MASK_MAIN, static_cast<uint8_t>(mask), static_cast<uint8_t>(mask >> 8),
        static_cast<uint8_t>(mask >> 16), static_cast<uint8_t>(mask >> 24)
    };
    if (!bus_write_then_read(mask_frame, sizeof(mask_frame), nullptr, 0)) return false;

    g_emu_pages = scan.pages;
    g_emu_pages.resize(g_emu_pages.size() - g_emu_pages.size() % 4);
    g_emu_type2 = scan.sak == 0 && !g_emu_pages.empty();
    g_emu_test_tag = test_tag;
    g_emu_field = false;
    g_emu_selected = false;
    g_emu_debug = std::getenv("ST25R_EMU_DEBUG") != nullptr;
    read_irqs();
    reg_write(REG_OP_CONTROL, OP_CONTROL_EN | OP_CONTROL_RX_EN | OP_CONTROL_EN_FD_AUTO);
    const uint8_t sense = CMD_GOTO_SENSE;
    if (!bus_write_then_read(&sense, 1, nullptr, 0)) return false;
    const uint8_t unmask = CMD_UNMASK_RX;
    return bus_write_then_read(&unmask, 1, nullptr, 0);
}

void listen_respond(const uint8_t* resp, size_t bits, bool with_crc) {
    cmd(CMD_CLEAR_FIFO);
    fifo_write(resp, (bits + 7) / 8);
    set_num_tx_bits(static_cast<uint16_t>(bits));
    cmd(with_crc ? CMD_TX_WITH_CRC : CMD_TX_WITHOUT_CRC);
}

void listen_halt() {
    reg_write(REG_PASSIVE_TARGET, PASSIVE_TARGET_LISTEN_A);
    cmd(CMD_GOTO_SLEEP);
    g_emu_selected = false;
}

void t2t_respond(const uint8_t* data, size_t n) {
    const size_t pages = g_emu_pages.size() / 4;
    uint8_t ack = 0x00; // NAK: invalid or unsupported request.
    switch (data[0]) {
        case 0x30: { // READ: four pages, wrapping at the end of memory.
            if (n != 2 || data[1] >= pages) break;
            uint8_t response[16];
            for (size_t i = 0; i < 4; ++i) {
                const size_t page = (data[1] + i) % pages;
                std::memcpy(response + i * 4, g_emu_pages.data() + page * 4, 4);
            }
            listen_respond(response, sizeof(response) * 8, true);
            return;
        }
        case 0x3A: { // FAST_READ: contiguous pages through the inclusive end.
            if (n != 3 || data[1] > data[2] || data[2] >= pages) break;
            const size_t count = data[2] - data[1] + 1;
            if (count > 64) break;
            listen_respond(g_emu_pages.data() + data[1] * 4, count * 32, true);
            return;
        }
        case 0xA2: // WRITE: modify the emulated copy in memory.
            if (n == 6 && data[1] >= 4 && data[1] < pages) {
                std::memcpy(g_emu_pages.data() + data[1] * 4, data + 2, 4);
                ack = 0x0A;
            }
            break;
        case 0x60: { // GET_VERSION for the synthetic NTAG215 used by the CLI.
            if (n != 1 || !g_emu_test_tag) break;
            constexpr uint8_t version[] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
            listen_respond(version, sizeof(version) * 8, true);
            return;
        }
    }
    listen_respond(&ack, 4, false);
}

bool emulate_service() {
    const uint32_t irqs = read_irqs();
    if (!irqs) {
        // Use a shorter interval while a reader field is present.
        const timespec delay{0, g_emu_field ? 100000L : 1000000L};
        nanosleep(&delay, nullptr);
        return false;
    }

    if (irqs & IRQ_EOF) {
        g_emu_field = g_emu_selected = false;
        reg_write(REG_PASSIVE_TARGET, PASSIVE_TARGET_LISTEN_A);
        cmd(CMD_GOTO_SENSE);
    }
    if (irqs & IRQ_EON) g_emu_field = true;
    bool selection = false;
    if (!g_emu_selected && (irqs & (IRQ_WU_A | IRQ_WU_AX)) && !(irqs & IRQ_EOF)) {
        // Confirm Active/Active* before switching off hardware anticollision.
        const uint8_t state = reg_read(REG_TARGET_STATUS) & 0x0F;
        if (state == 0x05 || state == 0x0D) {
            g_emu_field = g_emu_selected = selection = true;
            reg_write(REG_PASSIVE_TARGET, PASSIVE_TARGET_LISTEN_A | PT_D_106_AC_A);
        }
    }

    uint8_t frame[64]{};
    size_t n = 0;
    if (g_emu_selected && (irqs & IRQ_RXE)) {
        n = fifo_count();
        if ((irqs & IRQ_ERRORS) || n > sizeof(frame)) {
            cmd(CMD_CLEAR_FIFO);
            cmd(CMD_UNMASK_RX);
        } else if (n) {
            const uint8_t op = SPI_FIFO_READ;
            if (bus_write_then_read(&op, 1, frame, n)) {
                // Some revisions put the checked CRC bytes in the FIFO.
                if (n >= 3) {
                    uint8_t crc[2];
                    iso14443a_crc(frame, n - 2, crc);
                    if (frame[n - 2] == crc[0] && frame[n - 1] == crc[1]) n -= 2;
                }
                if (n == 2 && frame[0] == 0x50 && frame[1] == 0x00) {
                    listen_halt();
                } else if (g_emu_type2) {
                    t2t_respond(frame, n);
                } else {
                    cmd(CMD_UNMASK_RX);
                }
            }
        }
    }
    if (g_emu_debug && (irqs & (IRQ_EON | IRQ_EOF | IRQ_WU_A | IRQ_WU_AX | IRQ_RXE))) {
        std::fprintf(stderr, "NFC IRQ=%08X frame=%02X len=%zu\n", irqs, frame[0], n);
    }
    return selection;
}

// Build an NFC Forum Type-2 NDEF text record for testing with a phone.
void build_test_tag(St25rScan& scan) {
    scan.pages.assign(135 * 4, 0);
    scan.pages[0] = scan.uid[0];
    scan.pages[1] = scan.uid[1];
    scan.pages[2] = scan.uid[2];
    const uint8_t first_cascade[] = {0x88, scan.uid[0], scan.uid[1], scan.uid[2]};
    scan.pages[3] = anticoll_bcc(first_cascade);
    std::memcpy(scan.pages.data() + 4, scan.uid.data() + 3, 4);
    scan.pages[8] = anticoll_bcc(scan.uid.data() + 3);
    scan.pages[9] = 0x48;
    const uint8_t contents[] = {
        0xE1, 0x10, 0x3E, 0x00, // NTAG215 capability container
        0x03, 0x0C, 0xD1, 0x01, 0x08, 0x54, 0x02, 'e', 'n', 'B', 'r', 'u', 'c', 'e', 0xFE
    };
    std::memcpy(scan.pages.data() + 12, contents, sizeof(contents));
}

int emulate_loop(const St25rScan& scan, bool test_tag = false) {
    if (!emulate_setup(scan, test_tag)) {
        std::fputs("error: ST25R3916 could not initialize NFC-A target mode.\n", stderr);
        close_chip();
        return 1;
    }

    g_emu_interrupted = 0;
    struct sigaction stop{};
    stop.sa_handler = [](int) { g_emu_interrupted = 1; };
    sigemptyset(&stop.sa_mask);
    struct sigaction old_int{}, old_term{};
    sigaction(SIGINT, &stop, &old_int);
    sigaction(SIGTERM, &stop, &old_term);

    std::puts("Listening as an NFC-A target. Present a reader. Ctrl-C / Back to stop.");
    std::fflush(stdout);
    unsigned long selections = 0;
    while (!g_emu_interrupted) {
        if (emulate_service()) {
            std::printf("reader selection #%lu\n", ++selections);
            std::fflush(stdout);
        }
    }
    close_chip();
    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);
    return 0;
}

} // namespace
#endif

bool st25r_present_i2c() {
#if defined(__linux__)
    const uint8_t op = SPI_READ_MODE | REG_IC_IDENTITY;
    for (const auto& bus : i2c_buses()) {
        const int fd = i2c_open(bus.c_str(), kI2cAddr);
        if (fd < 0) continue;
        uint8_t id = 0;
        const bool read = i2c_write_read(fd, kI2cAddr, &op, 1, &id, 1);
        i2c_close(fd);
        const uint8_t type = id & IC_TYPE_MASK;
        if (read && (type == IC_TYPE_3916 || type == IC_TYPE_3916B)) return true;
    }
#endif
    return false;
}

bool st25r_session_open(std::string* report) {
#if defined(__linux__)
    std::string local;
    const bool ok = g_fd >= 0 || open_chip(resolve_spec(0, nullptr, 0), local);
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
    if (g_fd < 0 || g_emu_worker.joinable()) return false;
    return poll_tag(out, dump_pages);
#else
    (void)out; (void)dump_pages;
    return false;
#endif
}

void st25r_session_close() {
#if defined(__linux__)
    if (g_emu_worker.joinable()) {
        g_emu_stop.store(true);
        g_emu_worker.join();
    }
    close_chip();
#endif
}

bool st25r_emulate_begin(const St25rScan& scan) {
#if defined(__linux__)
    if (g_fd < 0 || g_emu_worker.joinable()) return false;
    if (!emulate_setup(scan)) {
        field_off();
        return false;
    }
    g_emu_stop.store(false);
    g_emu_selections.store(0);
    g_emu_reported = 0;
    try {
        g_emu_worker = std::thread([] {
            while (!g_emu_stop.load()) {
                if (emulate_service()) ++g_emu_selections;
            }
        });
    } catch (const std::system_error&) {
        field_off();
        return false;
    }
    return true;
#else
    (void)scan;
    return false;
#endif
}

bool st25r_emulate_step() {
#if defined(__linux__)
    if (!g_emu_worker.joinable() || g_emu_reported == g_emu_selections.load()) return false;
    ++g_emu_reported;
    return true;
#else
    return false;
#endif
}

St25rWriteResult st25r_session_write_tag(const St25rScan& scan, std::string& report) {
#if defined(__linux__)
    constexpr size_t first_data_page = 4;
    const auto user_pages = [](const uint8_t* memory) -> size_t {
        const uint8_t* cc = memory + 12; // Type-2 capability container (page 3)
        return cc[0] == 0xE1 && (cc[1] >> 4) == 1 ? cc[2] * 2u : 0;
    };
    if (g_fd < 0 || g_emu_worker.joinable() || !scan.t2t || scan.pages.size() <= 16) {
        report = "A Type-2 dump with data pages is required.";
        return St25rWriteResult::Failed;
    }
    const size_t page_count = std::min(user_pages(scan.pages.data()), scan.pages.size() / 4 - first_data_page);
    if (page_count == 0) {
        report = "The dump has no formatted Type-2 user area.";
        return St25rWriteResult::Failed;
    }

    St25rScan destination;
    if (!poll_tag(destination, false)) {
        field_off();
        return St25rWriteResult::WaitingForTag;
    }
    uint8_t data[16]{};
    if (!destination.t2t || !t2t_read_page(0, data) || user_pages(data) < page_count || data[15] != 0) {
        report = "Use a writable NFC Forum Type-2 tag with enough user space. No pages written.";
        return St25rWriteResult::Failed;
    }

    for (size_t i = 0; i < page_count; ++i) {
        const auto page = static_cast<uint8_t>(first_data_page + i);
        const uint8_t* bytes = scan.pages.data() + page * 4;
        if (!t2t_write_page(page, bytes)) {
            report = "Write stopped at page " + std::to_string(page) + ". " +
                     std::to_string(i) + " pages verified.";
            return St25rWriteResult::Failed;
        }
        if (!t2t_read_page(page, data) || std::memcmp(bytes, data, 4) != 0) {
            report = "Could not verify page " + std::to_string(page) + ". " +
                     std::to_string(i) + " preceding pages verified.";
            return St25rWriteResult::Failed;
        }
    }
    report = std::to_string(page_count) + " user pages written and verified.";
    return St25rWriteResult::Written;
#else
    (void)scan;
    report = "RFID is only available on Linux builds.";
    return St25rWriteResult::Failed;
#endif
}

bool st25r_load_nfc(const std::string& path, St25rScan& scan, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) > 64 * 1024 || ec) {
        error = "Cannot open .nfc file or file is too large.";
        return false;
    }
    std::ifstream file(path);
    if (!file) {
        error = "Cannot open .nfc file.";
        return false;
    }

    St25rScan loaded;
    std::array<bool, 256> present{};
    std::vector<uint8_t> bytes;
    bool filetype = false, have_atqa = false, have_sak = false;
    auto invalid = [&error](std::string_view field) {
        error = "Invalid .nfc field: " + std::string(field);
        return false;
    };
    std::string line;
    while (std::getline(file, line)) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == '#') continue;
        const auto colon = line.find(':', start);
        if (colon == std::string::npos) continue;
        const std::string_view name(line.data() + start, colon - start);
        const auto first = line.find_first_not_of(" \t\r", colon + 1);
        const auto last = line.find_last_not_of(" \t\r");
        const std::string value = first == std::string::npos ? "" : line.substr(first, last - first + 1);
        if (name == "Filetype") {
            if (value != "Flipper NFC device") return invalid(name);
            filetype = true;
        } else if (name == "UID") {
            if (!loaded.uid.empty() || !parse_hex(value, loaded.uid) ||
                (loaded.uid.size() != 4 && loaded.uid.size() != 7 && loaded.uid.size() != 10)) return invalid(name);
        } else if (name == "ATQA") {
            if (have_atqa || !parse_hex(value, bytes) || bytes.size() != 2) return invalid(name);
            std::copy(bytes.begin(), bytes.end(), loaded.atqa);
            have_atqa = true;
        } else if (name == "SAK") {
            if (have_sak || !parse_hex(value, bytes) || bytes.size() != 1) return invalid(name);
            loaded.sak = bytes.front();
            have_sak = true;
        } else if (name.substr(0, 5) == "Page ") {
            unsigned page = 0;
            const auto end = name.data() + name.size();
            const auto parsed = std::from_chars(name.data() + 5, end, page);
            if (parsed.ec != std::errc{} || parsed.ptr != end || page >= present.size() || present[page] ||
                !parse_hex(value, bytes) || bytes.size() != 4) return invalid(name);
            present[page] = true;
            loaded.page_count = std::max(loaded.page_count, page + 1);
            loaded.pages.resize(loaded.page_count * 4);
            std::copy(bytes.begin(), bytes.end(), loaded.pages.begin() + page * 4);
        }
    }
    if (file.bad() || !filetype || loaded.uid.empty() || !have_atqa || !have_sak) {
        error = "The file must contain a Flipper NFC header and NFC-A UID, ATQA and SAK.";
        return false;
    }
    loaded.t2t = loaded.sak == 0;
    if (loaded.page_count && (!loaded.t2t ||
        std::find(present.begin(), present.begin() + loaded.page_count, false) != present.begin() + loaded.page_count)) {
        error = "Type-2 pages must be contiguous from page 0.";
        return false;
    }
    loaded.sak_text = sak_describe(loaded.sak);
    scan = std::move(loaded);
    error.clear();
    return true;
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
        std::fputs("usage: --rfid-emulate <uid-hex> [bus]  (4 or 7 byte UID)\n", stdout);
        return 2;
    }
    St25rScan scan;
    if (!parse_hex(argv[0], scan.uid) || (scan.uid.size() != 4 && scan.uid.size() != 7)) {
        std::fputs("error: UID must be 4 or 7 bytes of hex.\n", stdout);
        return 2;
    }
    const bool test_tag = scan.uid.size() == 7;
    if (test_tag) build_test_tag(scan);
    std::string report = "RFID emulate (NFC-A target)\nUID: " +
                         to_hex(scan.uid.data(), scan.uid.size(), true) + "\n";
    if (!open_chip(resolve_spec(argc, argv, 1), report)) {
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    std::fputs(report.c_str(), stdout);
    return emulate_loop(scan, test_tag);
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
    St25rScan scan;
    if (!poll_tag(scan, true)) {
        std::fputs(report.c_str(), stdout);
        std::puts("No tag in field (present a card and hold it on the antenna).");
        close_chip();
        return 1;
    }
    report += "UID: " + to_hex(scan.uid.data(), scan.uid.size(), true) + "\n";
    std::fputs(report.c_str(), stdout);
    return emulate_loop(scan);
#else
    (void)argc; (void)argv;
    std::fputs("RFID is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

} // namespace platform
