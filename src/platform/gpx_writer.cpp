/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "gpx_writer.h"

#include "gps_tracker.h" // gpx_header / gpx_point / gpx_footer

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace platform::gps {
namespace {

struct Job {
    enum class Kind { Point, Finalize };
    Kind kind = Kind::Point;
    std::string name;
    GpxPoint point;
};

#if defined(__linux__)
bool write_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false; // EFBIG (RLIMIT_FSIZE), ENOSPC, EIO, ...
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool is_dir(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string parent_dir(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return slash == 0 ? "/" : path.substr(0, slash);
}

// "<base>_gps_tracker.gpx" -> "<base>_gps_tracker-<n>.gpx"
std::string with_suffix(const std::string& name, int n) {
    if (n == 0) {
        return name;
    }
    const size_t dot = name.rfind(".gpx");
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    return stem + "-" + std::to_string(n) + ".gpx";
}
#endif

} // namespace

struct GpxWriter::Shared {
    std::string dir;
    int delay_ms = 0;

    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::deque<Job> jobs;
    std::deque<Result> results;
    bool busy = false;
    bool closing = false;         // owner gone: drain the queue, then exit
    bool finalize_queued = false; // a Finalize job was ever queued
    bool done = false;            // worker loop has returned
    bool reported = false;        // logged as unfinished at shutdown

    // Worker-only state.
    int dir_fd = -1;
    int file_fd = -1;
    std::string filename;

    void run();
    Result do_point(const Job& job);
    Result do_finalize();
    void delay() const {
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
};

void GpxWriter::Shared::run() {
#if defined(__linux__)
    // A write past RLIMIT_FSIZE raises SIGXFSZ on the writing thread; blocked
    // here, the write fails with EFBIG instead of the default core-dump kill.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGXFSZ);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
#endif
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return !jobs.empty() || closing; });
            if (jobs.empty()) {
                break; // closing and drained
            }
            job = jobs.front();
            jobs.pop_front();
            busy = true;
        }
        Result r = job.kind == Job::Kind::Point ? do_point(job) : do_finalize();
        {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(std::move(r));
            busy = false;
        }
        cv.notify_all();
    }
#if defined(__linux__)
    if (file_fd >= 0) {
        ::close(file_fd); // owner vanished without finalize(): still release
    }
    if (dir_fd >= 0) {
        ::close(dir_fd);
    }
#endif
    {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
    }
    cv.notify_all();
}

GpxWriter::Result GpxWriter::Shared::do_point(const Job& job) {
    Result r;
    r.kind = Result::Kind::Point;
    r.point = job.point;
#if defined(__linux__)
    delay();
    if (file_fd < 0) {
        // getFsStorage(): the storage root is the user's home (Bruce: SD card).
        if (dir.empty() || !is_dir(parent_dir(dir))) {
            r.storage_error = true;
            r.error = "Storage setup error";
            return r;
        }
        if (dir_fd < 0) {
            if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
                r.storage_error = true;
                r.error = "Storage setup error";
                return r;
            }
            dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (dir_fd < 0) {
                r.storage_error = true; // missing, not a directory, or a symlink
                r.error = "Storage setup error";
                return r;
            }
        }
        for (int n = 0; n < 100 && file_fd < 0; ++n) {
            const std::string name = with_suffix(job.name, n);
            const int fd = ::openat(dir_fd, name.c_str(),
                                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
            if (fd < 0) {
                if (errno == EEXIST) {
                    continue;
                }
                r.error = "Failed to open file for writing";
                return r;
            }
            {
                // Named before the header write so a shutdown report can say
                // which file a stuck write leaves incomplete.
                std::lock_guard<std::mutex> lock(mutex);
                filename = name;
            }
            if (!write_all(fd, gpx_header())) {
                ::close(fd);
                ::unlinkat(dir_fd, name.c_str(), 0); // never leave a header-only stub
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    filename.clear();
                }
                r.error = "Failed to write GPS file";
                return r;
            }
            file_fd = fd;
        }
        if (file_fd < 0) {
            r.error = "Failed to open file for writing";
            return r;
        }
    }
    r.filename = filename;
    if (!write_all(file_fd, gpx_point(job.point.lat, job.point.lng, job.point.ele, job.point.hdop,
                                      job.point.sats))) {
        r.error = "Failed to write GPS file";
        return r;
    }
    r.ok = true;
#else
    r.storage_error = true;
    r.error = "Storage setup error";
#endif
    return r;
}

GpxWriter::Result GpxWriter::Shared::do_finalize() {
    Result r;
    r.kind = Result::Kind::Finalized;
    r.ok = true;
#if defined(__linux__)
    if (file_fd < 0) {
        return r; // nothing created by this session
    }
    delay();
    r.filename = filename;
    const int fd = file_fd;
    file_fd = -1;
    const bool wrote = write_all(fd, gpx_footer());
    const bool closed = ::close(fd) == 0;
    if (!wrote || !closed) {
        r.ok = false;
        r.error = "Failed to finalize " + filename;
    }
#endif
    return r;
}

// --- process-wide owner of the worker threads --------------------------------------
//
// ~GpxWriter() must not block (Back on slow storage), so a writer's thread
// outlives its GpxWriter. The registry owns every worker thread instead of
// detaching it: finished ones are reaped as new writers start, and
// drain_writers() - run on orderly shutdown and from an atexit() hook (LVGL's
// SDL window close calls exit() directly) - closes the tracks and joins them.
// The registry is never destroyed, so no std::thread is ever destructed while
// joinable, whatever the exit path.
namespace {

struct Registry {
    std::mutex mutex;
    std::vector<std::pair<std::shared_ptr<GpxWriter::Shared>, std::thread>> workers;
};

Registry& registry() {
    static Registry* r = [] {
        auto* reg = new Registry();
        std::atexit([] {
            // Same deadline as main()'s drain: never a second full wait.
            if (drain_writers() > 0) {
                std::fflush(nullptr);
                std::_Exit(1); // unfinished GPX writes: do not report success
            }
        });
        return reg;
    }();
    return *r;
}

} // namespace

void GpxWriter::request_close(Shared& s, bool finalize) {
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (finalize && !s.finalize_queued) {
            s.finalize_queued = true;
            Job job;
            job.kind = Job::Kind::Finalize;
            s.jobs.push_back(std::move(job));
        }
        s.closing = true;
    }
    s.cv.notify_all();
}

namespace {
std::mutex g_deadline_mutex;
bool g_deadline_set = false;
std::chrono::steady_clock::time_point g_deadline;
int g_shutdown_timeout_ms = kShutdownTimeoutMs;
} // namespace

std::chrono::steady_clock::time_point shutdown_deadline() {
    std::lock_guard<std::mutex> lock(g_deadline_mutex);
    if (!g_deadline_set) {
        g_deadline_set = true;
        g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(g_shutdown_timeout_ms);
    }
    return g_deadline;
}

void set_shutdown_timeout_ms(int ms) {
    std::lock_guard<std::mutex> lock(g_deadline_mutex);
    g_shutdown_timeout_ms = ms;
}

void reset_shutdown_for_tests() {
    std::lock_guard<std::mutex> lock(g_deadline_mutex);
    g_deadline_set = false;
    g_shutdown_timeout_ms = kShutdownTimeoutMs;
}

namespace {
// Failures of orphaned writers (owner gone, result never applied) that were
// logged; any of them makes the shutdown report failure.
std::atomic<size_t> g_orphan_failures{0};

// Log, once, every failed result nobody collected (e.g. the footer write
// failing with ENOSPC after the screen went away). Called with s.mutex held.
size_t report_orphan_failures_locked(GpxWriter::Shared& s) {
    size_t n = 0;
    for (auto& r : s.results) {
        if (!r.ok) {
            const std::string file = s.dir + "/" + (r.filename.empty() ? s.filename : r.filename);
            std::fprintf(stderr, "gps: GPX write failed (%s): %s - the track is incomplete\n",
                         r.error.c_str(), file.c_str());
            ++n;
        }
    }
    s.results.clear(); // consumed: never logged twice
    g_orphan_failures += n;
    return n;
}
} // namespace

size_t drain_writers() {
    Registry& reg = registry();
    std::vector<std::pair<std::shared_ptr<GpxWriter::Shared>, std::thread>> workers;
    {
        std::lock_guard<std::mutex> lock(reg.mutex);
        workers.swap(reg.workers);
    }
    // Close every track that is still open (a live tracker at exit included),
    // then wait for the workers, all against the one process-wide deadline.
    for (auto& w : workers) {
        GpxWriter::request_close(*w.first, /*finalize=*/true);
    }
    const auto deadline = shutdown_deadline();
    size_t stuck = 0;
    for (auto& w : workers) {
        GpxWriter::Shared& s = *w.first;
        bool finished;
        bool report = false;
        std::string what;
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            finished = s.cv.wait_until(lock, deadline, [&s] { return s.done; });
            if (!finished && !s.reported) {
                s.reported = true;
                report = true;
                what = s.dir + "/" + (s.filename.empty() ? std::string("(new track)") : s.filename);
            }
        }
        if (finished) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                report_orphan_failures_locked(s);
            }
            w.second.join();
            continue;
        }
        ++stuck; // storage hung past the deadline: leave it, never destructed
        if (report) {
            std::fprintf(stderr, "gps: GPX write unfinished at shutdown (storage not responding): %s "
                                 "- the track may be incomplete\n", what.c_str());
        }
        std::lock_guard<std::mutex> lock(reg.mutex);
        reg.workers.push_back(std::move(w));
    }
    return stuck + g_orphan_failures.load();
}

size_t live_writers() {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    size_t n = 0;
    for (auto& w : reg.workers) {
        std::lock_guard<std::mutex> l(w.first->mutex);
        n += w.first->done ? 0 : 1;
    }
    return n;
}

GpxWriter::GpxWriter(std::string dir, int delay_ms) : shared_(std::make_shared<Shared>()) {
    shared_->dir = std::move(dir);
    shared_->delay_ms = delay_ms;
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    // Reap workers that already finished (cheap joins).
    for (auto it = reg.workers.begin(); it != reg.workers.end();) {
        bool done;
        {
            std::lock_guard<std::mutex> l(it->first->mutex);
            done = it->first->done;
            if (done) {
                report_orphan_failures_locked(*it->first); // before forgetting it
            }
        }
        if (done) {
            it->second.join();
            it = reg.workers.erase(it);
        } else {
            ++it;
        }
    }
    std::shared_ptr<Shared> s = shared_;
    reg.workers.emplace_back(s, std::thread([s] { s->run(); }));
}

GpxWriter::~GpxWriter() {
    // Never blocks: queued work finishes on the worker, which the registry owns.
    request_close(*shared_, /*finalize=*/false);
}

void GpxWriter::write_point(const std::string& name, const GpxPoint& p) {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        Job job;
        job.kind = Job::Kind::Point;
        job.name = name;
        job.point = p;
        shared_->jobs.push_back(std::move(job));
    }
    shared_->cv.notify_all();
}

void GpxWriter::finalize() {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->finalize_queued) {
            return;
        }
        shared_->finalize_queued = true;
        Job job;
        job.kind = Job::Kind::Finalize;
        shared_->jobs.push_back(std::move(job));
    }
    shared_->cv.notify_all();
}

bool GpxWriter::take_result(Result& out) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->results.empty()) {
        return false;
    }
    out = std::move(shared_->results.front());
    shared_->results.pop_front();
    return true;
}

bool GpxWriter::idle() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->jobs.empty() && !shared_->busy && shared_->results.empty();
}

bool GpxWriter::wait_idle(int timeout_ms) const {
    std::unique_lock<std::mutex> lock(shared_->mutex);
    // Idle apart from uncollected results: the worker has nothing left to do.
    return shared_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
        return shared_->jobs.empty() && !shared_->busy;
    });
}

} // namespace platform::gps
