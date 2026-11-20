/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "cap_bringup.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

#if defined(__linux__)

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && v[0] != '\0') ? v : fallback;
}

// The shipped CardputerZero image exposes the HAT-port 5V load switch as a
// gpio-led named "ext_5v_out" - that LED class node, not the raw I2C expander
// pin, is the mechanism the working CC1101 stack (RaspyJack) drives. Write it
// first; if the node is absent (custom overlay missing) the open fails and we
// fall back to the I2C PYG13 path below. Returns true when the write landed.
bool enable_ext_5v_led() {
    const std::string path =
        env_or("CAP_5V_LED", "/sys/class/leds/ext_5v_out/brightness");
    const int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const bool ok = write(fd, "1", 1) == 1;
    close(fd);
    return ok;
}

// --- M5IOE1 IO expander (register map: m5stack/M5IOE1) ----------------------

constexpr uint8_t IOE_REG_UID_L     = 0x00; // R  [7:0] UID low
constexpr uint8_t IOE_REG_MODE_L    = 0x03; // R/W mode P8-P1 (bit0 = P1)
constexpr uint8_t IOE_REG_MODE_H    = 0x04; // R/W mode P14-P9 (bit4 = P13)
constexpr uint8_t IOE_REG_OUT_L     = 0x05; // R/W out P8-P1
constexpr uint8_t IOE_REG_OUT_H     = 0x06; // R/W out P14-P9

// 16-bit little-endian register access (M5IOE1_I2C_*_REG16 format: reg, lo, hi).
bool ioe_read16(int fd, uint8_t reg, uint16_t& out) {
    if (write(fd, &reg, 1) != 1) return false;
    uint8_t buf[2] = {0, 0};
    if (read(fd, buf, 2) != 2) return false;
    out = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
    return true;
}

bool ioe_write16(int fd, uint8_t reg, uint16_t value) {
    uint8_t buf[3] = {reg, static_cast<uint8_t>(value & 0xFF),
                      static_cast<uint8_t>(value >> 8)};
    return write(fd, buf, 3) == 3;
}

// The expander sleeps and wakes on an I2C START+STOP; SMBus Quick sends exactly
// that (M5IOE1_I2C_*_SEND_WAKE equivalent).
void ioe_wake(int fd) {
    i2c_smbus_ioctl_data args{};
    args.read_write = I2C_SMBUS_WRITE;
    args.size = I2C_SMBUS_QUICK;
    ioctl(fd, I2C_SMBUS, &args);
}

// Set one expander pin to output with the given level. Returns false on any
// I2C failure; mode/out registers are read-modify-written. P1-P8 live in the
// L registers, P9-P14 in the H registers.
bool ioe_pin_set(int fd, uint8_t pin, bool high) {
    if (pin < 1 || pin > 14) return false;
    const uint8_t bit = cap_ioe_bit(pin);
    const uint8_t mode_reg = cap_ioe_high_bank(pin) ? IOE_REG_MODE_H : IOE_REG_MODE_L;
    const uint8_t out_reg = cap_ioe_high_bank(pin) ? IOE_REG_OUT_H : IOE_REG_OUT_L;
    uint16_t mode = 0;
    uint16_t out = 0;
    if (!ioe_read16(fd, mode_reg, mode) || !ioe_read16(fd, out_reg, out)) {
        return false;
    }
    mode |= bit;
    if (high) {
        out |= bit;
    } else {
        out &= static_cast<uint16_t>(~bit);
    }
    return ioe_write16(fd, mode_reg, mode) && ioe_write16(fd, out_reg, out);
}

// Claim a GPIO output line (kept open for the process lifetime).
bool claim_output_impl(const char* chip_path, unsigned line, bool initial_high,
                       int* out_handle) {
    const int fd = open(chip_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    gpiohandle_request req{};
    req.lineoffsets[0] = line;
    req.lines = 1;
    req.flags = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = initial_high ? 1 : 0;
    std::strncpy(req.consumer_label, "bruce-cap", sizeof(req.consumer_label) - 1);
    if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        close(fd);
        return false;
    }
    close(fd); // chip fd no longer needed once the line handle exists
    if (out_handle != nullptr) {
        *out_handle = req.fd;
        return true;
    }
    // Closing the request fd would release the line and let it float, so keep
    // it (intentional leak - the tools exit right after).
    static int keep_open[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (int& slot : keep_open) {
        if (slot < 0) {
            slot = req.fd;
            return true;
        }
    }
    close(req.fd);
    return false;
}

bool set_line_impl(int handle_fd, bool high) {
    if (handle_fd < 0) return false;
    gpiohandle_data data{};
    data.values[0] = high ? 1 : 0;
    return ioctl(handle_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) >= 0;
}

bool read_line_impl(const char* chip_path, unsigned line, bool& value) {
    const int fd = open(chip_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    gpiohandle_request req{};
    req.lineoffsets[0] = line;
    req.lines = 1;
    req.flags = GPIOHANDLE_REQUEST_INPUT;
    std::strncpy(req.consumer_label, "bruce-cap", sizeof(req.consumer_label) - 1);
    if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        close(fd);
        return false;
    }
    close(fd);
    gpiohandle_data data{};
    const bool ok = ioctl(req.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) >= 0;
    close(req.fd); // release the input line
    if (ok) {
        value = data.values[0] != 0;
    }
    return ok;
}

bool ioe_present(int fd, uint8_t addr, uint16_t& uid) {
    if (ioctl(fd, I2C_SLAVE, addr) < 0) return false;
    ioe_wake(fd);
    uid = 0;
    return ioe_read16(fd, IOE_REG_UID_L, uid);
}

#endif // __linux__

} // namespace

CapBringupReport cap_bringup() {
    CapBringupReport r;

#if defined(__linux__)
    // No root check here: on a properly provisioned CardputerZero the user is in
    // the gpio/spi/i2c groups and the ext_5v_out LED is group-writable, so the
    // CAP tools run fine unprivileged. A real permission problem surfaces as an
    // EACCES hint at the specific step that failed (below).

    // Primary 5V rail enable (device-tree gpio-led). Done before the I2C path
    // so the rail is already coming up while we poke the expander.
    r.rail_led = enable_ext_5v_led();
    r.text += r.rail_led
                  ? "cap power: ext_5v_out LED on (HAT 5V rail)\n"
                  : "cap power: no ext_5v_out LED node - using I2C expander path\n";

    // Secondary/legacy path: the I2C expander also gates the rail on some units.
    // Best-effort - a failure here no longer aborts bring-up now that ext_5v_out
    // is the primary enable and GPIO26 (below) must run regardless.
    const std::string i2c_dev = env_or("M5IOE1_I2C", "/dev/i2c-1");
    const int fd = open(i2c_dev.c_str(), O_RDWR);
    if (fd < 0) {
        r.text += std::string("cap power: ") + i2c_dev + " not available (" +
                  std::strerror(errno) + ")\n";
        if (errno == ENOENT) {
            r.text += "  hint: enable I2C: sudo raspi-config nonint do_i2c 0 (reboot)\n";
        } else if (errno == EACCES || errno == EPERM) {
            r.text += "  hint: permission denied - restart the app with sudo.\n";
        }
    } else {
        // CardputerZero wires the expander at its secondary address; the library
        // default (0x6F) is tried for other products.
        uint16_t uid = 0;
        uint8_t used_addr = 0;
        for (uint8_t addr : {0x4F, 0x6F}) {
            if (ioe_present(fd, addr, uid)) {
                used_addr = addr;
                break;
            }
        }
        if (used_addr == 0) {
            // On the shipped image the m5ioe1 kernel driver owns 0x4F, so the raw
            // I2C_SLAVE claim returns EBUSY - not an error: the rail is already
            // enabled via the ext_5v_out gpio-led above (and PYG13 by that driver).
            r.text += "cap power: M5IOE1 at 0x4F is kernel-driver-bound (expected) "
                      "- rail handled by the ext_5v_out LED path\n";
        } else {
            // PYG13 high = HAT-port 5V load switch; PYG1 low = HAT_P0/P1 to GPIO.
            r.hat_5v = ioe_pin_set(fd, 13, true) && ioe_pin_set(fd, 1, false);
            r.text += r.hat_5v
                          ? "cap power: HAT rail on (M5IOE1 0x" +
                                std::to_string(used_addr) + " PYG13 high, P1=GPIO mode)\n"
                          : "cap power: M5IOE1 present but PYG13/P1 writes failed\n";
        }
        close(fd);
    }

    // NOTE: G26/GPIO26 is NOT a system power-enable. Per the C154 schematic the
    // HAT 5V rail is gated solely by M5IOE1 PYG13_HAT_EN (above); G26 is a plain
    // GPIO routed to the HAT that the known CAPs use as a per-module RST line.
    // Reset is therefore handled per module in the probes (each probe releases
    // its own `rst` GPIO), not as a blanket drive here. See bruce_module.h.
    r.cap_en = false;

    // Let the DC-DC and the chip crystals come up before anyone reads SPI.
    // ponytail: fixed 250ms settle; make it env-tunable only if a cap needs longer.
    usleep(250 * 1000);
#else
    r.text = "cap power: only available on Linux builds\n";
#endif
    return r;
}

bool gpio_line_write(const char* chip_path, unsigned line, bool high) {
#if defined(__linux__)
    return claim_output_impl(chip_path, line, high, nullptr);
#else
    (void)chip_path; (void)line; (void)high;
    return false;
#endif
}

bool gpio_claim_output(const char* chip_path, unsigned line, bool initial_high,
                       int* out_handle) {
#if defined(__linux__)
    return claim_output_impl(chip_path, line, initial_high, out_handle);
#else
    (void)chip_path; (void)line; (void)initial_high; (void)out_handle;
    return false;
#endif
}

bool gpio_line_set(int handle_fd, bool high) {
#if defined(__linux__)
    return set_line_impl(handle_fd, high);
#else
    (void)handle_fd; (void)high;
    return false;
#endif
}

bool gpio_read_line(const char* chip_path, unsigned line, bool& value) {
#if defined(__linux__)
    return read_line_impl(chip_path, line, value);
#else
    (void)chip_path; (void)line; (void)value;
    return false;
#endif
}

bool gpio_set_function(unsigned line, unsigned fsel) {
#if defined(__linux__)
    // BCM GPFSEL: 10 pins per 32-bit register, 3 bits per pin. /dev/gpiomem maps
    // the GPIO block at offset 0 (GPFSEL0), readable/writable by the gpio group.
    const int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (fd < 0) {
        return false;
    }
    const size_t len = 4096;
    void* map = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return false;
    }
    volatile uint32_t* gpio = static_cast<volatile uint32_t*>(map);
    const unsigned reg = line / 10;
    const unsigned shift = (line % 10) * 3;
    uint32_t v = gpio[reg];
    v = (v & ~(0x7u << shift)) | ((fsel & 0x7u) << shift);
    gpio[reg] = v;
    munmap(map, len);
    return true;
#else
    (void)line; (void)fsel;
    return false;
#endif
}

bool gpio_restore_serial_mux() {
#if defined(__linux__)
    // G14/G15 = mini-UART TXD1/RXD1 = ALT5 (FSEL 0b010) on the CardputerZero.
    constexpr unsigned kAlt5 = 0x2;
    const bool a = gpio_set_function(14, kAlt5);
    const bool b = gpio_set_function(15, kAlt5);
    return a && b;
#else
    return false;
#endif
}

uint8_t cap_ioe_bit(uint8_t pin) {
    if (pin < 1 || pin > 14) return 0;
    return static_cast<uint8_t>(pin >= 9 ? (1u << (pin - 9)) : (1u << (pin - 1)));
}

bool cap_ioe_high_bank(uint8_t pin) {
    return pin >= 9 && pin <= 14;
}

std::string spidev_pick(const std::vector<std::string>& candidates) {
#if defined(__linux__)
    for (const auto& path : candidates) {
        if (path.empty()) continue;
        struct stat st {};
        if (stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode)) {
            return path;
        }
    }
#else
    (void)candidates;
#endif
    return "";
}

} // namespace platform
