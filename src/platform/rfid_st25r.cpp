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
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "cap_bringup.h"
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

// Registers (space A).
constexpr uint8_t REG_OP_CONTROL     = 0x02;
constexpr uint8_t REG_MODE           = 0x03;
constexpr uint8_t REG_BIT_RATE       = 0x04;
constexpr uint8_t REG_ISO14443A_NFC  = 0x05;
constexpr uint8_t REG_PASSIVE_TARGET = 0x08;
constexpr uint8_t REG_AUX            = 0x0A;
constexpr uint8_t REG_NO_RESP_TIMER1 = 0x10;
constexpr uint8_t REG_NO_RESP_TIMER2 = 0x11;
constexpr uint8_t REG_TIMER_EMV_CTRL = 0x12;
constexpr uint8_t REG_IRQ_MAIN       = 0x1A; // 0x1A..0x1D auto-increment on read
constexpr uint8_t REG_FIFO_STATUS1   = 0x1E;
constexpr uint8_t REG_FIFO_STATUS2   = 0x1F;
constexpr uint8_t REG_NUM_TX_BYTES1  = 0x22;
constexpr uint8_t REG_NUM_TX_BYTES2  = 0x23;
constexpr uint8_t REG_TX_DRIVER      = 0x28;
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
constexpr uint32_t IRQ_WU_A = 0x01000000;

// SPI mode bytes.
constexpr uint8_t SPI_READ_MODE  = 0x40;
constexpr uint8_t SPI_WRITE_MODE = 0x00;
constexpr uint8_t SPI_FIFO_LOAD  = 0x80;
constexpr uint8_t SPI_FIFO_READ  = 0x9F;

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

#if defined(__linux__)
int g_fd = -1;
int g_cs_fd = -1;      // GPIO line handle: the cap's NFC_CS (active low)
unsigned g_cs_line = 22;

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

uint8_t reg_read(uint8_t addr) {
    uint8_t tx[2] = {static_cast<uint8_t>(SPI_READ_MODE | addr), 0x00};
    uint8_t rx[2] = {0, 0};
    spi_xfer(tx, rx, 2);
    return rx[1];
}

void reg_write(uint8_t addr, uint8_t val) {
    uint8_t tx[2] = {static_cast<uint8_t>(SPI_WRITE_MODE | addr), val};
    uint8_t rx[2] = {0, 0};
    spi_xfer(tx, rx, 2);
}

void cmd(uint8_t c) {
    uint8_t rx = 0;
    spi_xfer(&c, &rx, 1);
}

void fifo_write(const uint8_t* data, size_t n) {
    uint8_t tx[1 + 256];
    uint8_t rx[1 + 256];
    tx[0] = SPI_FIFO_LOAD;
    std::memcpy(&tx[1], data, n);
    spi_xfer(tx, rx, n + 1);
}

size_t fifo_count() {
    const uint8_t hi = (reg_read(REG_FIFO_STATUS2) & FIFO_STATUS2_B_MASK) >> 6;
    const uint8_t lo = reg_read(REG_FIFO_STATUS1);
    return (static_cast<size_t>(hi) << 8) | lo;
}

size_t fifo_read(uint8_t* out, size_t cap) {
    size_t n = fifo_count();
    if (n > cap) n = cap;
    uint8_t tx[1 + 256] = {0};
    uint8_t rx[1 + 256] = {0};
    tx[0] = SPI_FIFO_READ;
    const size_t clamped = (n > 255) ? 255 : n;
    spi_xfer(tx, rx, clamped + 1);
    std::memcpy(out, &rx[1], clamped);
    return clamped;
}

// Read the four consecutive IRQ registers in one transfer (clears them).
uint32_t read_irqs() {
    uint8_t tx[5] = {static_cast<uint8_t>(SPI_READ_MODE | REG_IRQ_MAIN), 0, 0, 0, 0};
    uint8_t rx[5] = {0, 0, 0, 0, 0};
    spi_xfer(tx, rx, 5);
    return static_cast<uint32_t>(rx[1]) | (static_cast<uint32_t>(rx[2]) << 8) |
           (static_cast<uint32_t>(rx[3]) << 16) | (static_cast<uint32_t>(rx[4]) << 24);
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

// Open spidev (MODE1, MSB-first) and verify an ST25R3916 answers.
//
// The cap's NFC_CS is a plain GPIO on this board (G22, HAT_P0/P1 bus) - not a
// spidev chip select. The spidev node is therefore opened with SPI_NO_CS (so
// no internal CS - on this board they are the LCD's G8 or the CC1101's G7 -
// glitches during NFC traffic) and CS is bit-banged around every access.
bool open_chip(const std::string& path, std::string& report) {
    report += cap_bringup().text;

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

    report += "NFC CS: GPIO" + std::to_string(g_cs_line) + " (manual, SPI_NO_CS on " +
              path + ")\n";

    cmd(CMD_SET_DEFAULT);
    sleep_ms(1);
    const uint8_t id = reg_read(REG_IC_IDENTITY);
    const uint8_t ic = id & IC_TYPE_MASK;
    if (ic != IC_TYPE_3916 && ic != IC_TYPE_3916B) {
        char line[96];
        std::snprintf(line, sizeof(line),
                      "error: no ST25R3916 (IC_IDENTITY=0x%02X). Check the CAP seating, the "
                      "power lines above, and the CS GPIO.\n",
                      id);
        report += line;
        close(g_fd);
        g_fd = -1;
        return false;
    }
    report += std::string("ST25R3916") + (ic == IC_TYPE_3916B ? "B" : "") + " detected.\n";
    return true;
}

void close_chip() {
    if (g_fd >= 0) {
        field_off();
        close(g_fd);
        g_fd = -1;
    }
}

std::string resolve_spidev(int argc, char** argv, int path_arg) {
    if (argc > path_arg && argv[path_arg] && argv[path_arg][0] != '\0') {
        return argv[path_arg];
    }
    const char* env = std::getenv("ST25R_SPIDEV");
    if (env && env[0] != '\0') {
        return env;
    }
    // With SPI_NO_CS any spidev node works - pick whichever the kernel made.
    const std::string picked = spidev_pick({APP_ST25R_SPIDEV, "/dev/spidev0.1", "/dev/spidev0.0"});
    return picked.empty() ? std::string(APP_ST25R_SPIDEV) : picked;
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

// NFC-A passive-target / listen loop advertising `uid`. Same register path the
// ESP32 firmware uses; runs until the process is killed (SIGTERM from the UI).
// ponytail: relies on the chip's automatic target mode; robust T2T emulation
// ultimately wants ST's full listen state machine. Experimental.
[[noreturn]] void emulate_loop(const uint8_t* uid, size_t uidlen) {
    cmd(CMD_SET_DEFAULT);
    sleep_ms(1);
    reg_write(REG_OP_CONTROL, OP_CONTROL_EN | OP_CONTROL_RX_EN | OP_CONTROL_EN_FD_AUTO);
    reg_write(REG_MODE, MODE_TARG_TARG | MODE_OM_TARG_NFCA);
    reg_write(REG_PASSIVE_TARGET, 0x00);
    fifo_write(uid, uidlen); // load UID for the chip's automatic anticollision
    read_irqs();

    std::fputs("Listening as a tag. Present a reader. Ctrl-C / Back to stop.\n", stdout);
    std::fflush(stdout);

    unsigned long activations = 0;
    for (;;) {
        const uint32_t irqs = wait_irq(IRQ_WU_A | IRQ_RXE, 1000);
        if (irqs & (IRQ_WU_A | IRQ_RXE)) {
            std::printf("reader detected (activation #%lu)\n", ++activations);
            std::fflush(stdout);
        }
    }
}

} // namespace
#endif

int rfid_read_main(int argc, char** argv) {
#if defined(__linux__)
    std::string report = "RFID read (ISO14443A)\n";
    if (!open_chip(resolve_spidev(argc, argv, 0), report)) {
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
    if (!open_chip(resolve_spidev(argc, argv, 0), report)) {
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
    if (!open_chip(resolve_spidev(argc, argv, 2), report)) {
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
    if (!open_chip(resolve_spidev(argc, argv, 1), report)) {
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
    if (!open_chip(resolve_spidev(argc, argv, 0), report)) {
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
