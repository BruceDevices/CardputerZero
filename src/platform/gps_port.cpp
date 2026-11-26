/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "gps_port.h"

#include "gps_parser.h"
#include "uart_speed.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace platform::gps {
namespace {

#if defined(__linux__)

bool exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

std::string real_path(const std::string& path) {
    char buf[4096];
    if (::realpath(path.c_str(), buf)) {
        return buf;
    }
    return path;
}

// "ttyUSB10" after "ttyUSB2": compare the trailing number numerically.
bool natural_less(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s, std::string& stem, long& num) {
        size_t i = s.size();
        while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') {
            --i;
        }
        stem = s.substr(0, i);
        num = i < s.size() ? std::strtol(s.c_str() + i, nullptr, 10) : -1;
    };
    std::string sa, sb;
    long na = 0, nb = 0;
    split(a, sa, na);
    split(b, sb, nb);
    if (sa != sb) {
        return sa < sb;
    }
    return na < nb;
}

std::vector<std::string> list_dir(const std::string& dir, const std::string& prefix) {
    std::vector<std::string> names;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            if (!prefix.empty() && name.rfind(prefix, 0) != 0) {
                continue;
            }
            names.push_back(name);
        }
        ::closedir(d);
    }
    std::sort(names.begin(), names.end(), natural_less);
    return names;
}

uint64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

#endif

bool cancelled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load();
}

} // namespace

#if defined(__linux__)

std::vector<std::string> port_candidates_in(const std::string& dev_root) {
    std::vector<std::string> out;
    std::vector<std::string> seen_real;
    auto add = [&](const std::string& path) {
        if (!exists(path)) {
            return;
        }
        const std::string rp = real_path(path);
        if (std::find(seen_real.begin(), seen_real.end(), rp) != seen_real.end()) {
            return;
        }
        seen_real.push_back(rp);
        out.push_back(path);
    };
    add(dev_root + "/serial0");
    for (const auto& n : list_dir(dev_root + "/serial/by-id", "")) {
        add(dev_root + "/serial/by-id/" + n);
    }
    for (const auto& n : list_dir(dev_root, "ttyACM")) {
        add(dev_root + "/" + n);
    }
    for (const auto& n : list_dir(dev_root, "ttyUSB")) {
        add(dev_root + "/" + n);
    }
    return out;
}

std::vector<std::string> port_candidates() {
    if (const char* env = std::getenv("BRUCE_GPS_PORT_CANDIDATES"); env && env[0] != '\0') {
        std::vector<std::string> out;
        std::string list = env;
        size_t start = 0;
        while (start <= list.size()) {
            const size_t colon = list.find(':', start);
            const std::string item = list.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
            if (!item.empty() && exists(item) && std::find(out.begin(), out.end(), item) == out.end()) {
                out.push_back(item);
            }
            if (colon == std::string::npos) {
                break;
            }
            start = colon + 1;
        }
        return out;
    }
    return port_candidates_in("/dev");
}

int probe_port(const std::string& port, int baud, int ms, const std::atomic<bool>* cancel) {
    const int fd = ::open(port.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        return err == EPERM ? EACCES : (err == ENODEV || err == ENXIO ? ENOENT : err);
    }
    // Same exclusivity rule as Session: a port a GPS feature (or another
    // probe) holds is busy, never shared.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return EBUSY;
    }
    const bool tty = ::isatty(fd) != 0;
    if (tty) {
        std::string err;
        if (!uart_set_raw(fd, baud, /*vtime_ds=*/0, &err)) {
            ::close(fd);
            return EIO;
        }
    }
    Parser parser;
    char buf[512];
    const uint64_t deadline = now_ms() + static_cast<uint64_t>(ms > 0 ? ms : 0);
    int rc = ETIMEDOUT;
    for (;;) {
        if (cancelled(cancel)) {
            rc = ECANCELED;
            break;
        }
        const uint64_t now = now_ms();
        if (now >= deadline) {
            break;
        }
        const int wait = static_cast<int>(std::min<uint64_t>(100, deadline - now));
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, wait);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = EIO;
            break;
        }
        if (r == 0) {
            continue;
        }
        const bool hangup = (p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
        if (p.revents & POLLIN) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                if (parser.encode(buf, static_cast<size_t>(n)) > 0 || parser.passedChecksum() > 0) {
                    rc = 0;
                    break;
                }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR) && !hangup) {
                continue;
            }
            if (n < 0 && !hangup) {
                rc = EIO;
                break;
            }
            if (n == 0 && !tty) {
                break; // end of file / FIFO writer gone: no (more) NMEA here
            }
            if (!hangup) {
                // A tty read of 0 without a hang-up flag (VMIN=0): nothing yet.
                continue;
            }
            // Zero read / error with POLLHUP|POLLERR: fall through. poll()
            // would report the hang-up again at once, so leaving here is what
            // keeps this loop from spinning until the deadline.
        }
        if (hangup) {
            if (!tty) {
                break;
            }
            rc = EIO; // pty master gone / USB unplugged
            break;
        }
    }
    ::close(fd);
    return rc;
}

#else

std::vector<std::string> port_candidates_in(const std::string&) { return {}; }
std::vector<std::string> port_candidates() { return {}; }
int probe_port(const std::string&, int, int, const std::atomic<bool>*) { return ENOSYS; }

#endif

std::vector<std::string> auto_order(const std::string& preferred, const std::vector<std::string>& candidates) {
    std::vector<std::string> out;
    for (const auto& c : candidates) {
        if (c == preferred) {
            out.push_back(c);
        }
    }
    for (const auto& c : candidates) {
        if (c != preferred) {
            out.push_back(c);
        }
    }
    return out;
}

ProbeResult auto_detect(const std::vector<std::string>& ports, int preferred_baud, const std::vector<int>& bauds,
                        int ms_per_try, const std::atomic<bool>* cancel) {
    ProbeResult res;
    res.tried = ports;
    if (ports.empty()) {
        res.kind = ProbeResult::Kind::NoCandidates;
        res.message = "GPS not Found! (Auto: no serial port to try)";
        return res;
    }
    std::vector<int> order;
    order.push_back(preferred_baud);
    for (const int b : bauds) {
        if (b != preferred_baud) {
            order.push_back(b);
        }
    }

    std::string denied;
    std::string busy;
    std::vector<std::string> skip; // missing / unusable nodes are not retried per baud
    for (const int baud : order) {
        for (const auto& port : ports) {
            if (std::find(skip.begin(), skip.end(), port) != skip.end()) {
                continue;
            }
            if (cancelled(cancel)) {
                res.kind = ProbeResult::Kind::Cancelled;
                res.message = "Cancelled";
                return res;
            }
            const int rc = probe_port(port, baud, ms_per_try, cancel);
            if (rc == 0) {
                res.kind = ProbeResult::Kind::Found;
                res.port = port;
                res.baud = baud;
                res.message = "GPS on " + port + " @ " + std::to_string(baud) + " bps";
                return res;
            }
            if (rc == ECANCELED) {
                res.kind = ProbeResult::Kind::Cancelled;
                res.message = "Cancelled";
                return res;
            }
            if (rc == EACCES) {
                if (denied.empty()) {
                    denied = port;
                }
                skip.push_back(port);
            } else if (rc == EBUSY) {
                if (busy.empty()) {
                    busy = port;
                }
                skip.push_back(port);
            } else if (rc != ETIMEDOUT) {
                skip.push_back(port); // ENOENT, EIO, ...: the node is gone or broken
            }
        }
    }
    if (!denied.empty()) {
        res.kind = ProbeResult::Kind::Permission;
        res.message = permission_message(denied);
    } else if (!busy.empty()) {
        res.kind = ProbeResult::Kind::Busy;
        res.message = "GPS port " + busy + " is busy (gpsd or another GPS app?)";
    } else {
        res.kind = ProbeResult::Kind::NoNmea;
        res.message = "GPS not Found! (Auto: no NMEA on " + std::to_string(ports.size()) + " port" +
                      (ports.size() == 1 ? "" : "s") + ")";
    }
    return res;
}

} // namespace platform::gps
