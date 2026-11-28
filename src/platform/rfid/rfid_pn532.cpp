/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * See rfid_pn532.h. Native GetFirmwareVersion probe over I2C and SPI.
 */

#include "rfid_pn532.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rfid_i2c.h"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#endif

namespace platform {

#if defined(__linux__)
namespace {

constexpr uint8_t kI2cAddr = 0x24; // PN532 7-bit I2C address (0x48 >> 1)

// GetFirmwareVersion information frame and the PN532 ACK frame.
constexpr uint8_t kGfvFrame[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
constexpr uint8_t kAck[6]     = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

void sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

// ---- I2C: write the command, then poll the (RDY + frame) read for the ACK ----
bool probe_i2c_bus(const char* dev) {
    int fd = i2c_open(dev, kI2cAddr);
    if (fd < 0) return false;
    bool ok = false;
    if (i2c_write(fd, kGfvFrame, sizeof(kGfvFrame))) {
        // PN532 I2C reads are prefixed by a ready byte (bit0 = 1 when ready).
        for (int attempt = 0; attempt < 20 && !ok; ++attempt) {
            uint8_t buf[7] = {0};
            if (i2c_read(fd, buf, sizeof(buf)) && (buf[0] & 0x01)) {
                ok = std::memcmp(buf + 1, kAck, sizeof(kAck)) == 0;
                break;
            }
            sleep_ms(2);
        }
    }
    i2c_close(fd);
    return ok;
}

// ---- SPI: PN532 is LSB-first; bit-reverse every byte, hardware CS per xfer ----
uint8_t rev8(uint8_t b) {
    b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

bool spi_xfer(int fd, const uint8_t* tx, uint8_t* rx, size_t len) {
    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<uintptr_t>(tx);
    tr.rx_buf = reinterpret_cast<uintptr_t>(rx);
    tr.len = static_cast<uint32_t>(len);
    tr.speed_hz = 1000000;
    tr.bits_per_word = 8;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr) >= 0;
}

bool probe_spi_dev(const char* dev) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) return false;
    uint8_t mode = SPI_MODE_0, bits = 8;
    uint32_t speed = 1000000;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    const uint8_t kDataWrite = 0x01, kStatRead = 0x02, kDataRead = 0x03;
    bool ok = false;

    // 1) Write the GetFirmwareVersion frame (leading DATAWRITE byte).
    {
        uint8_t tx[1 + sizeof(kGfvFrame)];
        uint8_t rx[sizeof(tx)] = {0};
        tx[0] = rev8(kDataWrite);
        for (size_t i = 0; i < sizeof(kGfvFrame); ++i) tx[1 + i] = rev8(kGfvFrame[i]);
        if (spi_xfer(fd, tx, rx, sizeof(tx))) {
            // 2) Poll the status byte until the PN532 reports ready.
            bool ready = false;
            for (int attempt = 0; attempt < 20 && !ready; ++attempt) {
                sleep_ms(2);
                uint8_t stx[2] = {rev8(kStatRead), 0x00};
                uint8_t srx[2] = {0};
                if (spi_xfer(fd, stx, srx, sizeof(stx)) && (rev8(srx[1]) & 0x01)) ready = true;
            }
            if (ready) {
                // 3) Read back the frame and look for the ACK.
                uint8_t dtx[1 + sizeof(kAck)] = {rev8(kDataRead)};
                uint8_t drx[sizeof(dtx)] = {0};
                if (spi_xfer(fd, dtx, drx, sizeof(dtx))) {
                    uint8_t got[sizeof(kAck)];
                    for (size_t i = 0; i < sizeof(kAck); ++i) got[i] = rev8(drx[1 + i]);
                    ok = std::memcmp(got, kAck, sizeof(kAck)) == 0;
                }
            }
        }
    }
    close(fd);
    return ok;
}

} // namespace
#endif // __linux__

bool pn532_present(std::string* where) {
#if defined(__linux__)
    for (const auto& bus : i2c_buses()) {
        if (probe_i2c_bus(bus.c_str())) {
            if (where) *where = "I2C " + bus;
            return true;
        }
    }
    const char* spidev = std::getenv("PN532_SPIDEV");
    const std::string dev = (spidev && spidev[0]) ? spidev : "/dev/spidev0.0";
    if (access(dev.c_str(), F_OK) == 0 && probe_spi_dev(dev.c_str())) {
        if (where) *where = "SPI " + dev;
        return true;
    }
    return false;
#else
    (void)where;
    return false;
#endif
}

int pn532_detect_main(int argc, char** argv) {
#if defined(__linux__)
    (void)argc; (void)argv;
    std::string where;
    if (pn532_present(&where)) {
        std::printf("PN532 detection\nResult: PN532 detected on %s.\n", where.c_str());
        std::fflush(stdout);
        return 0;
    }
    std::printf("PN532 detection\n"
                "No PN532 found on I2C (0x24) or SPI (%s).\n"
                "Check wiring/power; for SPI set $PN532_SPIDEV if not on spidev0.0.\n",
                (std::getenv("PN532_SPIDEV") && std::getenv("PN532_SPIDEV")[0])
                    ? std::getenv("PN532_SPIDEV") : "/dev/spidev0.0");
    std::fflush(stdout);
    return 1;
#else
    (void)argc; (void)argv;
    std::puts("PN532 detection is only available on Linux builds.");
    return 1;
#endif
}

} // namespace platform
