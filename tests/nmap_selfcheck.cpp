/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Standalone self-check for the Nmap guided-flow state machine. Not part of the
 * CMake build. Run manually:
 *
 *   g++ -std=c++17 -Isrc/model src/model/tool_catalog.cpp \
 *       tests/nmap_selfcheck.cpp -o /tmp/nmap_check && /tmp/nmap_check
 *
 * Verifies the discover -> pick host -> Fast/Full drill-down: row layout in each
 * state, navigation rows (empty argv), and the state transitions that
 * on_navigation_action() applies - the part that silently breaks the flow if the
 * builder and the handler disagree on which row maps to which host. It also
 * runs the discovery script against stub `nmap` / `ip` binaries to check the
 * hosts file (ip/mac/vendor/flag, gateway first), the summary line, and the
 * failure / not-installed paths, then the Fast/Full HOST INFO scripts: output
 * format, live streaming of open ports, exit-status propagation and Stop
 * (process-group SIGTERM, as ProcessRunner does).
 */

#include "tool_catalog.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <chrono>
#include <csignal>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static const model::ToolCategory& nmap_category() {
    model::refresh_dynamic_categories();
    for (const auto& c : model::tool_catalog()) {
        if (std::string(c.label) == "Nmap") return c;
    }
    std::fprintf(stderr, "no Nmap category\n");
    std::abort();
}

static void write_exec(const std::string& path, const std::string& body) {
    { std::ofstream o(path); o << body; }
    chmod(path.c_str(), 0755);
}

// Single-quote `s` for sh.
static std::string shq(const std::string& s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    return out + "'";
}

struct RunResult {
    std::string out;
    int code;
};

// Run `script` via `env <env> /bin/sh -c <script> <args...>` (stderr folded
// into stdout). `args` become $0, $1, ... exactly like a catalog argv tail.
static RunResult run_script(const std::string& env, const std::string& script,
                            const std::vector<std::string>& args = {}) {
    std::string cmd = "env -u SUDO_UID -u SUDO_GID " + env + " /bin/sh -c " + shq(script);
    for (const auto& a : args) cmd += " " + shq(a);
    cmd += " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    assert(p);
    RunResult r{{}, -1};
    char buf[512];
    while (std::fgets(buf, sizeof buf, p)) r.out += buf;
    const int st = pclose(p);
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}

static int count_lines(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (!line.empty()) ++n;
    }
    return n;
}

static std::string exec_tests(const std::string& dir, const std::string& script) {
    char tmpl[] = "/tmp/nmap_selfcheck_XXXXXX";
    const char* t = mkdtemp(tmpl);
    const bool made = t != nullptr;
    assert(made);
    const std::string tmp = t;
    const std::string stub = tmp + "/stub";
    const std::string bare = tmp + "/bare";
    mkdir(stub.c_str(), 0755);
    mkdir(bare.c_str(), 0755);

    // Stub nmap. Discovery (-sn): realistic normal output on stdout and in
    // the -oN file - hosts out of order, the gateway with a vendor, this
    // device without a MAC, an "(Unknown)" vendor; EMPTY=1 finds nothing.
    // Port scans: `-v` style "Discovered open port" lines, a --stats-every
    // timing line and the final PORT table; NOPORTS=1 finds nothing, SLOW=N
    // sleeps N s after the first port (streaming / Stop checks), FAIL=1 exits 1
    // with an error on stderr.
    write_exec(stub + "/nmap",
               "#!/bin/sh\n"
               "echo \"$@\" > \"" + tmp + "/nmap.args\"\n"
               "echo $$ > \"" + tmp + "/nmap.pid\"\n"
               "out=; sn=; for a; do t=$a; done\n"
               "while [ $# -gt 0 ]; do\n"
               "  case \"$1\" in -oN) out=$2; shift ;; -sn) sn=1 ;; esac\n"
               "  shift\n"
               "done\n"
               "if [ \"$FAIL\" = 1 ]; then echo 'stub: boom' >&2; exit 1; fi\n"
               "if [ -n \"$sn\" ]; then\n"
               "  { echo '# Nmap 7.98 scan initiated'\n"
               "    if [ \"$EMPTY\" != 1 ]; then\n"
               "    printf 'Nmap scan report for 10.9.8.42\\nHost is up (0.0031s latency).\\n"
               "MAC Address: B8:27:EB:00:11:22 (Raspberry Pi Trading)\\n'\n"
               "    printf 'Nmap scan report for 10.9.8.100\\nHost is up.\\n"
               "MAC Address: 02:00:00:AA:BB:CC (Unknown)\\n'\n"
               "    printf 'Nmap scan report for 10.9.8.1\\nHost is up (0.0020s latency).\\n"
               "MAC Address: 3C:84:6A:AA:BB:01 (TP-LINK Technologies)\\n'\n"
               "    printf 'Nmap scan report for 10.9.8.7\\nHost is up.\\n'\n"
               "    fi\n"
               "    echo '# Nmap done at Sat -- 256 IP addresses scanned'\n"
               "  } > \"${out:-/dev/null}\"\n"
               "  cat \"${out:-/dev/null}\"\n"
               "  exit 0\n"
               "fi\n"
               "echo 'Starting Nmap 7.98 ( https://nmap.org )'\n"
               "echo 'Initiating Connect Scan'\n"
               "echo \"Scanning $t [100 ports]\"\n"
               "if [ \"$NOPORTS\" != 1 ]; then\n"
               "  echo \"Discovered open port 22/tcp on $t\"\n"
               "  [ -n \"$SLOW\" ] && sleep \"$SLOW\"\n"
               "  echo 'Connect Scan Timing: About 42.30% done; ETC: 14:31 (0:01:10 remaining)'\n"
               "  echo \"Discovered open port 80/tcp on $t\"\n"
               "  echo \"Discovered open port 22/tcp on $t\"\n"
               "fi\n"
               "echo \"Nmap scan report for $t\"\n"
               "echo 'Host is up (0.0021s latency).'\n"
               "if [ \"$NOPORTS\" != 1 ]; then\n"
               "  printf 'Not shown: 98 closed tcp ports (conn-refused)\\nPORT   STATE SERVICE\\n"
               "22/tcp open  ssh\\n80/tcp open  http\\n\\n'\n"
               "else echo 'All 100 scanned ports on x are in ignored states.'; fi\n"
               "echo 'Read data files from: /usr/share/nmap'\n"
               "echo 'Nmap done: 1 IP address (1 host up) scanned in 0.05 seconds'\n"
               "exit 0\n");
    // Stub ip: default route via wlan0, address prefix from $PFX (default 24).
    write_exec(stub + "/ip",
               "#!/bin/sh\n"
               "case \"$*\" in\n"
               "  *route*) echo 'default via 10.9.8.1 dev wlan0 proto dhcp metric 600' ;;\n"
               "  *addr*) echo \"3: wlan0    inet 10.9.8.7/${PFX:-24} brd 10.9.8.255 scope global wlan0\" ;;\n"
               "esac\n");
    // "bare" PATH (empty dir): the nmap check is the script's first step.

    const std::string path_env = "PATH=" + stub + ":" + std::getenv("PATH");
    setenv("NMAP_SELFCHECK_TMP", tmp.c_str(), 1);

    // Success: hosts file gets both hosts, summary line reports 2.
    {
        const auto r = run_script(path_env, script);
        if (r.code != 0) std::fprintf(stderr, "%s\n", r.out.c_str());
        assert(r.code == 0);
        assert(r.out.find("Discovering hosts on 10.9.8.7/24 (wlan0)") != std::string::npos);
        assert(r.out.find("Nmap scan report for 10.9.8.42") != std::string::npos); // streamed
        assert(r.out.find("Done: 4 host(s) found (gateway 10.9.8.1)") != std::string::npos);
        // ip<TAB>mac<TAB>vendor<TAB>flag; gateway first, then numeric order
        // (.7 < .42 < .100); this device has no MAC; "(Unknown)" -> empty.
        std::ifstream in(dir + "/hosts");
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(all ==
               "10.9.8.1\t3C:84:6A:AA:BB:01\tTP-LINK Technologies\tgw\n"
               "10.9.8.7\t\t\tself\n"
               "10.9.8.42\tB8:27:EB:00:11:22\tRaspberry Pi Trading\t\n"
               "10.9.8.100\t02:00:00:AA:BB:CC\t\t\n");
        std::ifstream args(tmp + "/nmap.args");
        std::string a;
        std::getline(args, a);
        assert(a.find("-sn -n 10.9.8.7/24 -oN") != std::string::npos);
    }
    // Nothing up: "No hosts found", empty hosts file, success exit.
    {
        const auto r = run_script(path_env + " EMPTY=1", script);
        assert(r.code == 0);
        assert(r.out.find("No hosts found") != std::string::npos);
        assert(count_lines(dir + "/hosts") == 0);
    }
    // Wide prefix (/16) is clamped to /24.
    {
        const auto r = run_script(path_env + " PFX=16", script);
        assert(r.code == 0);
        assert(r.out.find("Discovering hosts on 10.9.8.7/24") != std::string::npos);
    }
    // nmap failure: reported, non-zero exit, nmap's stderr is visible.
    {
        const auto r = run_script(path_env + " FAIL=1", script);
        assert(r.code != 0);
        assert(r.out.find("nmap failed") != std::string::npos);
        assert(r.out.find("stub: boom") != std::string::npos);
        struct stat st {};
        assert(stat((dir + "/hosts.tmp").c_str(), &st) != 0);   // no temp left behind
    }
    // nmap missing: install hint + 127.
    {
        const auto r = run_script("PATH=" + bare, script);
        assert(r.code == 127);
        assert(r.out.find("nmap is not installed - run System / Clock > "
                          "\"Install optional tools (apt)\" or: sudo apt install nmap") != std::string::npos);
    }

    // Script-side guard: the same script pointed at a symlinked dir refuses
    // to touch it; pointed at a missing dir it creates it (0700) and works.
    {
        const std::string needle = "d='" + dir + "'";
        const size_t at = script.find(needle);
        assert(at != std::string::npos);
        auto with_dir = [&](const std::string& d) {
            std::string s2 = script;
            s2.replace(at, needle.size(), "d='" + d + "'");
            return s2;
        };
        const std::string real = tmp + "/real";
        const std::string link = tmp + "/link";
        mkdir(real.c_str(), 0700);
        const int ln = symlink(real.c_str(), link.c_str());
        assert(ln == 0);
        auto r = run_script(path_env, with_dir(link));
        assert(r.code == 1);
        assert(r.out.find("unsafe nmap state dir") != std::string::npos);
        struct stat st {};
        assert(stat((real + "/hosts").c_str(), &st) != 0);     // nothing written

        // Missing dir: the root script must not create it.
        const std::string fresh = tmp + "/fresh";
        r = run_script(path_env, with_dir(fresh));
        assert(r.code == 1);
        assert(r.out.find("nmap state dir missing") != std::string::npos);
        assert(lstat(fresh.c_str(), &st) != 0);                 // nothing created

        // Group/world-accessible dir (0777) is refused.
        const std::string open_dir = tmp + "/open";
        mkdir(open_dir.c_str(), 0700);
        const int cm = chmod(open_dir.c_str(), 0777);
        assert(cm == 0);
        r = run_script(path_env, with_dir(open_dir));
        assert(r.code == 1);
        assert(r.out.find("unsafe nmap state dir") != std::string::npos);
        assert(stat((open_dir + "/hosts").c_str(), &st) != 0);
        assert(stat((open_dir + "/hosts.tmp").c_str(), &st) != 0);
    }

    // Re-run the real discovery so the hosts file holds the 4 stub hosts for
    // the caller's row-label / Fast / Full checks.
    {
        const auto r = run_script(path_env, script);
        assert(r.code == 0);
        std::remove((dir + "/scan.nmap").c_str());
    }
    return path_env;
}

// Start `script args...` like ProcessRunner (own session, stdout+stderr on one
// pipe) and return {pid, read fd}.
static std::pair<pid_t, int> spawn_like_runner(const std::string& env_path, const std::string& script,
                                               const std::vector<std::string>& args,
                                               const std::vector<std::string>& env_extra) {
    int fds[2];
    const int pr = pipe(fds);
    assert(pr == 0);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        setsid();
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        setenv("PATH", env_path.c_str(), 1);
        for (const auto& e : env_extra) putenv(const_cast<char*>(e.c_str()));
        std::vector<const char*> av{"sh", "-c", script.c_str()};
        for (const auto& a : args) av.push_back(a.c_str());
        av.push_back(nullptr);
        execv("/bin/sh", const_cast<char* const*>(av.data()));
        _exit(127);
    }
    close(fds[1]);
    return {pid, fds[0]};
}

// Read one line from fd with a timeout; false on EOF/timeout.
static bool read_line_fd(int fd, std::string& line, int timeout_ms) {
    line.clear();
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) return false;
        char c;
        if (read(fd, &c, 1) != 1) return false;
        if (c == '\n') return true;
        line += c;
    }
}

static bool have_util_linux_script() {
    return std::system("script -V 2>/dev/null | grep -q util-linux") == 0;
}

static void port_scan_tests(const std::string& path_env, const model::ToolAction& fast,
                            const model::ToolAction& full) {
    // A non-IPv4 $1 is refused before anything runs.
    {
        std::vector<std::string> a(fast.argv.begin() + 3, fast.argv.end());
        a[1] = "1.2.3.4;id";
        const auto r = run_script(path_env, fast.argv[2], a);
        assert(r.code == 2);
        assert(r.out.find("invalid host") != std::string::npos);
    }
    const std::vector<std::string> fargs(fast.argv.begin() + 3, fast.argv.end());
    const std::vector<std::string> gargs(full.argv.begin() + 3, full.argv.end());
    // HOST INFO block, streamed open ports (deduplicated), nmap's table, summary.
    // Also: the log pipe closes as soon as nmap ends (nothing - e.g. the
    // fallback heartbeat's sleep - may keep it open).
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = run_script(path_env, fast.argv[2], fargs);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        assert(dt < 5.0);
        if (r.code != 0) std::fprintf(stderr, "%s\n", r.out.c_str());
        assert(r.code == 0);
        const char* expect =
            "HOST INFO\n"
            "IP: 10.9.8.1 (GTW)\n"
            "MAC: 3C:84:6A:AA:BB:01  Vendor: TP-LINK Technologies\n"
            "Scanning ports... wait\n"
            "+ 22/tcp open\n"
            "progress 42% (0:01:10 left)\n"
            "+ 80/tcp open\n"
            "PORT   STATE SERVICE\n"
            "22/tcp open  ssh\n"
            "80/tcp open  http\n"
            "Done: 2 open port(s)\n";
        if (r.out != expect) std::fprintf(stderr, "got:\n%s", r.out.c_str());
        assert(r.out == expect);
    }
    // Fast/Full nmap flags.
    {
        auto args_of = [&](const model::ToolAction& a, const std::vector<std::string>& av) {
            run_script(path_env, a.argv[2], av);
            std::ifstream in(std::string(std::getenv("NMAP_SELFCHECK_TMP")) + "/nmap.args");
            std::string l;
            std::getline(in, l);
            return l;
        };
        assert(args_of(fast, fargs) == "-Pn -n -v -F -T4 10.9.8.1");
        assert(args_of(full, gargs) == "-Pn -n -v -p- -T4 --stats-every 15s 10.9.8.1");
    }
    // No open ports.
    {
        const auto r = run_script(path_env + " NOPORTS=1", fast.argv[2], fargs);
        assert(r.code == 0);
        assert(r.out.find("No open ports found") != std::string::npos);
        assert(r.out.find("+ ") == std::string::npos);
    }
    // nmap failure: its stderr is shown and its exit status propagates.
    {
        const auto r = run_script(path_env + " FAIL=1", full.argv[2], gargs);
        assert(r.code == 1);
        assert(r.out.find("stub: boom") != std::string::npos);
        assert(r.out.find("nmap failed (exit 1)") != std::string::npos);
    }
    // Host without MAC.
    {
        std::vector<std::string> a = fargs;
        a[2] = "";
        a[3] = "";
        a[4] = "(this device)";
        const auto r = run_script(path_env, fast.argv[2], a);
        assert(r.out.find("MAC: (not available)") != std::string::npos);
        assert(r.out.find("IP: 10.9.8.1 (this device)") != std::string::npos);
    }
    // Streaming: the first open port reaches the log while nmap is still
    // running (SLOW=2), i.e. neither sh nor awk buffers it.
    {
        const std::string pth = path_env.substr(5); // strip "PATH="
        auto [pid, fd] = spawn_like_runner(pth, fast.argv[2], fargs, {"SLOW=2"});
        const auto t0 = std::chrono::steady_clock::now();
        std::string line;
        bool got = false;
        while (read_line_fd(fd, line, 3000)) {
            if (line == "+ 22/tcp open") { got = true; break; }
        }
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        assert(got && dt < 1.5);
        while (read_line_fd(fd, line, 5000)) {}
        close(fd);
        int st = 0;
        waitpid(pid, &st, 0);
        assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    // Stop: SIGTERM to the process group (what ProcessRunner does) ends the
    // whole pipeline - the log pipe reaches EOF promptly and nmap is gone.
    {
        const std::string pth = path_env.substr(5);
        auto [pid, fd] = spawn_like_runner(pth, full.argv[2], gargs, {"SLOW=30"});
        std::string line;
        bool got = false;
        while (read_line_fd(fd, line, 3000)) {
            if (line == "+ 22/tcp open") { got = true; break; }
        }
        assert(got);
        kill(-pid, SIGTERM);
        const auto t0 = std::chrono::steady_clock::now();
        while (read_line_fd(fd, line, 3000)) {}
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        // util-linux script(1) SIGTERMs its child and waits briefly before
        // exiting (and closing the log pipe), hence the slack.
        assert(dt < 4.0);
        close(fd);
        int st = 0;
        waitpid(pid, &st, 0);
        std::ifstream in(std::string(std::getenv("NMAP_SELFCHECK_TMP")) + "/nmap.pid");
        pid_t np = 0;
        in >> np;
        assert(np > 0);
        // Gone, or a zombie awaiting reaping by init (kill(pid, 0) still
        // succeeds on those) - give the reaper up to 2 s.
        auto alive = [](pid_t q) {
            if (kill(q, 0) != 0) return false;
            std::ifstream stat_in("/proc/" + std::to_string(q) + "/stat");
            std::string pid_s, comm, state;
            stat_in >> pid_s >> comm >> state;
            return !(state == "Z" || state == "X");
        };
        bool dead = false;
        for (int i = 0; i < 40 && !(dead = !alive(np)); ++i) usleep(50 * 1000);
        assert(dead);
    }
}

// The C++ side must refuse an unsafe state dir: one non-root error row, and
// navigation must not write into it.
static void unsafe_dir_tests(const std::string& dir, int nmap_idx) {
    std::remove((dir + "/selected").c_str());
    std::remove((dir + "/hosts").c_str());
    std::remove((dir + "/scan.nmap").c_str());
    const int rd = rmdir(dir.c_str());
    assert(rd == 0);

    auto expect_error_row = [&](const char* reason) {
        const auto& c = nmap_category();
        assert(c.actions.size() == 1);
        const auto& a = c.actions[0];
        assert(!a.needs_root);
        assert(a.argv.size() == 3 && a.argv[0] == "sh");
        assert(a.argv[2].find("unsafe nmap state dir " + dir) != std::string::npos);
        assert(a.argv[2].find(reason) != std::string::npos);
        const auto r = run_script("", a.argv[2]);
        assert(r.code == 1);
        assert(r.out.find("unsafe nmap state dir") != std::string::npos);
    };

    // Symlink to a dir we own (planted-link case).
    char tmpl[] = "/tmp/nmap_selfcheck_target_XXXXXX";
    const char* t = mkdtemp(tmpl);
    const bool made = t != nullptr;
    assert(made);
    const std::string target = t;
    { std::ofstream o(target + "/hosts"); o << "6.6.6.6\t\t\t\n"; }
    const int ln = symlink(target.c_str(), dir.c_str());
    assert(ln == 0);
    expect_error_row("symlink");
    model::on_navigation_action(nmap_idx, 1);    // would pick hosts[0]
    struct stat st {};
    assert(stat((target + "/selected").c_str(), &st) != 0);
    std::remove(dir.c_str());

    // A regular file in its place.
    { std::ofstream o(dir); o << "x"; }
    expect_error_row("not a directory");
    std::remove(dir.c_str());

    // Our own dir, but group/world accessible (0777).
    mkdir(dir.c_str(), 0700);
    const int cm = chmod(dir.c_str(), 0777);
    assert(cm == 0);
    expect_error_row("permissions 0777");
    model::on_navigation_action(nmap_idx, 1);
    assert(stat((dir + "/selected").c_str(), &st) != 0);
    const int rd2 = rmdir(dir.c_str());
    assert(rd2 == 0);

    const std::string cleanup = "rm -rf " + shq(target);
    const int rc = std::system(cleanup.c_str());
    assert(rc == 0);

    // Back to normal: the dir is recreated and the root rows return.
    const auto& c = nmap_category();
    assert(c.actions.size() == 2 && c.actions[0].needs_root);
    const int sr = lstat(dir.c_str(), &st);
    assert(sr == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid());
}

// Non-TTY integration test with the REAL nmap (skipped if absent): the Full
// script, with only its nmap flags swapped for a slow localhost scan, must
// show live "progress N%" lines through a pipe (nmap prints --stats-every
// output only to a TTY - this is what the script(1) wrapper is for), then a
// summary and exit 0.
static void real_nmap_progress_test(const model::ToolAction& full) {
    if (std::system("command -v nmap >/dev/null 2>&1") != 0) {
        std::puts("SKIP: real nmap not installed - progress integration test not run");
        return;
    }
    if (!have_util_linux_script()) {
        std::puts("SKIP: util-linux script(1) not installed - real progress test not run");
        return;
    }
    std::string sc = full.argv[2];
    const std::string flags = "-p- -T4 --stats-every 15s";
    const size_t at = sc.find(flags);
    assert(at != std::string::npos);
    sc.replace(at, flags.size(), "-p1-60 --max-rate 10 --stats-every 2s");
    const auto r = run_script("", sc, {"nmap-full", "127.0.0.1", "", "", "(this device)"});
    int progress = 0;
    for (size_t pos = 0; (pos = r.out.find("\nprogress ", pos)) != std::string::npos; ++pos) ++progress;
    if (r.code != 0 || progress < 2) std::fprintf(stderr, "real nmap run:\n%s", r.out.c_str());
    assert(r.code == 0);
    assert(progress >= 2);
    assert(r.out.find("Done: ") != std::string::npos || r.out.find("No open ports found") != std::string::npos);
    std::puts("real nmap progress test OK");
}

int main() {
    const std::string dir = model::nmap_state_dir();
    assert(dir.find(std::to_string(getuid())) != std::string::npos);
    mkdir(dir.c_str(), 0700);
    std::remove((dir + "/selected").c_str());

    // --- state: no hosts discovered yet ---
    std::remove((dir + "/hosts").c_str());
    std::string script;
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 2);               // Scan + "(no hosts yet)"
        assert(!c.actions[0].argv.empty());          // Scan is runnable
        assert(c.actions[0].argv[0] == "sh");        // discovery via sh -c
        assert(c.actions[0].needs_root);
        assert(c.actions[0].argv.size() == 3);
        script = c.actions[0].argv[2];
        assert(script.find("command -v nmap") != std::string::npos);
        assert(script.find(dir) != std::string::npos);
        // nmap's own stderr must reach the log (not piped away/discarded).
        const size_t nm = script.find("nmap -sn");
        assert(nm != std::string::npos);
        const std::string nmap_cmd = script.substr(nm, script.find(';', nm) - nm);
        assert(nmap_cmd.find("2>/dev/null") == std::string::npos);
        assert(script.find("-oG - 2>/dev/null |") == std::string::npos);
        assert(script.find("No hosts found") != std::string::npos);
        // uid baked in; script re-vets the dir and chowns without following links.
        const std::string uid = std::to_string(getuid());
        assert(script.find("o=" + uid + ";") != std::string::npos);
        assert(script.find("[ \"$(stat -c %u \"$d\")\" = \"$o\" ]") != std::string::npos);
        assert(script.find("[ ! -L \"$d\" ]") != std::string::npos);
        assert(script.find("chown -h \"$o\"") != std::string::npos);
        assert(script.find("SUDO_UID") == std::string::npos);
        assert(script.find("stat -c %a") != std::string::npos);
        assert(script.find("mkdir") == std::string::npos);      // root never creates it
        assert(script.find("chown -h \"$o\" \"$d\";") == std::string::npos);
    }

    const std::string path_env = exec_tests(dir, script);

    int nmap_idx = -1;
    {
        const auto& cat = model::tool_catalog();
        for (size_t i = 0; i < cat.size(); ++i) {
            if (std::string(cat[i].label) == "Nmap") nmap_idx = (int)i;
        }
    }
    assert(nmap_idx >= 0);

    // --- rows built from the discovered hosts file (Bruce-style labels) ---
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 1 + 4);
        assert(std::string(c.actions[1].label) == "10.9.8.1 (GTW) TP-LINK");
        assert(std::string(c.actions[2].label) == "10.9.8.7 (me)");
        assert(std::string(c.actions[3].label) == "10.9.8.42 Raspberry Pi");
        assert(std::string(c.actions[4].label) == "10.9.8.100");
        for (int i = 1; i <= 4; ++i) assert(c.actions[i].argv.empty());
    }
    model::on_navigation_action(nmap_idx, 1);        // pick the gateway
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 3);
        const auto& fast = c.actions[1];
        const auto& full = c.actions[2];
        assert(!fast.needs_root && !full.needs_root);   // TCP connect scan, no sudo
        assert(fast.argv.size() == 8 && fast.argv[0] == "sh" && fast.argv[1] == "-c");
        assert(fast.argv[4] == "10.9.8.1" && fast.argv[5] == "3C:84:6A:AA:BB:01");
        assert(fast.argv[6] == "TP-LINK Technologies" && fast.argv[7] == "(GTW)");
        assert(fast.argv[2].find("-F -T4") != std::string::npos);
        assert(full.argv[2].find("-p- -T4 --stats-every 15s") != std::string::npos);
        assert(full.argv[4] == "10.9.8.1");
        // host data is passed as arguments, never spliced into the script
        assert(fast.argv[2].find("10.9.8.1") == std::string::npos);
        assert(fast.argv[2].find("fflush()") != std::string::npos);
        // Both progress paths: nmap under script(1)'s PTY (if util-linux
        // script exists here) and the no-PTY heartbeat fallback (a fake
        // non-util-linux `script` first in PATH).
        if (have_util_linux_script()) {
            port_scan_tests(path_env, fast, full);
        } else {
            std::puts("SKIP: util-linux script(1) not installed - PTY path not exercised");
        }
        const std::string noscript = std::string(std::getenv("NMAP_SELFCHECK_TMP")) + "/noscript";
        mkdir(noscript.c_str(), 0755);
        write_exec(noscript + "/script", "#!/bin/sh\necho 'script from busybox'\nexit 1\n");
        port_scan_tests("PATH=" + noscript + ":" + path_env.substr(5), fast, full);
        real_nmap_progress_test(full);
    }
    model::on_navigation_action(nmap_idx, 0);        // back
    {
        const std::string cleanup = "rm -rf " + shq(std::getenv("NMAP_SELFCHECK_TMP"));
        const int rc = std::system(cleanup.c_str());
        assert(rc == 0);
    }

    // --- state: hosts discovered, none selected (older bare-IP format and
    //     junk lines too: invalid entries are skipped by builder and handler
    //     alike, so row N still maps to the N-th valid host) ---
    { std::ofstream o(dir + "/hosts");
      o << "192.168.1.1\nnot-an-ip\n192.168.1.50\t\tEvil'$(reboot)`x`\t\n1.2.3.4; rm -rf /\tx\n10.0.0.9\n"; }
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 1 + 3);           // Scan + 3 valid host rows
        assert(!c.actions[0].argv.empty());          // Scan runnable
        for (int i = 1; i <= 3; ++i) {
            assert(c.actions[i].argv.empty());       // host rows are navigation
        }
        assert(std::string(c.actions[2].label) == "192.168.1.50 Evil(reboot)x");
    }

    // --- pick the 2nd host (row index 2) -> selected becomes 192.168.1.50 ---
    model::on_navigation_action(/*category*/ -999, 2); // wrong category: no-op
    model::on_navigation_action(nmap_idx, 2);        // row 2 == 2nd valid host

    // --- state: host selected -> Back + Fast + Full ---
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 3);
        assert(c.actions[0].argv.empty());           // "Back to host list" (nav)
        assert(c.actions[1].argv[4] == "192.168.1.50");
        assert(c.actions[1].argv[6] == "Evil(reboot)x"); // sanitized
        assert(c.actions[2].argv[4] == "192.168.1.50");
        assert(!c.actions[1].needs_root && !c.actions[2].needs_root);
        assert(std::string(c.actions[1].label).find("Fast scan 192.168.1.50") == 0);
    }

    // --- "Back to host list" (row 0) clears selection ---
    model::on_navigation_action(nmap_idx, 0);
    {
        const auto& c = nmap_category();
        assert(c.actions.size() == 1 + 3);           // back to host list
        assert(c.actions[2].argv.empty());
    }

    // --- a corrupt `selected` is ignored (host list shown again) ---
    { std::ofstream o(dir + "/selected"); o << "999.1.1.1\n"; }
    assert(nmap_category().actions.size() == 1 + 3);
    std::remove((dir + "/selected").c_str());

    // --- "Install optional tools (apt)" (named by the not-installed hint)
    //     lives in "System / Clock" and installs nmap ---
    {
        bool found = false;
        for (const auto& c : model::tool_catalog()) {
            for (const auto& a : c.actions) {
                if (std::string(a.label) != "Install optional tools (apt)") continue;
                found = true;
                assert(std::string(c.label) == "System / Clock");
                assert(a.needs_root);
                assert(a.argv[2].find("apt-get install -y nmap ") != std::string::npos);
                assert(std::string(a.description).find("sudoers rule") != std::string::npos);
                assert(std::string(a.description).find("sudo password") != std::string::npos);
            }
        }
        assert(found);
    }

    // --- Wi-Fi "Scan LAN hosts (nmap)" shares the guard + subnet logic ---
    {
        bool found = false;
        for (const auto& c : model::tool_catalog()) {
            if (std::string(c.label) != "Wi-Fi") continue;
            for (const auto& a : c.actions) {
                if (std::string(a.label) != "Scan LAN hosts (nmap)") continue;
                found = true;
                assert(a.needs_root);
                assert(a.argv.size() == 3);
                assert(a.argv[2].find("nmap is not installed - run System / Clock > "
                                      "\"Install optional tools (apt)\"") != std::string::npos);
                assert(a.argv[2].find("nmap -sn -n \"$net\" 2>&1") != std::string::npos);
            }
        }
        assert(found);
    }

    unsafe_dir_tests(dir, nmap_idx);

    std::remove((dir + "/selected").c_str());
    std::remove((dir + "/hosts").c_str());
    std::puts("nmap self-check OK");
    return 0;
}
