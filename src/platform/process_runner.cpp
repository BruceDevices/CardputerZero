/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "process_runner.h"

#include <array>
#include <cerrno>
#include <cstring>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace platform {
namespace {

constexpr size_t kReadChunkSize = 512;
constexpr uint32_t kPollPeriodMs = 50;

} // namespace

ProcessRunner::ProcessRunner() = default;

ProcessRunner::~ProcessRunner() {
    stop();
}

bool ProcessRunner::start(const std::vector<std::string>& argv, UpdateCallback on_update, void* owner) {
    if (running_ || argv.empty() || !on_update) {
        return false;
    }

    // A previous run may have finished on its own (running_ == false) but left
    // its thread joinable; joining it now avoids terminate() on reassignment.
    join_reader();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_text_.clear();
        pending_exit_ = false;
    }

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }

    if (pid == 0) {
        // Child: own process group so stop() can signal the whole tree
        // (nc/responder/bluetoothctl children included), stdout+stderr both
        // routed into the pipe so the log view shows everything in order.
        setsid();
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);

        std::vector<char*> exec_argv;
        exec_argv.reserve(argv.size() + 1);
        for (const auto& arg : argv) {
            exec_argv.push_back(const_cast<char*>(arg.c_str()));
        }
        exec_argv.push_back(nullptr);

        execvp(exec_argv[0], exec_argv.data());
        const std::string message = "exec failed: " + argv[0] + ": " + std::strerror(errno) + "\n";
        write(STDERR_FILENO, message.c_str(), message.size());
        _exit(127);
    }

    close(pipe_fds[1]);
    child_pid_ = pid;
    read_fd_ = pipe_fds[0];
    on_update_ = on_update;
    owner_ = owner;
    running_ = true;

    reader_thread_ = std::thread(&ProcessRunner::reader_thread_main, this);
    poll_timer_ = lv_timer_create(poll_timer_cb, kPollPeriodMs, this);
    return true;
}

void ProcessRunner::stop() {
    // ponytail: SIGTERM only. join_reader() blocks until the child's pipe
    // closes, so a tool that *ignores* SIGTERM would hang the UI here. Every
    // catalog tool respects it (timeout-wrapped ones self-limit); escalate to
    // SIGKILL or a poll()-with-timeout reader loop if that ever stops holding.
    if (running_ && child_pid_ > 0) {
        kill(-child_pid_.load(), SIGTERM);
    }

    join_reader();

    if (poll_timer_) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }
}

void ProcessRunner::join_reader() {
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

bool ProcessRunner::running() const {
    return running_;
}

void ProcessRunner::reader_thread_main() {
    std::array<char, kReadChunkSize> buffer{};

    ssize_t bytes_read;
    while ((bytes_read = read(read_fd_, buffer.data(), buffer.size())) > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_text_.append(buffer.data(), static_cast<size_t>(bytes_read));
    }

    close(read_fd_);
    read_fd_ = -1;

    int status = 0;
    waitpid(child_pid_.load(), &status, 0);
    child_pid_ = -1;
    running_ = false;

    std::lock_guard<std::mutex> lock(mutex_);
    pending_exit_ = true;
}

void ProcessRunner::poll_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<ProcessRunner*>(lv_timer_get_user_data(timer));

    std::string text;
    bool exited;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        text.swap(self->pending_text_);
        exited = self->pending_exit_;
        self->pending_exit_ = false;
    }

    if (self->on_update_ && (!text.empty() || exited)) {
        self->on_update_(self->owner_, text, exited);
    }

    // Once the exit has been delivered the timer has nothing left to do; the
    // reader thread is joined lazily on the next start()/stop().
    if (exited) {
        lv_timer_delete(timer);
        if (self->poll_timer_ == timer) {
            self->poll_timer_ = nullptr;
        }
    }
}

} // namespace platform
