/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "cc1101_driver.h"

#include <cstdint>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#ifndef APP_CC1101_SPIDEV
#define APP_CC1101_SPIDEV "/dev/spidev0.0"
#endif

namespace platform {
namespace rf {
namespace {

constexpr uint32_t kSpeedHz = 1000000; // 1 MHz - conservative, valid for reads
constexpr uint8_t kWriteBurst = 0x40;
constexpr uint8_t kReadSingle = 0x80;
constexpr uint8_t kReadBurst = 0xC0;

// Baseline OOK async-serial register block. Values are the widely-used CC1101
// ASK/OOK receive/transmit configuration (SmartRF-derived, as ELECHOUSE/rc-
// switch-on-CC1101 ships): GDO0 async serial, infinite-length async packet
// mode, ASK/OOK modulation, no sync word, manual calibration. Frequency, RX
// bandwidth and PA table are layered on top by the driver. Final AGC/PA tuning
// is a hardware bring-up step (see the Phase 2 notes).
struct RegVal {
    uint8_t addr;
    uint8_t val;
};
const RegVal kOokAsyncConfig[] = {
    {CC1101_IOCFG2, 0x2E},                    // GDO2 tri-state (unused)
    {CC1101_IOCFG0, CC1101_GDO0_ASYNC_SERIAL},// GDO0 = async serial data
    {CC1101_FIFOTHR, 0x47},
    {CC1101_PKTCTRL0, 0x30},                  // async serial, infinite length, no CRC
    {CC1101_FSCTRL1, 0x06},                   // IF frequency
    {CC1101_FSCTRL0, 0x00},
    {CC1101_MDMCFG4, 0x67},                   // RX BW high nibble overwritten by preset
    {CC1101_MDMCFG3, 0x93},                   // data rate mantissa (~4.8 kBaud default)
    {CC1101_MDMCFG2, 0x30},                   // ASK/OOK, no sync
    {CC1101_MDMCFG1, 0x22},
    {CC1101_MDMCFG0, 0xF8},
    {CC1101_DEVIATN, 0x00},
    {CC1101_MCSM0, 0x18},                     // auto-calibrate from IDLE->RX/TX
    {CC1101_FOCCFG, 0x16},
    {CC1101_AGCCTRL2, 0x03},                  // OOK AGC (magnitude target)
    {CC1101_AGCCTRL1, 0x00},
    {CC1101_AGCCTRL0, 0x91},
    {CC1101_FREND1, 0x56},
    {CC1101_FREND0, 0x11},                    // PA_POWER index 1 for OOK
    {CC1101_FSCAL3, 0xE9},
    {CC1101_FSCAL2, 0x2A},
    {CC1101_FSCAL1, 0x00},
    {CC1101_FSCAL0, 0x1F},
    {CC1101_TEST2, 0x81},
    {CC1101_TEST1, 0x35},
    {CC1101_TEST0, 0x09},
};

void settle_us(unsigned us) {
#if defined(__linux__)
    usleep(us);
#else
    (void)us;
#endif
}

} // namespace

Cc1101::~Cc1101() { close(); }

bool Cc1101::xfer(const uint8_t* tx, uint8_t* rx, size_t len) {
#if defined(__linux__)
    if (fd_ < 0) return false;
    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<uintptr_t>(tx);
    tr.rx_buf = reinterpret_cast<uintptr_t>(rx);
    tr.len = static_cast<uint32_t>(len);
    tr.speed_hz = kSpeedHz;
    tr.bits_per_word = 8;
    return ioctl(fd_, SPI_IOC_MESSAGE(1), &tr) >= 1;
#else
    (void)tx;
    (void)rx;
    (void)len;
    return false;
#endif
}

bool Cc1101::open(const std::string& spidev_path) {
#if defined(__linux__)
    close();
    const std::string path = spidev_path.empty() ? std::string(APP_CC1101_SPIDEV) : spidev_path;
    fd_ = ::open(path.c_str(), O_RDWR);
    if (fd_ < 0) return false;

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = kSpeedHz;
    ioctl(fd_, SPI_IOC_WR_MODE, &mode);
    ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    return true;
#else
    (void)spidev_path;
    return false;
#endif
}

void Cc1101::close() {
#if defined(__linux__)
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

bool Cc1101::strobe(uint8_t cmd) {
    uint8_t rx = 0;
    return xfer(&cmd, &rx, 1);
}

bool Cc1101::write_reg(uint8_t addr, uint8_t value) {
    const uint8_t tx[2] = {addr, value};
    uint8_t rx[2] = {0, 0};
    return xfer(tx, rx, 2);
}

bool Cc1101::write_burst(uint8_t addr, const uint8_t* data, size_t n) {
    if (n == 0 || n > 63) return false;
    uint8_t tx[64];
    uint8_t rx[64];
    tx[0] = addr | kWriteBurst;
    for (size_t i = 0; i < n; i++) tx[i + 1] = data[i];
    return xfer(tx, rx, n + 1);
}

uint8_t Cc1101::read_reg(uint8_t addr) {
    const uint8_t tx[2] = {static_cast<uint8_t>(addr | kReadSingle), 0x00};
    uint8_t rx[2] = {0, 0};
    if (!xfer(tx, rx, 2)) return 0;
    return rx[1];
}

uint8_t Cc1101::read_status(uint8_t addr) {
    const uint8_t tx[2] = {static_cast<uint8_t>(addr | kReadBurst), 0x00};
    uint8_t rx[2] = {0, 0};
    if (!xfer(tx, rx, 2)) return 0;
    return rx[1];
}

bool Cc1101::reset() {
    if (!strobe(CC1101_SRES)) return false;
    settle_us(1000); // let the chip come out of reset before touching registers
    return true;
}

bool Cc1101::detected() {
    uint8_t version = read_status(CC1101_VERSION);
    // A floating/absent MISO reads all-zeros or all-ones; a real CC1101 reports a
    // non-trivial VERSION across silicon revs.
    return version != 0x00 && version != 0xFF;
}

int Cc1101::marcstate() {
    if (!is_open()) return -1;
    return read_status(CC1101_MARCSTATE) & 0x1F;
}

void Cc1101::set_frequency_hz(double hz) {
    FreqRegs r = cc1101_calc_freq(hz);
    write_reg(CC1101_FREQ2, r.freq2);
    write_reg(CC1101_FREQ1, r.freq1);
    write_reg(CC1101_FREQ0, r.freq0);
}

void Cc1101::configure_ook_async() {
    for (const auto& rv : kOokAsyncConfig) write_reg(rv.addr, rv.val);
    set_ook_patable();
}

void Cc1101::apply_preset(uint8_t modulation, double deviation_hz, double rxbw_hz, double datarate_hz) {
    // MDMCFG2 modulation format bits [6:4]: 0=2-FSK, 1=GFSK, 3=ASK/OOK, 7=MSK.
    uint8_t modfmt = 0x00;
    switch (modulation) {
        case 2: modfmt = 0x30; break; // ASK/OOK
        case 1: modfmt = 0x10; break; // GFSK
        case 4: modfmt = 0x70; break; // MSK
        default: modfmt = 0x00; break; // 2-FSK
    }
    // Preserve the sync-mode low bits already programmed (async OOK uses 0).
    uint8_t mdmcfg2 = (read_reg(CC1101_MDMCFG2) & 0x0F) | modfmt;
    write_reg(CC1101_MDMCFG2, mdmcfg2);

    uint8_t mdmcfg4 = read_reg(CC1101_MDMCFG4);
    if (rxbw_hz > 0) {
        mdmcfg4 = (mdmcfg4 & 0x0F) | cc1101_calc_rxbw_mdmcfg4_high(rxbw_hz);
    }
    if (datarate_hz > 0) {
        DataRateRegs dr = cc1101_calc_datarate(datarate_hz);
        mdmcfg4 = (mdmcfg4 & 0xF0) | (dr.drate_e & 0x0F);
        write_reg(CC1101_MDMCFG3, dr.drate_m);
    }
    write_reg(CC1101_MDMCFG4, mdmcfg4);

    if (deviation_hz > 0) write_reg(CC1101_DEVIATN, cc1101_calc_deviation(deviation_hz));
}

void Cc1101::set_ook_patable(uint8_t power) {
    // OOK keys the carrier by toggling between PA_TABLE[0] (off) and [1] (on).
    const uint8_t pa[2] = {0x00, power};
    write_burst(CC1101_PATABLE, pa, 2);
}

void Cc1101::idle() {
    strobe(CC1101_SIDLE);
    // Wait for MARCSTATE == IDLE (0x01), bounded so a wedged chip can't hang us.
    for (int i = 0; i < 100; i++) {
        if (marcstate() == 0x01) break;
        settle_us(100);
    }
}

void Cc1101::flush_rx() {
    idle();
    strobe(CC1101_SFRX);
}

void Cc1101::flush_tx() {
    idle();
    strobe(CC1101_SFTX);
}

void Cc1101::enter_rx_async() {
    idle();
    write_reg(CC1101_IOCFG0, CC1101_GDO0_ASYNC_SERIAL); // GDO0 outputs demod data
    strobe(CC1101_SCAL);
    settle_us(800); // calibration time
    strobe(CC1101_SRX);
}

void Cc1101::enter_tx_async() {
    idle();
    // In async TX PKTCTRL0=0x30 makes GDO0 the modulator input; the pin is driven
    // by rf_transmit (libgpiod), so here we only calibrate and start TX.
    strobe(CC1101_SCAL);
    settle_us(800);
    strobe(CC1101_STX);
}

} // namespace rf
} // namespace platform
