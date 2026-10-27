"""Top-bar status — the data behind Bruce's drawStatusBar().

On the real firmware the bar shows the RTC clock (left), active connection
icons (center) and the battery (right). Here we read the host's real clock and
its /sys state, so the simulator's bar is live: your laptop's time, Wi-Fi link
and battery. Pure data (no pygame); polled values are cached so the 60fps draw
loop isn't stat()-ing /sys every frame.
"""

import glob
import os
import time
from datetime import datetime

_cache = {"t": -1e9, "wifi": False, "ble": False, "bat": None}


def clock():
    """(HH:MM, DD/MM) from the host clock — Bruce's timeStr."""
    n = datetime.now()
    return n.strftime("%H:%M"), n.strftime("%d/%m")


def _read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def _detect():
    # Wi-Fi: any wireless iface whose link is up (Bruce: WiFi.getMode() != 0).
    wifi = any(_read(os.path.join(os.path.dirname(p), "operstate")) == "up"
               for p in glob.glob("/sys/class/net/*/wireless"))
    # BLE: a Bluetooth adapter exists (Bruce: BLEConnected).
    ble = bool(glob.glob("/sys/class/bluetooth/hci*"))
    # Battery: first BAT* with a numeric capacity -> (percent, charging).
    bat = None
    for b in sorted(glob.glob("/sys/class/power_supply/BAT*")):
        cap = _read(os.path.join(b, "capacity"))
        if cap and cap.isdigit():
            bat = (int(cap), _read(os.path.join(b, "status")) == "Charging")
            break
    return wifi, ble, bat


def status(interval=5.0):
    """(wifi_up, ble_present, battery) with battery = (pct, charging) or None."""
    now = time.time()
    if now - _cache["t"] > interval:
        try:
            _cache["wifi"], _cache["ble"], _cache["bat"] = _detect()
        except Exception:                               # /sys quirks — keep last
            pass
        _cache["t"] = now
    return _cache["wifi"], _cache["ble"], _cache["bat"]


if __name__ == "__main__":
    hhmm, ddmm = clock()
    assert len(hhmm) == 5 and len(ddmm) == 5, (hhmm, ddmm)
    wifi, ble, bat = status()
    assert isinstance(wifi, bool) and isinstance(ble, bool)
    assert bat is None or (isinstance(bat, tuple) and len(bat) == 2)
    assert status() == (wifi, ble, bat)                 # cached, no re-stat
    print(f"status OK — {hhmm} {ddmm}  wifi={wifi} ble={ble} bat={bat}")
