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
    return (v && v[0] != '\0') ? std::atoi(v) : fallback;
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
    usleep(10000); // 10ms to leave reset before reading status

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
    const bool hw_cs = (cs_gpio == kHwCsGpio);
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

    // CLI diagnostic default (CAP CC1101 profile: ST25R3916 on the G7 HW CS);
    // override with ST25R_CS_GPIO. Runtime pin truth lives in the HAL registry.
    const int cs_gpio = env_int_or("ST25R_CS_GPIO", kSt25rDefaultCs);
    const int rst_gpio = env_int_or("ST25R_RST_GPIO", 26);
    const bool hw_cs = (cs_gpio == kHwCsGpio);

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
                spidev_pick({APP_ST25R_SPIDEV, "/dev/spidev0.1", "/dev/spidev0.0"});
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
