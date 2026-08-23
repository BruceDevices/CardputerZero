/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "cc1101.h"

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
#define APP_CC1101_SPIDEV "/dev/spidev0.0"
#endif

namespace platform {
namespace {

constexpr uint8_t kSres    = 0x30; // SRES command strobe (reset)
constexpr uint8_t kPartnum = 0x30; // status register PARTNUM
constexpr uint8_t kVersion = 0x31; // status register VERSION
constexpr uint32_t kSpeedHz = 1000000; // 1 MHz - conservative, valid for reads

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
    std::string report = "CC1101 CAP detection\nspidev: " + spidev_path + "\n";

#if defined(__linux__)
    const int fd = open(spidev_path.c_str(), O_RDWR);
    if (fd < 0) {
        report += "error: cannot open spidev (" + std::string(std::strerror(errno)) + ")\n";
        report += "hint: enable the SPI overlay and pass the right node, e.g.\n"
                  "      CC1101_SPIDEV=/dev/spidev0.1\n";
        return report;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = kSpeedHz;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    spi_strobe(fd, kSres);
    usleep(1000); // let the chip come out of reset before reading status regs

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
                  : "Result: no CC1101 response - check the CAP is seated, the "
                    "spidev node, and wiring.\n";
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
        path = (env && env[0] != '\0') ? env : APP_CC1101_SPIDEV;
    }

    bool detected = false;
    const std::string report = cc1101_detect_report(path, detected);
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return detected ? 0 : 1;
}

} // namespace platform
