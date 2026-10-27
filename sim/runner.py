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

import subprocess
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
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()
        if self._thread:
            self._thread.join(timeout=2)

    def drain(self):
        """Return (new_lines, exited). Called from the UI loop."""
        with self._lock:
            lines, self._pending = self._pending, []
            return lines, self._exited

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
