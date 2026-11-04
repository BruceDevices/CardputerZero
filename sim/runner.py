"""ProcessRunner — Python port of src/platform/process_runner.*.

Same threading model as the C++: a background thread fills a lock-protected
buffer, the UI thread polls it with drain(). start() plays an action; the
renderer never blocks.

Two modes:
  sim (default) — streams the action's canned `sim` lines with small delays.
                  Nothing on the machine is touched. This is the whole point of
                  the pygame build.
  real (--real) — subprocess the action's real argv (like the device does),
                  for plain tool rows only. Rows whose argv targets device-only
                  hardware ("self", wlan0, spidev, ...) stay simulated even in
                  real mode, because a desktop has none of it.
"""

import os
import subprocess
import sys
import threading
import time


# argv[0] values that only mean something on the CardputerZero (native
# re-execs, radios, CAP) — never run these as real subprocesses on a desktop.
_DEVICE_ONLY = {"self", "iw", "hostapd", "dnsmasq", "airmon-ng", "airodump-ng",
                "mdk4", "hcitool", "bluetoothctl", "responder", "rfkill",
                "flashrom", "i2cdetect", "gpiodetect", "rtl_433", "rtl_test"}


class ProcessRunner:
    def __init__(self, real=False):
        self.real = real
        self._lock = threading.Lock()
        self._pending = []        # list[str] lines captured since last drain
        self._exited = False
        self._thread = None
        self._stop = threading.Event()
        self._proc = None

    def running(self):
        return self._thread is not None and self._thread.is_alive()

    def start(self, action):
        if self.running():
            return False
        self._stop.clear()
        with self._lock:
            self._pending = []
            self._exited = False
        real = self.real and action.argv and action.argv[0] not in _DEVICE_ONLY
        target = self._run_real if real else self._run_sim
        self._thread = threading.Thread(target=target, args=(action,), daemon=True)
        self._thread.start()
        return True

    def stop(self):
        self._stop.set()
        try:
            self._pw_event.set()        # release a worker waiting on a sudo password
        except AttributeError:
            pass
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()
        if self._thread:
            self._thread.join(timeout=2)

    def drain(self):
        """Return (new_lines, exited). Called from the UI loop."""
        with self._lock:
            lines, self._pending = self._pending, []
            return lines, self._exited

    # ── Terminal ────────────────────────────────────────────────────────────
    # One typed command from the Terminal screen. Real mode mirrors the native
    # app's execution semantics:
    #   user command            -> sh -c <cmd>
    #   root, already root      -> sh -c <cmd>
    #   root, passwordless rule -> sudo -n <self> --root-sh <cmd>
    #   root, otherwise         -> sudo -k -S -p '' <self> --root-sh <cmd>
    #                             with the password fed on stdin on demand.
    def start_command(self, cmd, root=False):
        if self.running():
            return False
        self._stop.clear()
        self._pw_needed = False
        self._pw = None
        self._pw_event = threading.Event()
        with self._lock:
            self._pending = []
            self._exited = False
        target = self._cmd_real if self.real else self._cmd_sim
        self._thread = threading.Thread(target=target, args=(cmd, root), daemon=True)
        self._thread.start()
        return True

    def password_pending(self):
        """True while the worker is blocked asking for a sudo password."""
        return getattr(self, "_pw_needed", False) and self._pw is None

    def provide_password(self, pw):
        self._pw = pw
        self._pw_event.set()

    def _self_argv(self, cmd):
        # <self> = this simulator, re-invoked with the --root-sh passthrough.
        return [sys.executable, os.path.abspath(sys.argv[0]), "--root-sh", cmd]

    def _cmd_sim(self, cmd, root):
        self._emit(("# " if root else "$ ") + cmd)
        for line in ("[sim] simulation only — nothing is executed on this machine",
                     "[sim] with --real this runs via sh -c / sudo ... --root-sh"):
            if self._stop.is_set():
                break
            self._emit(line)
            time.sleep(0.12)
        self._finish()

    def _cmd_real(self, cmd, root):
        stdin_line = None
        try:
            if root and os.geteuid() != 0:
                passwordless = subprocess.run(
                    ["sudo", "-n", "true"], capture_output=True).returncode == 0
                if passwordless:
                    argv = ["sudo", "-n", *self._self_argv(cmd)]
                else:
                    self._pw_needed = True
                    self._emit("[sudo] password: type it, then press Enter")
                    self._pw_event.wait(timeout=120)
                    if self._stop.is_set() or self._pw is None:
                        self._emit("sudo: no password entered — aborted")
                        self._finish()
                        return
                    argv = ["sudo", "-k", "-S", "-p", "", *self._self_argv(cmd)]
                    stdin_line = self._pw + "\n"
            else:
                argv = ["sh", "-c", cmd]
            self._proc = subprocess.Popen(
                argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                stdin=subprocess.PIPE if stdin_line is not None else None,
                text=True, bufsize=1)
            if stdin_line is not None:
                self._proc.stdin.write(stdin_line)
                self._proc.stdin.close()
            for line in self._proc.stdout:
                if self._stop.is_set():
                    break
                self._emit(line.rstrip("\n"))
            self._proc.wait(timeout=3)
            self._emit(f"[process exited {self._proc.returncode}]")
        except FileNotFoundError:
            self._emit(f"{argv[0]}: not found on this machine")
        except Exception as e:                      # noqa: BLE001 — surface to log
            self._emit(f"error: {e}")
        self._finish()

    # ── workers ────────────────────────────────────────────────────────────
    def _emit(self, line):
        with self._lock:
            self._pending.append(line)

    def _finish(self):
        with self._lock:
            self._exited = True

    def _run_sim(self, action):
        for line in action.sim:
            if self._stop.is_set():
                self._emit("^C  (stopped)")
                break
            self._emit(line)
            time.sleep(0.12)        # readable streaming; feels like a live tool
        self._finish()

    def _run_real(self, action):
        try:
            self._proc = subprocess.Popen(
                action.argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1)
            for line in self._proc.stdout:
                if self._stop.is_set():
                    break
                self._emit(line.rstrip("\n"))
            self._proc.wait(timeout=3)
            self._emit(f"[process exited {self._proc.returncode}]")
        except FileNotFoundError:
            self._emit(f"{action.argv[0]}: not found on this machine")
        except Exception as e:                      # noqa: BLE001 — surface to log
            self._emit(f"error: {e}")
        self._finish()


if __name__ == "__main__":
    # ponytail self-check: a sim run streams every line then reports exit.
    class A:
        argv, sim, nav = [], ["one", "two", "three"], False
    r = ProcessRunner()
    assert r.start(A()) and not r.start(A())        # second start refused
    got, exited = [], False
    while not exited:
        lines, exited = r.drain()
        got += lines
        time.sleep(0.05)
    assert got == ["one", "two", "three"], got
    assert not r.running()
    print("runner OK")
