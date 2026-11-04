# Bruce CardputerZero — pygame simulator

A Python + pygame rewrite of the LVGL app in [`../src`](../src), so you can run
the Bruce UI on a desktop without the CardputerZero hardware.

It reproduces the real app at its native **320×170** LCD resolution (scaled up
for the desktop): boot splash, the purple/black category **carousel**, the
per-category **tool lists**, the live **tool-run log**, light/dark toggle and
accent **theme** picker — with the same 5-button nav bar and key mapping as the
device.

The top **status bar** mirrors Bruce firmware's `drawStatusBar()`: clock + date
on the left, Wi-Fi / Bluetooth icons in the middle (only when active), and a
battery gauge on the right (red under 16%, `+` when charging). It reads the
host's real clock and `/sys` state (`status.py`), so on a laptop it shows your
actual time, Wi-Fi link and battery.

## Run

```sh
./sim/run.sh            # simulation (default) — sets up a venv on first run, nothing is touched
./sim/run.sh --real     # also subprocess the plain tool rows (curl, df, date…)
```

`run.sh` creates `sim/.venv` and installs pygame the first time (many distros
block `pip install` into system Python — PEP 668). To do it by hand instead:

```sh
python3 -m venv sim/.venv && sim/.venv/bin/pip install pygame
sim/.venv/bin/python sim/bruce_sim.py
```

## Controls (same as `src/platform/linux_input.cpp`)

| Key | Slot | Menu | Tool list | Run log |
|-----|------|------|-----------|---------|
| `Esc` | 0 | *quit* | Back | Back |
| `F` `Z` `↑` `←` | 1 | Prev | Up | Scroll up |
| `Enter` | 2 | Select | Select | Run / Stop |
| `X` `C` `↓` `→` | 3 | Next | Down | Scroll down |
| `T` | 4 | Light/Dark | Light/Dark | Clear |

### Terminal screen

The **Terminal** category has two rows — *Terminal (user)* and *Root terminal
(sudo)* (both open the same screen; root is chosen per command). The terminal
has an output log above a command input line, with nav slots
`Back · Log Up · Run/Stop · Log Down · Type (keyboard-focus toggle)`; the log
tail-follows unless you scrolled up with `Log Up`/`Log Down`.

- **Typing focus:** Enter runs the typed command; `↑`/`↓` recall history;
  `Esc` leaves typing (nav mode), `Esc` again goes back.
- **Root is per command:** a command typed with a leading `sudo ` runs as root
  (the prefix is stripped). Root commands run as `sudo -n <self> --root-sh <cmd>`
  when a passwordless sudo rule exists, otherwise `sudo -k -S -p '' <self>
  --root-sh <cmd>` with the password typed into the terminal line when prompted.
  `--root-sh` is a sudoers-scoped passthrough that re-execs this script under
  sudo and runs `sh -c <cmd>`; plain commands run as `sh -c <cmd>`.
- **Default mode** prints a simulated transcript — nothing is executed.

You can also click the five on-screen nav buttons.

## Interactive results (Bruce `loopOptions`)

List-producing tools open a selectable menu of their results instead of just
streaming text — pick a result and act on it, with drill-down:

- **Wi-Fi → Scan / List SSIDs** → pick an SSID → *Show details · Clone to Evil
  Portal · Deauth clients · Capture handshake*
- **Bluetooth → Scan** → pick a device → *Show details · Connect (GATT enum)*
- **Nmap → Scan hosts** → pick a host → *Details · Fast scan · Full scan · Ping*
- **SMB → List shares** → pick a share → *Details · List contents*

`Select` drills in (rows ending `›` have sub-options); `Back` pops one level.
Add interactive rows by giving a `catalog.py` action an `items=[Item(...)]`
list (an `Item` with `children` is a sub-menu, otherwise its `lines` are the
detail text). Offensive sub-options stay simulation-only stubs.

## What's simulated

This is a **simulator**: a desktop has no wlan0, no CC1101/ST25R CAP, no SDR.
Every tool streams canned output (`catalog.py`'s `sim` lines) instead of
running the real command. Offensive / credential-capturing rows (Evil Portal,
deauth, BLE spam, Responder) are **status-only stubs** that perform nothing —
they exist so the menu matches the device. `--real` runs only the plain,
non-hardware rows as real subprocesses.

## Layout (mirrors the C++ MVVM split)

| File | Ports |
|------|-------|
| `theme.py` | `src/view/ui_const.h`, `theme.h`, `accent_presets()` |
| `status.py` | Bruce firmware `drawStatusBar()` — clock / Wi-Fi / BLE / battery |
| `catalog.py` | `src/model/tool_catalog.cpp` (data + simulated output) |
| `runner.py` | `src/platform/process_runner.*` |
| `bruce_sim.py` | `src/view/*`, `src/app/*`, `main.cpp` |

Each module has an `assert`-based self-check: `python3 catalog.py`,
`python3 runner.py`.

> ⚠️ For authorized security testing, CTF and research only — same notice as the
> device firmware.
