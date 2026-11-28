/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "rfid_i2c.h"

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace platform {

#if defined(__linux__)

int i2c_open(const char* dev, uint8_t addr7) {
    int fd = ::open(dev, O_RDWR);
    if (fd < 0) {
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, addr7) < 0) {
        // Busy = a kernel driver already bound this address (e.g. the M5IOE1 at
        // 0x4F). Keep the fd open so I2C_RDWR (which carries its own addr and is
        // not blocked by I2C_SLAVE) can still try; callers that use the bound
        // helpers will just fail cleanly.
        if (errno != EBUSY) {
            ::close(fd);
            return -1;
        }
    }
    return fd;
}

void i2c_close(int fd) {
    if (fd >= 0) ::close(fd);
}

bool i2c_write(int fd, const uint8_t* buf, size_t n) {
    if (fd < 0) return false;
    const ssize_t w = ::write(fd, buf, n);
    return w == static_cast<ssize_t>(n);
}

bool i2c_read(int fd, uint8_t* buf, size_t n) {
    if (fd < 0) return false;
    const ssize_t r = ::read(fd, buf, n);
    return r == static_cast<ssize_t>(n);
}

bool i2c_write_read(int fd, uint8_t addr7, const uint8_t* w, size_t wn, uint8_t* r, size_t rn) {
    if (fd < 0) return false;
    i2c_msg msgs[2];
    int nmsgs = 0;
    if (wn > 0) {
        msgs[nmsgs].addr = addr7;
        msgs[nmsgs].flags = 0;
        msgs[nmsgs].len = static_cast<uint16_t>(wn);
        msgs[nmsgs].buf = const_cast<uint8_t*>(w);
        ++nmsgs;
    }
    if (rn > 0) {
        msgs[nmsgs].addr = addr7;
        msgs[nmsgs].flags = I2C_M_RD;
        msgs[nmsgs].len = static_cast<uint16_t>(rn);
        msgs[nmsgs].buf = r;
        ++nmsgs;
    }
    if (nmsgs == 0) return true;
    i2c_rdwr_ioctl_data xfer{};
    xfer.msgs = msgs;
    xfer.nmsgs = static_cast<uint32_t>(nmsgs);
    return ioctl(fd, I2C_RDWR, &xfer) >= 0;
}

bool i2c_present(const char* dev, uint8_t addr7) {
    int fd = ::open(dev, O_RDWR);
    if (fd < 0) return false;
    bool ok = false;
    if (ioctl(fd, I2C_SLAVE, addr7) >= 0) {
        // A 0-length write ACK-probes the address without clobbering any
        // register. SMBus quick write semantics on most controllers.
        uint8_t dummy = 0;
        ok = ::write(fd, &dummy, 0) == 0;
        if (!ok) {
            // Fall back to a 1-byte read probe (some controllers reject len 0).
            ok = ::read(fd, &dummy, 1) == 1;
        }
    }
    ::close(fd);
    return ok;
}

std::vector<std::string> i2c_buses() {
    std::vector<std::string> out;
    // /dev/i2c-1 is the HAT/Grove bus on the CM0; prefer it, then scan 0..9.
    const char* preferred[] = {"/dev/i2c-1", "/dev/i2c-0"};
    for (const char* p : preferred) {
        if (::access(p, F_OK) == 0) out.emplace_back(p);
    }
    for (int i = 0; i <= 9; ++i) {
        std::string p = "/dev/i2c-" + std::to_string(i);
        if (::access(p.c_str(), F_OK) != 0) continue;
        bool dup = false;
        for (const auto& e : out)
            if (e == p) dup = true;
        if (!dup) out.push_back(p);
    }
    return out;
}

#else // !__linux__

int i2c_open(const char*, uint8_t) { return -1; }
void i2c_close(int) {}
bool i2c_write(int, const uint8_t*, size_t) { return false; }
bool i2c_read(int, uint8_t*, size_t) { return false; }
bool i2c_write_read(int, uint8_t, const uint8_t*, size_t, uint8_t*, size_t) { return false; }
bool i2c_present(const char*, uint8_t) { return false; }
std::vector<std::string> i2c_buses() { return {}; }

#endif

} // namespace platform
