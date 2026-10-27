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

// The ST25R3916 sits on the cap's own chip select, which is a plain GPIO on
// this board (G22), not a spidev CS - the NFC driver runs spidev with
// SPI_NO_CS and bit-bangs it. See cap_bringup.h for the full HAT wiring.
#ifndef APP_ST25R_SPIDEV
#define APP_ST25R_SPIDEV "/dev/spidev0.1"
#endif

namespace platform {
namespace {

constexpr uint8_t kSres    = 0x30; // SRES command strobe (reset)
constexpr uint8_t kPartnum = 0x30; // status register PARTNUM
constexpr uint8_t kVersion = 0x31; // status register VERSION
constexpr uint32_t kSpeedHz = 500000; // 500 kHz - the known-good CC1101/ST25R read speed

constexpr uint8_t kIcIdentity  = 0x3F; // ST25R3916 IC_IDENTITY (space-A register)
constexpr uint8_t kIcTypeMask  = 0xF8; // ic_type field = bits [7:3]
constexpr uint8_t kIcType3916  = 0x28; // ic_type 5<<3 (ST25R3916)
constexpr uint8_t kIcType3916b = 0x30; // ic_type 6<<3 (ST25R3916B)
constexpr uint8_t kIcRevMask   = 0x07; // ic_rev field = bits [2:0]

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && v[0] != '\0') ? v : fallback;
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

#endif

} // namespace

std::string cc1101_detect_report(const std::string& spidev_path, bool& detected) {
    detected = false;
    std::string report = "CC1101 CAP detection\nspidev: " + spidev_path +
                         " (CS1/G7 = the cap's CC1101_CS)\n";

#if defined(__linux__)
    report += cap_bringup().text;

    const int fd = open(spidev_path.c_str(), O_RDWR);
    if (fd < 0) {
        report += "error: cannot open spidev (" + std::string(std::strerror(errno)) + ")\n";
        if (errno == ENOENT) {
            report += "hint: SPI is off on this system. Run: sudo raspi-config nonint do_spi 0 "
                      "and reboot, then retry.\n";
        }
        report += "hint: override the node: CC1101_SPIDEV=/dev/spidev0.X\n";
        return report;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = kSpeedHz;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    spi_strobe(fd, kSres);
    usleep(10000); // 10ms for the chip to come out of reset before reading status

    uint8_t partnum = 0;
    uint8_t version = 0;
    const bool ok = spi_status_read(fd, kPartnum, partnum) &&
                    spi_status_read(fd, kVersion, version);
    close(fd);

    if (!ok) {
        report += "error: SPI transfer failed - is this a spidev node?\n";
        return report;
    }

    char line[64];
    std::snprintf(line, sizeof(line),
                  "PARTNUM (0x30): 0x%02X\nVERSION (0x31): 0x%02X\n", partnum, version);
    report += line;

    // A floating/absent MISO reads as all-zeros or all-ones; a real CC1101
    // reports a non-trivial VERSION (0x04/0x14/0x17 across silicon revs).
    detected = (version != 0x00 && version != 0xFF);
    report += detected
                  ? "Result: CC1101 detected. CAP is ready.\n"
                  : "Result: no CC1101 response - check the CAP is seated, that the "
                    "HAT power lines enabled above, and the spidev node.\n";
#else
    (void)detected;
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
            // Hardware CS1 is the cap's CC1101_CS; fall back to the other node
            // if the running kernel did not create it.
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
    std::string report = "ST25R3916 NFC detection\nspidev: " + spidev_path +
                         " (SPI_NO_CS; NFC_CS is GPIO22 on this board)\n";

#if defined(__linux__)
    report += cap_bringup().text;

    // Claim the cap's NFC_CS (active-low GPIO; spidev runs with SPI_NO_CS so
    // neither internal CS - the LCD's G8 or the CC1101's G7 - glitches).
    const std::string cs_chip = env_or("ST25R_CS_CHIP", "/dev/gpiochip0");
    const unsigned cs_line =
        static_cast<unsigned>(std::atoi(env_or("ST25R_CS_GPIO", "22").c_str()));
    int cs_fd = -1;
    if (!gpio_claim_output(cs_chip.c_str(), cs_line, true, &cs_fd)) {
        report += "error: cannot claim NFC CS GPIO" + std::to_string(cs_line) + " on " +
                  cs_chip + " (" + std::strerror(errno) + ")\n";
        return report;
    }
    auto cs = [cs_fd](bool high) { gpio_line_set(cs_fd, high); };

    const int fd = open(spidev_path.c_str(), O_RDWR);
    if (fd < 0) {
        report += "error: cannot open spidev (" + std::string(std::strerror(errno)) + ")\n";
        if (errno == ENOENT) {
            report += "hint: SPI is off on this system. Run: sudo raspi-config nonint do_spi 0 "
                      "and reboot, then retry.\n";
        }
        report += "hint: override the node: ST25R_SPIDEV=/dev/spidev0.X\n";
        return report;
    }

    uint8_t mode = SPI_MODE_1 | SPI_NO_CS; // ST25R3916 SPI mode 1; CS is GPIO22
    uint8_t bits = 8;
    uint32_t speed = kSpeedHz;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    uint8_t id = 0;
    {
        const uint8_t tx[2] = {st25r_reg_read_header(kIcIdentity), 0x00};
        uint8_t rx[2] = {0, 0};
        cs(false);
        const bool ok = spi_xfer(fd, tx, rx, 2) && (id = rx[1], true);
        cs(true);
        if (!ok) {
            report += "error: SPI transfer failed - is this a spidev node?\n";
            close(fd);
            return report;
        }
    }
    close(fd);

    char line[64];
    std::snprintf(line, sizeof(line),
                  "IC_IDENTITY (0x3F): 0x%02X (ic_type=0x%02X rev=%u)\n",
                  id, static_cast<unsigned>(id & kIcTypeMask),
                  static_cast<unsigned>(id & kIcRevMask));
    report += line;

    detected = (id & kIcTypeMask) == kIcType3916 || (id & kIcTypeMask) == kIcType3916b;
    report += detected
                  ? "Result: ST25R3916 detected. NFC CAP is ready.\n"
                  : "Result: no ST25R3916 response - check the CAP seating, the power "
                    "lines above, and the CS GPIO.\n";
#else
    (void)detected;
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
            // SPI_NO_CS: any existing spidev node works.
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
