/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "cc1101.h"

#include "cap_bringup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#ifndef APP_CC1101_SPIDEV
#define APP_CC1101_SPIDEV "/dev/spidev0.1"
#endif

#ifndef APP_ST25R_SPIDEV
#define APP_ST25R_SPIDEV "/dev/spidev0.1"
#endif

namespace platform {
namespace {

constexpr uint8_t kSres    = 0x30; // SRES command strobe (reset)
constexpr uint8_t kVersion = 0x31; // status register VERSION
constexpr uint32_t kSpeedHz = 500000; // 500 kHz - known-good CC1101/ST25R read speed
constexpr unsigned kResetSettleUs = 10000;

constexpr uint8_t kIcIdentity  = 0x3F; // ST25R3916 IC_IDENTITY (space-A register)
constexpr uint8_t kIcTypeMask  = 0xF8; // ic_type field = bits [7:3]
constexpr uint8_t kIcType3916  = 0x28; // ic_type 5<<3 (ST25R3916)
constexpr uint8_t kIcType3916b = 0x30; // ic_type 6<<3 (ST25R3916B)
constexpr uint8_t kIcRevMask   = 0x07; // ic_rev field = bits [2:0]

// G7 = SPI0_CE1 = /dev/spidev0.1. A probe whose CS is this GPIO uses the
// kernel's hardware chip-select; any other CS GPIO is bit-banged with SPI_NO_CS
// so the internal LCD (CE0/G8) never glitches.
constexpr int kHwCsGpio = 7;

// Default chip-selects for the standalone `--*-detect` subcommands, matching the
// CAP CC1101 profile confirmed on hardware (CC1101 on the G7 hardware CS; the
// ST25R3916 bit-banged on G22). The dynamic, per-CAP source of truth is the
// BruceModule registry, which the HAL's detect_cap() fills; these are only the
// CLI diagnostic defaults so this low-level layer stays free of a HAL dependency.
constexpr int kCc1101DefaultCs = 7;
constexpr int kSt25rDefaultCs  = 22;

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && v[0] != '\0') ? v : fallback;
}

int env_int_or(const char* name, int fallback) {
    const char* v = std::getenv(name);
    // base 0 => accept decimal and 0x.. hex (e.g. RF_TX_IOCFG0=0x2E).
    return (v && v[0] != '\0') ? static_cast<int>(std::strtol(v, nullptr, 0)) : fallback;
}

#if defined(__linux__)
bool spi_xfer(int fd, const uint8_t* tx, uint8_t* rx, size_t len) {
    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<uintptr_t>(tx);
    tr.rx_buf = reinterpret_cast<uintptr_t>(rx);
    tr.len = static_cast<uint32_t>(len);
    tr.speed_hz = kSpeedHz;
    tr.bits_per_word = 8;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr) >= 1;
}

bool spi_strobe(int fd, uint8_t cmd) {
    uint8_t rx = 0;
    return spi_xfer(fd, &cmd, &rx, 1);
}

bool spi_status_read(int fd, uint8_t addr, uint8_t& out) {
    const uint8_t tx[2] = {cc1101_status_read_header(addr), 0x00};
    uint8_t rx[2] = {0, 0};
    if (!spi_xfer(fd, tx, rx, 2)) {
        return false;
    }
    out = rx[1]; // rx[0] is the chip status byte, rx[1] the register value
    return true;
}

std::string cs_chip() {
    return env_or("CAP_CS_CHIP", "/dev/gpiochip0");
}

// Release (or pulse) a module's reset line. `rst_gpio < 0` is a no-op (returns
// -1). Otherwise claims it as output: for `pulse`, drives low -> high to reset
// the chip; else just drives high to deassert reset. Returns the line handle fd
// (caller closes to release the line) or -1.
int rst_begin(int rst_gpio, bool pulse) {
    if (rst_gpio < 0) {
        return -1;
    }
    int fd = -1;
    if (!gpio_claim_output(cs_chip().c_str(), static_cast<unsigned>(rst_gpio),
                           /*initial_high=*/!pulse, &fd)) {
        return -1;
    }
    if (pulse) {
        usleep(200);             // hold reset asserted (low)
        gpio_line_set(fd, true); // deassert
        usleep(10 * 1000);       // let the chip boot out of reset
    }
    return fd;
}

// Open + configure a spidev node. `no_cs` adds SPI_NO_CS (bit-banged CS path).
// Returns the fd (>=0) or -1 (errno set by open()).
int spi_open(const char* path, uint8_t base_mode, bool no_cs) {
    const int fd = open(path, O_RDWR);
    if (fd < 0) {
        return -1;
    }
    uint8_t mode = static_cast<uint8_t>(base_mode | (no_cs ? SPI_NO_CS : 0));
    uint8_t bits = 8;
    uint32_t speed = kSpeedHz;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    return fd;
}
#endif // __linux__

} // namespace

bool probe_cc1101(const char* spidev_path, int cs_gpio, int rst_gpio, uint8_t& version) {
    version = 0;
#if defined(__linux__)
    const bool hw_cs = (cs_gpio == kHwCsGpio);
    const int fd = spi_open(spidev_path, SPI_MODE_0, /*no_cs=*/!hw_cs);
    if (fd < 0) {
        return false;
    }

    const int rst_fd = rst_begin(rst_gpio, /*pulse=*/false); // deassert reset

    int cs_fd = -1;
    if (!hw_cs) {
        if (!gpio_claim_output(cs_chip().c_str(), static_cast<unsigned>(cs_gpio), true, &cs_fd)) {
            close(fd);
            if (rst_fd >= 0) close(rst_fd);
            return false;
        }
    }
    auto cs = [&](bool high) {
        if (!hw_cs) {
            gpio_line_set(cs_fd, high);
        }
    };

    cs(false);
    spi_strobe(fd, kSres);
    cs(true);
    usleep(kResetSettleUs);

    cs(false);
    const bool ok = spi_status_read(fd, kVersion, version);
    cs(true);

    close(fd);
    if (cs_fd >= 0) {
        close(cs_fd); // release the CS line so a later re-probe can reclaim it
    }
    if (rst_fd >= 0) {
        close(rst_fd);
    }
    // G14/G15 carry the mini-UART (GPS on CAP LORA). Bit-banging one as a CS
    // (Pingequa) switches it to GPIO and the kernel never reverts it, so restore
    // the serial mux (ALT5) on the way out — required so a later LORA keeps GPS.
    if (cs_gpio == 14 || cs_gpio == 15) {
        gpio_restore_serial_mux();
    }
    return ok && cc1101_version_valid(version);
#else
    (void)spidev_path;
    (void)cs_gpio;
    (void)rst_gpio;
    return false;
#endif
}

bool probe_st25r(const char* spidev_path, int cs_gpio, int rst_gpio, uint8_t& ic_identity) {
    ic_identity = 0;
#if defined(__linux__)
    const bool hw_cs = cs_gpio == kHwCsGpio || std::strcmp(spidev_path, "/dev/spidev0.2") == 0;
    const int fd = spi_open(spidev_path, SPI_MODE_1, /*no_cs=*/!hw_cs);
    if (fd < 0) {
        return false;
    }

    const int rst_fd = rst_begin(rst_gpio, /*pulse=*/false); // deassert reset

    int cs_fd = -1;
    if (!hw_cs) {
        if (!gpio_claim_output(cs_chip().c_str(), static_cast<unsigned>(cs_gpio), true, &cs_fd)) {
            close(fd);
            if (rst_fd >= 0) close(rst_fd);
            return false;
        }
    }
    auto cs = [&](bool high) {
        if (!hw_cs) {
            gpio_line_set(cs_fd, high);
        }
    };

    const uint8_t tx[2] = {st25r_reg_read_header(kIcIdentity), 0x00};
    uint8_t rx[2] = {0, 0};
    cs(false);
    const bool ok = spi_xfer(fd, tx, rx, 2);
    cs(true);
    ic_identity = rx[1];

    close(fd);
    if (cs_fd >= 0) {
        close(cs_fd);
    }
    if (rst_fd >= 0) {
        close(rst_fd);
    }
    return ok && ((ic_identity & kIcTypeMask) == kIcType3916 ||
                  (ic_identity & kIcTypeMask) == kIcType3916b);
#else
    (void)spidev_path;
    (void)cs_gpio;
    (void)rst_gpio;
    return false;
#endif
}

bool probe_sx1262(const char* spidev_path, int cs_gpio, int busy_gpio, int rst_gpio,
                  uint16_t& scratch, uint8_t& status) {
    scratch = 0;
    status = 0;
#if defined(__linux__)
    const bool hw_cs = (cs_gpio == kHwCsGpio);
    const int fd = spi_open(spidev_path, SPI_MODE_0, /*no_cs=*/!hw_cs);
    if (fd < 0) {
        return false;
    }

    int cs_fd = -1;
    if (!hw_cs) {
        if (!gpio_claim_output(cs_chip().c_str(), static_cast<unsigned>(cs_gpio), true, &cs_fd)) {
            close(fd);
            return false;
        }
    }
    auto cs = [&](bool high) {
        if (!hw_cs) {
            gpio_line_set(cs_fd, high);
        }
    };

    const int rst_fd = rst_begin(rst_gpio, /*pulse=*/true); // SX126x hardware reset

    // Poll BUSY low before each command (SX126x holds BUSY high while busy).
    auto wait_busy = [&]() {
        if (busy_gpio < 0) {
            usleep(1000);
            return;
        }
        for (int i = 0; i < 50; ++i) {
            bool b = true;
            if (gpio_read_line(cs_chip().c_str(), static_cast<unsigned>(busy_gpio), b) && !b) {
                return;
            }
            usleep(1000);
        }
    };

    // GetStatus (0xC0) - diagnostic only.
    wait_busy();
    {
        uint8_t tx[2] = {0xC0, 0x00};
        uint8_t rx[2] = {0, 0};
        cs(false);
        spi_xfer(fd, tx, rx, 2);
        cs(true);
        status = rx[1];
    }

    auto write_reg = [&](uint16_t addr, uint8_t hi, uint8_t lo) {
        wait_busy();
        uint8_t tx[5] = {0x0D, static_cast<uint8_t>(addr >> 8),
                         static_cast<uint8_t>(addr & 0xFF), hi, lo};
        uint8_t rx[5] = {0};
        cs(false);
        spi_xfer(fd, tx, rx, 5);
        cs(true);
    };
    auto read_reg = [&](uint16_t addr, uint8_t& hi, uint8_t& lo) {
        wait_busy();
        // 0x1D, addr_hi, addr_lo, NOP(status), data_hi, data_lo
        uint8_t tx[6] = {0x1D, static_cast<uint8_t>(addr >> 8),
                         static_cast<uint8_t>(addr & 0xFF), 0, 0, 0};
        uint8_t rx[6] = {0};
        cs(false);
        spi_xfer(fd, tx, rx, 6);
        cs(true);
        hi = rx[4];
        lo = rx[5];
    };

    // Scratch read/write test on the LoRa sync-word register (0x0740): write two
    // complementary patterns and read each back. A floating/absent bus cannot
    // echo both, so a double match is a strong SX126x signature.
    constexpr uint16_t kSyncWord = 0x0740;
    uint8_t rhi = 0;
    uint8_t rlo = 0;

    write_reg(kSyncWord, 0xAA, 0x55);
    read_reg(kSyncWord, rhi, rlo);
    const bool p1 = (rhi == 0xAA && rlo == 0x55);

    write_reg(kSyncWord, 0x55, 0xAA);
    read_reg(kSyncWord, rhi, rlo);
    const bool p2 = (rhi == 0x55 && rlo == 0xAA);
    scratch = static_cast<uint16_t>((static_cast<uint16_t>(rhi) << 8) | rlo);

    write_reg(kSyncWord, 0x14, 0x24); // restore reset-default sync word

    close(fd);
    if (cs_fd >= 0) {
        close(cs_fd);
    }
    if (rst_fd >= 0) {
        close(rst_fd);
    }
    return p1 && p2;
#else
    (void)spidev_path;
    (void)cs_gpio;
    (void)busy_gpio;
    (void)rst_gpio;
    return false;
#endif
}

bool probe_nrf24(const char* spidev_path, int cs_gpio, int ce_gpio,
                 uint8_t& status, uint8_t& config) {
    status = 0;
    config = 0;
#if defined(__linux__)
    const bool hw_cs = (cs_gpio == kHwCsGpio);
    const int fd = spi_open(spidev_path, SPI_MODE_0, /*no_cs=*/!hw_cs);
    if (fd < 0) {
        return false;
    }

    int cs_fd = -1;
    if (!hw_cs) {
        if (!gpio_claim_output(cs_chip().c_str(), static_cast<unsigned>(cs_gpio), true, &cs_fd)) {
            close(fd);
            return false;
        }
    }
    auto cs = [&](bool high) {
        if (!hw_cs) {
            gpio_line_set(cs_fd, high);
        }
    };

    // Hold CE low (standby) so the radio stays in a quiescent, SPI-friendly state.
    int ce_fd = -1;
    if (ce_gpio >= 0) {
        gpio_claim_output(cs_chip().c_str(), static_cast<unsigned>(ce_gpio), false, &ce_fd);
    }

    // Scratch test on TX_ADDR (reg 0x10, 5 bytes): write a pattern, read it back.
    constexpr uint8_t kTxAddr = 0x10;
    constexpr uint8_t kWReg = 0x20;  // W_REGISTER command base
    const uint8_t pat[5] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5};

    {
        uint8_t tx[6] = {static_cast<uint8_t>(kWReg | kTxAddr), pat[0], pat[1], pat[2], pat[3], pat[4]};
        uint8_t rx[6] = {0};
        cs(false);
        spi_xfer(fd, tx, rx, 6);
        cs(true);
    }
    uint8_t rb[5] = {0};
    {
        uint8_t tx[6] = {kTxAddr, 0, 0, 0, 0, 0};
        uint8_t rx[6] = {0};
        cs(false);
        spi_xfer(fd, tx, rx, 6);
        cs(true);
        status = rx[0]; // STATUS is shifted out during the command byte
        rb[0] = rx[1]; rb[1] = rx[2]; rb[2] = rx[3]; rb[3] = rx[4]; rb[4] = rx[5];
    }
    // CONFIG (reg 0x00) for diagnostics.
    {
        uint8_t tx[2] = {0x00, 0x00};
        uint8_t rx[2] = {0, 0};
        cs(false);
        spi_xfer(fd, tx, rx, 2);
        cs(true);
        config = rx[1];
    }

    close(fd);
    if (cs_fd >= 0) close(cs_fd);
    if (ce_fd >= 0) close(ce_fd);

    return std::memcmp(rb, pat, sizeof(pat)) == 0;
#else
    (void)spidev_path;
    (void)cs_gpio;
    (void)ce_gpio;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// CC1101 radio session (OOK async) — M6
// ---------------------------------------------------------------------------
namespace {

// CC1101 register addresses (config space 0x00-0x2E) + strobes/status used here.
enum : uint8_t {
    R_IOCFG2 = 0x00, R_IOCFG0 = 0x02, R_FIFOTHR = 0x03,
    R_PKTLEN = 0x06, R_PKTCTRL1 = 0x07, R_PKTCTRL0 = 0x08, R_ADDR = 0x09, R_CHANNR = 0x0A,
    R_FSCTRL1 = 0x0B, R_FSCTRL0 = 0x0C, R_FREQ2 = 0x0D, R_FREQ1 = 0x0E, R_FREQ0 = 0x0F,
    R_MDMCFG4 = 0x10, R_MDMCFG3 = 0x11, R_MDMCFG2 = 0x12, R_MDMCFG1 = 0x13, R_MDMCFG0 = 0x14,
    R_DEVIATN = 0x15, R_MCSM0 = 0x18, R_FOCCFG = 0x19, R_BSCFG = 0x1A,
    R_AGCCTRL2 = 0x1B, R_AGCCTRL1 = 0x1C, R_AGCCTRL0 = 0x1D,
    R_FREND1 = 0x21, R_FREND0 = 0x22,
    R_FSCAL3 = 0x23, R_FSCAL2 = 0x24, R_FSCAL1 = 0x25, R_FSCAL0 = 0x26,
    R_FSTEST = 0x29, R_TEST2 = 0x2C, R_TEST1 = 0x2D, R_TEST0 = 0x2E,
    S_SRES = 0x30, S_SCAL = 0x33, S_SRX = 0x34, S_STX = 0x35, S_SIDLE = 0x36, S_SFRX = 0x3A,
    ST_RSSI = 0x34, ST_MARCSTATE = 0x35, // status registers (burst-read header)
};
constexpr uint8_t kReadSingle = 0x80; // read header for a single config register

void radio_cs(const Cc1101Radio& r, bool high) {
    if (!r.hw_cs && r.cs_fd >= 0) gpio_line_set(r.cs_fd, high);
}

} // namespace

void cc1101_reg_write(const Cc1101Radio& r, uint8_t addr, uint8_t value) {
#if defined(__linux__)
    const uint8_t tx[2] = {addr, value};
    uint8_t rx[2] = {0, 0};
    radio_cs(r, false);
    spi_xfer(r.fd, tx, rx, 2);
    radio_cs(r, true);
#else
    (void)r; (void)addr; (void)value;
#endif
}

uint8_t cc1101_reg_read(const Cc1101Radio& r, uint8_t addr) {
#if defined(__linux__)
    const uint8_t tx[2] = {static_cast<uint8_t>(addr | kReadSingle), 0x00};
    uint8_t rx[2] = {0, 0};
    radio_cs(r, false);
    spi_xfer(r.fd, tx, rx, 2);
    radio_cs(r, true);
    return rx[1];
#else
    (void)r; (void)addr; return 0;
#endif
}

uint8_t cc1101_status_read(const Cc1101Radio& r, uint8_t addr) {
#if defined(__linux__)
    const uint8_t tx[2] = {cc1101_status_read_header(addr), 0x00};
    uint8_t rx[2] = {0, 0};
    radio_cs(r, false);
    spi_xfer(r.fd, tx, rx, 2);
    radio_cs(r, true);
    return rx[1];
#else
    (void)r; (void)addr; return 0;
#endif
}

void cc1101_cmd(const Cc1101Radio& r, uint8_t strobe) {
#if defined(__linux__)
    radio_cs(r, false);
    spi_strobe(r.fd, strobe);
    radio_cs(r, true);
#else
    (void)r; (void)strobe;
#endif
}

void cc1101_reset_chip(const Cc1101Radio& r) {
#if defined(__linux__)
    cc1101_cmd(r, S_SRES);
    // spidev owns CS and MISO. Allow reset to finish before sending the first
    // config writes, using the same settling time as the presence probe.
    usleep(kResetSettleUs);
#else
    (void)r;
#endif
}

void cc1101_rx(const Cc1101Radio& r) { cc1101_cmd(r, S_SRX); }
void cc1101_tx(const Cc1101Radio& r) { cc1101_cmd(r, S_STX); }
void cc1101_idle(const Cc1101Radio& r) { cc1101_cmd(r, S_SIDLE); }

uint8_t cc1101_marcstate(const Cc1101Radio& r) {
    return static_cast<uint8_t>(cc1101_status_read(r, ST_MARCSTATE) & 0x1F);
}

int cc1101_rssi_dbm(const Cc1101Radio& r) {
#if defined(__linux__)
    const int raw = cc1101_status_read(r, ST_RSSI);
    return raw >= 128 ? (raw - 256) / 2 - 74 : raw / 2 - 74;
#else
    (void)r; return 0;
#endif
}

namespace {

// Integer map(), matching Arduino's map() (truncating division) used by the
// ELECHOUSE driver's Calibrate() to interpolate FSCTRL0.
int cc1101_map_i(int x, int in_lo, int in_hi, int out_lo, int out_hi) {
    if (x <= in_lo) return out_lo;
    if (x >= in_hi) return out_hi;
    return (x - in_lo) * (out_hi - out_lo) / (in_hi - in_lo) + out_lo;
}

// Port of ELECHOUSE SmartRC-CC1101-Driver-Lib Calibrate(): the active,
// per-frequency synthesizer trim the M5/Bruce build runs inside setMHZ() (and
// again in cc1101ApplyPreciseCalibration()). Our earlier port only wrote a
// static register image and skipped this, so FSCTRL0 stayed at its reset 0x00
// instead of the band-interpolated crystal-offset trim (~0x23 / +55 kHz at
// 433.92). A wide RX bandwidth masked the offset; the narrow 270 kHz OOK preset
// does not, which silently desensitized RX (flat noise floor) while TX, which
// tolerates the miscalibration, kept working. This writes:
//   - FSCTRL0 = frequency-offset trim, interpolated over the band (endpoints
//     match Bruce's setClb(): 315->24..28, 433->31..38, 868->65..76,
//     915->77..79);
//   - TEST0   = 0x09 for the upper (high-VCO) half of each band, else 0x0B;
//   - high-VCO FSCAL2 nudge (+0x20) so the PLL locks on the correct VCO core.
// The chip must be in IDLE (the caller strobes SIDLE before changing FREQ).
void cc1101_calibrate(const Cc1101Radio& r, float mhz) {
    // Diagnostics / tuning overrides (no rebuild needed):
    //   RF_CALIB=0     -> skip this routine entirely (leave FSCTRL0 at reset 0).
    //   RF_FSCTRL0=<n> -> force FSCTRL0 to <n> instead of the band interpolation.
    if (const char* v = std::getenv("RF_CALIB"); v && v[0] == '0') return;
    const int m = static_cast<int>(mhz);
    int in_lo, in_hi, clb_lo, clb_hi;
    float high_edge; // frequencies >= this use the high VCO core; 0 = always high
    if (m >= 280 && m <= 348) {
        in_lo = 280; in_hi = 348; clb_lo = 24; clb_hi = 28; high_edge = 322.88f;
    } else if (m >= 378 && m <= 464) {
        in_lo = 378; in_hi = 464; clb_lo = 31; clb_hi = 38; high_edge = 430.5f;
    } else if (m >= 779 && m <= 899) {
        in_lo = 779; in_hi = 899; clb_lo = 65; clb_hi = 76; high_edge = 861.0f;
    } else if (m >= 900 && m <= 928) {
        in_lo = 900; in_hi = 928; clb_lo = 77; clb_hi = 79; high_edge = 0.0f;
    } else {
        return; // out of the supported bands: leave reset defaults
    }
    // FSCTRL0 is the FREQOFF crystal trim. Bruce's band-interpolated value
    // (cc1101_map_i) is calibrated for the M5/T-Embed crystal; applying it to the
    // CardputerZero CAP (a different, accurate crystal) shifts the carrier ~55 kHz
    // off 433.92 and made the MODULATED TX invisible to a receiver tuned to the
    // nominal frequency (continuous-carrier RSSI still showed a peak, which hid
    // it). The CZ CAP needs NO trim, so default FSCTRL0 to 0; RF_FSCTRL0 can set
    // an explicit trim for a board that needs one. (void the computed map value.)
    int fsctrl0 = 0; // default: no trim (CZ CAP crystal is accurate)
    if (const char* v = std::getenv("RF_FSCTRL0"); v && v[0] != '\0') {
        fsctrl0 = (std::strcmp(v, "auto") == 0) ? cc1101_map_i(m, in_lo, in_hi, clb_lo, clb_hi)
                                                : static_cast<int>(std::strtol(v, nullptr, 0));
    }
    cc1101_reg_write(r, R_FSCTRL0, static_cast<uint8_t>(fsctrl0));
    const bool high_vco = (high_edge == 0.0f) || (mhz >= high_edge);
    if (high_vco) {
        cc1101_reg_write(r, R_TEST0, 0x09);
        const uint8_t s = cc1101_reg_read(r, R_FSCAL2);
        if (s < 0x20) cc1101_reg_write(r, R_FSCAL2, static_cast<uint8_t>(s + 0x20));
    } else {
        cc1101_reg_write(r, R_TEST0, 0x0B);
    }
}

} // namespace

void cc1101_set_frequency(Cc1101Radio& r, float mhz) {
#if defined(__linux__)
    r.mhz = mhz;
    // Frequency word decomposition, bit-identical to the M5/ELECHOUSE setMHZ():
    // f_carrier = 26MHz * FREQ / 2^16, built up by subtracting the per-register
    // step sizes. Keep byte arithmetic so rounding matches the reference exactly.
    uint8_t freq2 = 0, freq1 = 0, freq0 = 0;
    float m = mhz;
    for (bool done = false; !done;) {
        if (m >= 26) { m -= 26; freq2++; }
        else if (m >= 0.1015625f) { m -= 0.1015625f; freq1++; }
        else if (m >= 0.00039675f) { m -= 0.00039675f; freq0++; }
        else done = true;
    }

    cc1101_idle(r); // FREQ regs may only change while the synthesizer is off
    cc1101_reg_write(r, R_FREQ2, freq2);
    cc1101_reg_write(r, R_FREQ1, freq1);
    cc1101_reg_write(r, R_FREQ0, freq0);

    // Active per-frequency synthesizer trim (ELECHOUSE Calibrate()): sets
    // FSCTRL0/TEST0 and the high-VCO FSCAL2 nudge. Missing this was what killed
    // RX sensitivity under the narrow OOK bandwidth. Runs here (chip in IDLE)
    // right after FREQ, exactly as setMHZ() does in the reference driver.
    cc1101_calibrate(r, mhz);

    // Antenna path (M5 CAP CC1101): SP3T switches fed by SW0 (a GPIO) and SW1
    // (= CC1101 GDO2, forced through IOCFG2). Electrical levels measured on the
    // CardputerZero CAP (433 was verified over the air against a T-Embed):
    //   315 MHz       SW0=0 SW1=1
    //   433 MHz       SW0=1 SW1=0
    //   868/915 MHz   SW0=1 SW1=1
    // These levels differ from the logical switch labels in M5's public table,
    // implying an inversion in the CAP's control path.
    // IOCFG2=0x2F forces GDO2 low; bit 6 inverts it, so 0x6F forces it high.
    const int band = mhz < 374 ? 0 : (mhz < 650.5f ? 1 : 2); // 315 / 433 / 868-915
    bool sw0 = band != 0;
    bool sw1 = band != 1;
    if (const char* value = std::getenv("RF_SW0"); value && value[0] != '\0') {
        sw0 = std::atoi(value) != 0;
    }
    if (const char* value = std::getenv("RF_SW1"); value && value[0] != '\0') {
        sw1 = std::atoi(value) != 0;
    }
    if (r.sw0_fd >= 0) gpio_line_set(r.sw0_fd, sw0);
    cc1101_reg_write(r, R_IOCFG2, sw1 ? 0x6F : 0x2F);
#else
    (void)r; (void)mhz;
#endif
}

void cc1101_config_ook_async_rx(Cc1101Radio& r, float mhz) {
#if defined(__linux__)
    cc1101_reset_chip(r);

    // OOK/ASK, serial ASYNC: demodulated data appears on GDO0 (IOCFG0=0x0D),
    // packet engine bypassed (PKTCTRL0=0x32, infinite length). The modem/AGC
    // values are Bruce's tuned fixed-frequency OOK RX preset
    // (cc1101ApplyFixedFreqOokPreset), NOT the wide generic scan profile — that
    // is what keeps ambient noise out of the capture:
    //   MDMCFG4=0x67 -> RX BW ~270 kHz (CHANBW_E=1,M=2), not 812 kHz;
    //   MDMCFG3=0x32 -> data rate ~3.8 kBaud, matched to remote pulse widths;
    //   AGCCTRL0=0x40/AGCCTRL1=0x01/FREND1=0xB6/FIFOTHR=0x07 -> OOK AGC that does
    //   not crank gain into noise during the inter-frame silence.
    cc1101_reg_write(r, R_FSCTRL1, 0x06);
    cc1101_reg_write(r, R_IOCFG0, 0x0D);   // GDO0 = async serial data output
    cc1101_reg_write(r, R_PKTCTRL0, 0x32); // async serial mode, infinite packet
    cc1101_reg_write(r, R_PKTCTRL1, 0x04);
    cc1101_reg_write(r, R_PKTLEN, 0x00);
    cc1101_reg_write(r, R_ADDR, 0x00);
    cc1101_reg_write(r, R_CHANNR, 0x00);
    // FIFOTHR bits [5:4] = CLOSE_IN_RX attenuation (0/6/12/18 dB): raise it via
    // RF_RX_CLOSE_IN (0..3) when a strong nearby transmitter saturates the front
    // end (close-range bench test), which otherwise garbles the OOK envelope.
    const int close_in = env_int_or("RF_RX_CLOSE_IN", 0) & 0x3;
    cc1101_reg_write(r, R_FIFOTHR, static_cast<uint8_t>(0x07 | (close_in << 4)));
    cc1101_reg_write(r, R_MDMCFG4, 0x67);  // RX BW ~270 kHz (narrow -> less noise)
    cc1101_reg_write(r, R_MDMCFG3, 0x32);  // data rate ~3.8 kBaud (OOK remotes)
    cc1101_reg_write(r, R_MDMCFG2, 0x30);  // ASK/OOK, no sync, no manchester
    cc1101_reg_write(r, R_MDMCFG1, 0x00);
    cc1101_reg_write(r, R_MDMCFG0, 0x00);
    cc1101_reg_write(r, R_DEVIATN, 0x47);
    cc1101_reg_write(r, R_MCSM0, 0x18);    // auto-calibrate when going IDLE->RX
    cc1101_reg_write(r, R_FOCCFG, 0x18);
    cc1101_reg_write(r, R_BSCFG, 0x1C);
    cc1101_reg_write(r, R_AGCCTRL2, 0xC7);
    cc1101_reg_write(r, R_AGCCTRL1, 0x01); // OOK AGC: carrier-sense threshold
    cc1101_reg_write(r, R_AGCCTRL0, 0x40); // OOK AGC: filter/hysteresis/wait
    cc1101_reg_write(r, R_FREND1, 0xB6);   // OOK RX front-end
    cc1101_reg_write(r, R_FREND0, 0x11);   // ASK front-end
    cc1101_reg_write(r, R_FSCAL3, 0xE9);
    cc1101_reg_write(r, R_FSCAL2, 0x2A);
    cc1101_reg_write(r, R_FSCAL1, 0x00);
    cc1101_reg_write(r, R_FSCAL0, 0x1F);
    cc1101_reg_write(r, R_FSTEST, 0x59);
    cc1101_reg_write(r, R_TEST2, 0x81);
    cc1101_reg_write(r, R_TEST1, 0x35);
    cc1101_reg_write(r, R_TEST0, 0x09);

    cc1101_set_frequency(r, mhz);
#else
    (void)r; (void)mhz;
#endif
}

void cc1101_set_patable(const Cc1101Radio& r, uint8_t off_level, uint8_t on_level) {
#if defined(__linux__)
    // PATABLE burst (0x3E): index 0 = data '0' level, index 1 = data '1' level.
    // Setting both equal forces an UNMODULATED carrier (diagnostic), independent
    // of the GDO0 async data line.
    const uint8_t tx[3] = {static_cast<uint8_t>(0x3E | 0x40), off_level, on_level};
    uint8_t rx[3] = {0, 0, 0};
    radio_cs(r, false);
    spi_xfer(r.fd, tx, rx, 3);
    radio_cs(r, true);
#else
    (void)r; (void)off_level; (void)on_level;
#endif
}

void cc1101_config_ook_async_tx(Cc1101Radio& r, float mhz) {
#if defined(__linux__)
    cc1101_reset_chip(r);

    // Same OOK modem as RX, but TX-side: GDO0 is automatically made the async
    // DATA INPUT by the CC1101 while TX is active. Keep IOCFG0=0x0D, matching
    // ELECHOUSE setCCMode(false); forcing 0x2E here prevented modulation on the
    // M5 CAP even though MARCSTATE reached TX. FREND0=0x11 selects PATABLE[1].
    cc1101_reg_write(r, R_FSCTRL1, 0x06);
    const uint8_t iocfg0 = static_cast<uint8_t>(env_int_or("RF_TX_IOCFG0", 0x0D) & 0xFF);
    cc1101_reg_write(r, R_IOCFG0, iocfg0); // GDO0 becomes the async TX input in TX state
    cc1101_reg_write(r, R_PKTCTRL0, 0x32); // async serial mode, infinite packet
    cc1101_reg_write(r, R_FIFOTHR, 0x47);
    cc1101_reg_write(r, R_MDMCFG4, 0x67);
    cc1101_reg_write(r, R_MDMCFG3, 0x32);
    cc1101_reg_write(r, R_MDMCFG2, 0x30);  // ASK/OOK
    cc1101_reg_write(r, R_MDMCFG1, 0x00);
    cc1101_reg_write(r, R_MDMCFG0, 0x00);
    cc1101_reg_write(r, R_DEVIATN, 0x47);
    cc1101_reg_write(r, R_MCSM0, 0x18);
    cc1101_reg_write(r, R_FOCCFG, 0x18);
    cc1101_reg_write(r, R_FREND0, 0x11);   // ASK: PA_POWER index 1 = ON level
    cc1101_reg_write(r, R_FSCAL3, 0xE9);
    cc1101_reg_write(r, R_FSCAL2, 0x2A);
    cc1101_reg_write(r, R_FSCAL1, 0x00);
    cc1101_reg_write(r, R_FSCAL0, 0x1F);
    cc1101_reg_write(r, R_FSTEST, 0x59);
    cc1101_reg_write(r, R_TEST2, 0x81);
    cc1101_reg_write(r, R_TEST1, 0x35);
    cc1101_reg_write(r, R_TEST0, 0x09);

    // PATABLE burst (0x3E): OOK keys the carrier between index 0 (off) and index
    // 1 (ON power). 0xC0 ≈ +10..12 dBm at 433 MHz (ELECHOUSE PA table top entry).
    // RF_TX_POWER overrides the ON byte for bench tuning.
    const uint8_t on = static_cast<uint8_t>(env_int_or("RF_TX_POWER", 0xC0) & 0xFF);
    const uint8_t pa_tx[9] = {static_cast<uint8_t>(0x3E | 0x40),
                              0x00, on, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t pa_rx[9] = {0};
    radio_cs(r, false);
    // Only the two OOK levels are transferred. Sending all eight entries on
    // this CAP made the PA go silent even though MARCSTATE remained TX.
    spi_xfer(r.fd, pa_tx, pa_rx, 3);
    radio_cs(r, true);

    cc1101_set_frequency(r, mhz);
#else
    (void)r; (void)mhz;
#endif
}

bool cc1101_radio_open(Cc1101Radio& r, const char* spidev_path,
                       int cs_gpio, int gdo0_gpio, int sw0_gpio, int rst_gpio) {
    r = Cc1101Radio{};
    r.cs_gpio = cs_gpio;
    r.gdo0_gpio = gdo0_gpio;
    r.sw0_gpio = sw0_gpio;
    r.rst_gpio = rst_gpio;
    r.hw_cs = (cs_gpio == kHwCsGpio);
#if defined(__linux__)
    r.fd = spi_open(spidev_path, SPI_MODE_0, /*no_cs=*/!r.hw_cs);
    if (r.fd < 0) {
        r = Cc1101Radio{};
        return false;
    }
    if (!r.hw_cs) {
        if (!gpio_claim_output(cs_chip().c_str(), static_cast<unsigned>(cs_gpio), true, &r.cs_fd)) {
            close(r.fd);
            r = Cc1101Radio{};
            return false;
        }
    }
    if (sw0_gpio >= 0) {
        gpio_claim_output(cs_chip().c_str(), static_cast<unsigned>(sw0_gpio), true, &r.sw0_fd);
    }
    r.rst_fd = rst_begin(rst_gpio, /*pulse=*/true); // hardware reset, leave deasserted
    return true;
#else
    (void)spidev_path;
    return false;
#endif
}

void cc1101_radio_close(Cc1101Radio& r) {
#if defined(__linux__)
    if (r.fd >= 0) close(r.fd);
    if (r.cs_fd >= 0) close(r.cs_fd);
    if (r.sw0_fd >= 0) close(r.sw0_fd);
    if (r.rst_fd >= 0) close(r.rst_fd);
    // Same mux contract as the probes: if a CC1101 CS or GDO0 lived on G14/G15
    // (CAP CC1101), hand the mini-UART pins back to ALT5 on the way out.
    if (r.cs_gpio == 14 || r.cs_gpio == 15 || r.gdo0_gpio == 14 || r.gdo0_gpio == 15) {
        gpio_restore_serial_mux();
    }
#endif
    r = Cc1101Radio{};
}

std::string cc1101_detect_report(const std::string& spidev_path, bool& detected) {
    detected = false;

    // CLI diagnostic default (CAP CC1101 profile: CC1101 bit-banged on G22);
    // override with CC1101_CS_GPIO. Runtime pin truth lives in the HAL registry.
    const int cs_gpio = env_int_or("CC1101_CS_GPIO", kCc1101DefaultCs);
    const int rst_gpio = env_int_or("CC1101_RST_GPIO", 26);
    const bool hw_cs = (cs_gpio == kHwCsGpio);

    std::string report = "CC1101 CAP detection\nspidev: " + spidev_path + "\n";
    report += "CC1101_CS = G" + std::to_string(cs_gpio) +
              (hw_cs ? " (hardware CS)\n" : " (GPIO bit-bang, SPI_NO_CS)\n");

#if defined(__linux__)
    report += cap_bringup().text;

    uint8_t version = 0;
    detected = probe_cc1101(spidev_path.c_str(), cs_gpio, rst_gpio, version);

    char line[96];
    std::snprintf(line, sizeof(line), "VERSION (0x31): 0x%02X\n", version);
    report += line;

    report += detected
                  ? "Result: CC1101 detected. CAP is ready.\n"
                  : "Result: no CC1101 response - check the CAP is seated, the HAT "
                    "power lines enabled above, the spidev node, and CC1101_CS_GPIO.\n";
#else
    report += "error: spidev is only available on Linux builds.\n";
#endif

    return report;
}

int cc1101_detect_main(int argc, char** argv) {
    std::string path;
    if (argc >= 1 && argv[0] && argv[0][0] != '\0') {
        path = argv[0];
    } else {
        const char* env = std::getenv("CC1101_SPIDEV");
        if (env && env[0] != '\0') {
            path = env;
        } else {
            const std::string picked =
                spidev_pick({APP_CC1101_SPIDEV, "/dev/spidev0.1", "/dev/spidev0.0"});
            path = picked.empty() ? std::string(APP_CC1101_SPIDEV) : picked;
        }
    }

    bool detected = false;
    const std::string report = cc1101_detect_report(path, detected);
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return detected ? 0 : 1;
}

std::string st25r_detect_report(const std::string& spidev_path, bool& detected) {
    detected = false;

    // CLI diagnostic default (CAP CC1101 profile: ST25R3916 on G22);
    // override with ST25R_CS_GPIO. Runtime pin truth lives in the HAL registry.
    const int cs_gpio = env_int_or("ST25R_CS_GPIO", kSt25rDefaultCs);
    const int rst_gpio = env_int_or("ST25R_RST_GPIO", 26);
    const bool hw_cs = cs_gpio == kHwCsGpio || spidev_path == "/dev/spidev0.2";

    std::string report = "ST25R3916 NFC detection\nspidev: " + spidev_path + "\n";
    report += "NFC_CS = G" + std::to_string(cs_gpio) +
              (hw_cs ? " (hardware CS)\n" : " (GPIO bit-bang, SPI_NO_CS)\n");

#if defined(__linux__)
    report += cap_bringup().text;

    uint8_t id = 0;
    detected = probe_st25r(spidev_path.c_str(), cs_gpio, rst_gpio, id);

    char line[96];
    std::snprintf(line, sizeof(line),
                  "IC_IDENTITY (0x3F): 0x%02X (ic_type=0x%02X rev=%u)\n",
                  id, static_cast<unsigned>(id & kIcTypeMask),
                  static_cast<unsigned>(id & kIcRevMask));
    report += line;

    report += detected
                  ? "Result: ST25R3916 detected. NFC CAP is ready.\n"
                  : "Result: no ST25R3916 response - check the CAP seating, the power "
                    "lines above, the spidev node, and ST25R_CS_GPIO.\n";
#else
    report += "error: spidev is only available on Linux builds.\n";
#endif

    return report;
}

int st25r_detect_main(int argc, char** argv) {
    std::string path;
    if (argc >= 1 && argv[0] && argv[0][0] != '\0') {
        path = argv[0];
    } else {
        const char* env = std::getenv("ST25R_SPIDEV");
        if (env && env[0] != '\0') {
            path = env;
        } else {
            const std::string picked =
                spidev_pick({"/dev/spidev0.2", APP_ST25R_SPIDEV, "/dev/spidev0.1", "/dev/spidev0.0"});
            path = picked.empty() ? std::string(APP_ST25R_SPIDEV) : picked;
        }
    }

    bool detected = false;
    const std::string report = st25r_detect_report(path, detected);
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return detected ? 0 : 1;
}

} // namespace platform
