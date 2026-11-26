/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "gps_session.h"

#include "gps_port.h"
#include "uart_speed.h"

#include <cerrno>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace platform::gps {
namespace {

std::atomic<bool> g_connected{false};

// Coalesce bursts: after a wake-up, let ~50 ms of NMEA accumulate before the
// next read so the thread wakes ~20x/s at most instead of once per UART FIFO
// chunk (the Cap streams ~1.3 KB/s at 115200). One bounded read per poll()
// round; a full buffer skips the pause so a backlog drains at once.
constexpr int kCoalesceMs = 50;
constexpr size_t kReadChunk = 4096;

} // namespace

bool connected() {
    return g_connected.load();
}

void set_connected(bool value) {
    g_connected.store(value);
}

Session::~Session() {
    close();
}

#if defined(__linux__)

bool Session::open(const std::string& port, int baud, OpenResult& result) {
    close();
    result = OpenResult{};
    hung_up_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        parser_ = Parser{};
        available_ = 0;
    }

    const int fd = ::open(port.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        switch (err) {
            case ENOENT:
            case ENODEV:
            case ENXIO:
                result.error = OpenError::NotFound;
                result.message = "GPS not Found!";
                break;
            case EACCES:
            case EPERM:
                result.error = OpenError::Permission;
                result.message = permission_message(port);
                break;
            case EBUSY:
                result.error = OpenError::Busy;
                result.message = "GPS port " + port + " is busy (gpsd or another GPS app?)";
                break;
            default:
                result.error = OpenError::Other;
                result.message = "Cannot open " + port + ": " + std::strerror(err);
                break;
        }
        return false;
    }

    // Exclusive access for the whole session: flock() for any node (also
    // stops a root opener that ignores TIOCEXCL), TIOCEXCL on a tty so other
    // readers (gpsd, cat, a second tracker) get EBUSY.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        ::close(fd);
        result.error = OpenError::Busy;
        result.message = err == EWOULDBLOCK ? "GPS port " + port + " is busy (gpsd or another GPS app?)"
                                            : "Cannot lock " + port + ": " + std::strerror(err);
        return false;
    }
    const bool is_tty = ::isatty(fd) != 0;
    if (is_tty && ::ioctl(fd, TIOCEXCL) != 0) {
        const int err = errno;
        ::close(fd);
        result.error = OpenError::Other;
        result.message = "Cannot take " + port + " exclusively: " + std::strerror(err);
        return false;
    }

    if (is_tty) {
        std::string err;
        if (!uart_set_raw(fd, baud, /*vtime_ds=*/0, &err)) {
            ::ioctl(fd, TIOCNXCL);
            ::close(fd);
            result.error = OpenError::Setup;
            result.message = "Cannot set " + port + " to " + std::to_string(baud) + " bps: " + err;
            return false;
        }
    }

    if (::pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) {
        const int err = errno;
        if (is_tty) {
            ::ioctl(fd, TIOCNXCL);
        }
        ::close(fd);
        result.error = OpenError::Other;
        result.message = std::string("Cannot start GPS reader: ") + std::strerror(err);
        return false;
    }

    fd_ = fd;
    is_tty_ = is_tty;
    reader_ = std::thread(&Session::reader_main, this);
    return true;
}

void Session::close() {
    if (reader_.joinable()) {
        const char c = 'x';
        (void)!::write(wake_[1], &c, 1);
        reader_.join();
    }
    if (fd_ >= 0) {
        if (is_tty_) {
            ::ioctl(fd_, TIOCNXCL); // the flock goes with close()
        }
        ::close(fd_);
        fd_ = -1;
    }
    is_tty_ = false;
    for (int& w : wake_) {
        if (w >= 0) {
            ::close(w);
            w = -1;
        }
    }
}

void Session::reader_main() {
    char buf[kReadChunk];
    bool drain = false; // last read filled the buffer: read again without pausing
    for (;;) {
        pollfd fds[2] = {{fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        const int r = ::poll(fds, 2, -1); // block: no CPU while the GPS is silent
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            mark_hung_up();
            return;
        }
        if (fds[1].revents != 0) {
            return; // close() asked us to stop
        }
        const short ev = fds[0].revents;
        if (ev & POLLIN) {
            if (!drain) {
                // Let a burst accumulate, but stay responsive to close().
                pollfd wake{wake_[0], POLLIN, 0};
                if (::poll(&wake, 1, kCoalesceMs) > 0) {
                    return;
                }
            }
            // Exactly one bounded read per round.
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                parser_.encode(buf, static_cast<size_t>(n));
                available_ += static_cast<size_t>(n);
                drain = static_cast<size_t>(n) == sizeof(buf);
                continue;
            }
            drain = false;
            if (n == 0) {
                // A raw tty (VMIN=0) returns 0 when its queue is empty - that is
                // not a hang-up. Only POLLHUP, or EOF on a non-tty (FIFO writer
                // gone, end of a file), ends the stream.
                if ((ev & POLLHUP) || !is_tty_) {
                    mark_hung_up();
                    return;
                }
                continue;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            mark_hung_up(); // EIO: USB GPS unplugged / pty master gone
            return;
        }
        if (ev & (POLLHUP | POLLERR | POLLNVAL)) {
            mark_hung_up();
            return; // a hung-up tty would make poll() spin; stop reading
        }
    }
}

#else // !__linux__

bool Session::open(const std::string& port, int, OpenResult& result) {
    result.error = OpenError::Other;
    result.message = "Cannot open " + port + ": serial GPS is only available on Linux";
    return false;
}

void Session::close() {}

void Session::reader_main() {}

#endif

void Session::mark_hung_up() {
    std::lock_guard<std::mutex> lock(mutex_);
    hung_up_.store(true);
}

size_t Session::take_available() {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t n = available_;
    available_ = 0;
    return n;
}

} // namespace platform::gps
