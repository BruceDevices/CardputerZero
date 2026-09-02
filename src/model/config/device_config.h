/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace model {
namespace config {

// Linux-native analogue of Bruce's BruceConfigPins. On the CM0 a "pin" is not an
// ESP32 gpio_num: buses are kernel devices (spidev / i2c-dev / ttyS) and control
// pins are libgpiod lines (gpiochip + offset). The per-module bus shape mirrors
// BruceConfigPins so the modules Bruce already supports slot in as they are
// ported: LoRa, nRF24, IR, UART (GPS), PN532 and ST25R are declared here now and
// wired into the UI as each lands. Only CC1101 is editable in the UI today.
//
// Persistence is a flat `key=value` text file (no JSON dependency) at
// $BRUCE_CONFIG_DIR/pins.conf, else ~/.bruce/pins.conf. All fields are plain
// data; the serialization below is pure so it unit-tests without a filesystem.

// A single libgpiod line: chip name (e.g. "gpiochip0") + line offset. offset < 0
// means "not connected / unset". The silkscreen G# is NOT the kernel offset -
// resolve it with `gpioinfo` on the device.
struct GpioLine {
    std::string chip = "gpiochip0";
    int offset = -1;

    bool is_set() const { return offset >= 0; }
};

// An SPI peripheral: the spidev node (kernel drives SCK/MISO/MOSI/CS) plus up to
// three module-specific control lines (their meaning is documented per module,
// e.g. CC1101 aux0=GDO0, aux1=RF switch; LoRa aux0=DIO1, aux1=RESET, aux2=BUSY).
struct SpiModule {
    std::string spidev = "/dev/spidev0.0";
    GpioLine aux0; // primary data/IRQ line (CC1101 GDO0, LoRa DIO1/IRQ, nRF24 IRQ...)
    GpioLine aux1; // secondary line (CC1101 RF switch, LoRa RESET, nRF24 CE...)
    GpioLine aux2; // tertiary line (CC1101 GDO2/opt, LoRa BUSY, PN532 RESET...)
};

// A UART peripheral (GPS today): tty device + baud rate.
struct UartModule {
    std::string device = "/dev/ttyS0";
    int baud = 9600;
};

// An I2C peripheral: i2c-dev bus + 7-bit address.
struct I2cModule {
    std::string device = "/dev/i2c-1";
    int address = 0x24;
};

// Two IR gpio lines (transmit LED + receive sensor).
struct IrModule {
    GpioLine tx;
    GpioLine rx;
};

class DeviceConfig {
public:
    // --- CC1101 (Sub-GHz) - the only module wired into the UI today ---------
    // aux0 = GDO0 (async serial data), aux1 = RF_SW0 (antenna TX/RX switch),
    // aux2 = GDO2 (not broken out on the M5 CAP -> NC). Offsets preload the M5
    // CAP CC1101 reference wiring (docs/cardputerzero-gpio.md); the gpiochip
    // offset must be confirmed with `gpioinfo` on the CM0 (the M5 tables are
    // ESP32/Cardputer-Adv numbers, not CM0 kernel offsets).
    SpiModule cc1101{
        "/dev/spidev0.0",
        {"gpiochip0", 15}, // GDO0  (M5 "G0")
        {"gpiochip0", 13}, // RF_SW0 (antenna switch)
        {"gpiochip0", -1}, // GDO2  (not connected on this CAP)
    };
    double cc1101_freq_mhz = 433.92; // default carrier
    int cc1101_scan_range = 3;       // index into model::rf::kRangeLimits

    // --- Declared for the future ports (not yet UI-editable) ----------------
    // Defaults preload the M5 CAP LoRa-1262 reference wiring; confirm on device.
    SpiModule lora{
        "/dev/spidev0.0",
        {"gpiochip0", 4}, // DIO1/IRQ
        {"gpiochip0", 3}, // RESET
        {"gpiochip0", 6}, // BUSY
    };
    SpiModule nrf24;      // aux0=IRQ, aux1=CE
    SpiModule pn532_spi;  // aux0=IRQ, aux1=RESET
    SpiModule st25r;      // aux0=IRQ
    I2cModule pn532_i2c;
    UartModule gps{"/dev/ttyS0", 9600}; // LoRa CAP GPS UART (M5 GPS-RX/TX)
    IrModule ir;

    // --- Persistence --------------------------------------------------------
    // Absolute path of the config file ($BRUCE_CONFIG_DIR/pins.conf or
    // ~/.bruce/pins.conf).
    static std::string file_path();

    // Load from disk (missing file -> defaults kept, returns false). Saves the
    // parsed values into *this.
    bool load();
    // Serialize *this to the config file, creating the directory if needed.
    bool save() const;
    // Reset every field to its compiled-in default.
    void reset_defaults();

    // --- Pure (de)serialization (no filesystem, unit-testable) --------------
    std::string serialize() const;
    // Parse `key=value` text into *this. Unknown keys are ignored; missing keys
    // keep their current value.
    void parse(const std::string& text);
};

// Process-wide config, lazily loaded from disk on first access.
DeviceConfig& device_config();

} // namespace config
} // namespace model
