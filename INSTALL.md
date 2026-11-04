# Installing Bruce on CardputerZero

A step-by-step guide to build this app and get it running on your
CardputerZero. No prior CMake knowledge needed — copy/paste the commands.

> The CardputerZero is a full aarch64 (ARM64) Linux device. We build a `.deb`
> package on a **helper computer** (any Linux PC/laptop or WSL), copy it to the
> device, and install it with `apt`. `apt` also pulls in the security tools the
> app launches (curl, netcat, smbclient, iw, bluez, rfkill, …) automatically.

---

## What you need

- **The CardputerZero**, powered on and on the same network as your computer.
  You need its **IP address** and a **login user** (SSH enabled).
- **A Linux computer** (or Windows with WSL) to build on. This is the
  "helper". It does not need to be powerful.

Find the device IP: on the CardputerZero terminal run `ip addr` — look for the
`wlan0` address (e.g. `192.168.1.50`).

---

## Step 1 — Install the build tools on the helper computer

On Debian/Ubuntu/WSL:

```bash
sudo apt update
sudo apt install -y \
  git cmake ninja-build \
  crossbuild-essential-arm64 \
  dpkg-dev
```

> `crossbuild-essential-arm64` gives you `aarch64-linux-gnu-gcc`, the compiler
> that produces ARM64 binaries for the device. `cmake` must be **3.31+**
> (`cmake --version`). On older Ubuntu: `sudo snap install cmake --classic` and
> use `/snap/bin/cmake` in the commands below.

---

## Step 2 — Get the code

```bash
git clone https://github.com/BruceDevices/CardputerZero.git
cd CardputerZero
```

---

## Step 3 — Build the package

Everything (compile + package the `.deb`) is one command:

```bash
cmake --workflow --preset cp0-cross-package
```

The **first** run downloads the device BSP (`sdk_bsp.tar.gz`) and LVGL, so it
takes a few minutes and needs internet. Later builds are fast.

When it finishes you'll have:

```
dist/BruceCardputerZero_0.1.0_bruce1_arm64.deb
```

---

## Step 4 — Copy it to the CardputerZero and install

### The easy way (automated)

From the repo folder, fill in your device user and IP:

```bash
REMOTE_USER=pi REMOTE_HOST=192.168.1.50 ./deploy.sh
```

This copies the `.deb` to the device's home folder. Then SSH in and install:

```bash
ssh pi@192.168.1.50
sudo apt install ./BruceCardputerZero_0.1.0_bruce1_arm64.deb
```

### The manual way

```bash
scp dist/BruceCardputerZero_0.1.0_bruce1_arm64.deb pi@192.168.1.50:~
ssh pi@192.168.1.50
sudo apt install ./BruceCardputerZero_0.1.0_bruce1_arm64.deb
```

> Only the shared libraries are hard dependencies, so the package **always
> installs cleanly** — a missing CLI tool never blocks it. The tools the menu
> drives (curl, netcat-openbsd, smbclient, iw, wireless-tools, bluez, rfkill,
> iproute2, nmap, i2c-tools, gpiod, flashrom, **rtl-sdr, rtl-433** for Sub-GHz,
> **libnfc-bin/libnfc-examples** for PN532, **gpsd/gpsd-clients** for wardrive,
> qrencode) are **Recommends**: `apt install ./pkg.deb` pulls them in
> automatically, so every menu feature works with zero manual `apt` (use
> `--no-install-recommends` to skip them; note `dpkg -i` installs the app but
> does **not** pull Recommends — always use `apt install ./…`). Heavier /
> offensive tooling (aircrack-ng, mdk4, hostapd, arp-scan, dnsmasq) and
> `responder` are **Suggests** — install per feature, e.g.
> `sudo apt install aircrack-ng mdk4`.
> The RFID menu's **PN532** entries use libnfc (auto-installed above); point it
> at your reader in `/etc/nfc/libnfc.conf`
> (e.g. `device.connstring = "pn532_i2c:/dev/i2c-1"`). The ST25R3916 / CC1101
> entries use the M5 CAP directly and need no extra packages, but the **SPI
> interface must be enabled once**: `sudo raspi-config nonint do_spi 0`, then
> reboot. The app powers the cap up itself (M5IOE1 HAT rail + `POWER_EN`) and
> drives the cap's chip-select lines; if detection still fails, the tool log
> lists exactly which power/SPI step is missing.

---

## Step 5 — Run it

The package installs an **APPLaunch** launcher entry named
**"BruceCardputerZero"** — start it from the device's app menu.

To launch straight from a terminal instead:

```bash
bruce_cardputerzero
```

Optional — start automatically on boot (a systemd service ships in the
package):

```bash
sudo systemctl enable --now bruce_cardputerzero
```

---

## Using the app

Bruce opens on the **Main Menu**. Navigation uses the keyboard:

| Key | Action |
| --- | --- |
| `5` / `7` | Move selection **Up** / **Down** |
| `6` | **Select** (open a category or run a tool) |
| `4` or `ESC` | **Back** (or **Quit** on the main menu) |
| `8` | Toggle **Light / Dark** theme |

Inside a tool screen: `5` / `7` = scroll the log **up / down** (scrolling up
pauses tail-follow; scrolling back to the bottom resumes it), `6` =
**Run/Stop**, `8` = **Clear** log, `4` or `ESC` = **Back**.
The tool's live output streams into the log area.

Menu contents: **Wi-Fi** (including **Evil Portal** — AP + DNS-hijacked captive
portal capturing credentials to `~/.bruce/BruceEvilCreds/`, a Linux port of the
ESP32 Bruce feature; needs root and `sudo apt install hostapd dnsmasq`), plus
**Bluetooth LE**, **RFID**, **Sub-GHz/RF**, **Network Tools**, **Nmap**,
**SMB Client**, **Responder**, hardware buses, **Terminal**, **Scripts**,
**Theme** and **About**. Add or edit tools by editing one file —
`src/model/tool_catalog.cpp` — then rebuild.

**Terminal** (the "execute root commands" entry point): pick **Terminal (user)**
or **Root terminal (sudo)**, press `8` (Type), and type a command — Enter runs
it, Up/Down recall history, ESC leaves the input, `4`/ESC goes back. Every
command runs in a fresh shell (`sh -c`), so `cd` does not persist between runs.
A leading `sudo` runs it as root: with the sudoers rule installed that is
passwordless; without it the app asks for the sudo password on-device once and
caches it for the terminal session (a rejected password is dropped and asked
again next time). Example: `sudo iw dev wlan0 scan | grep SSID`.

---

## Troubleshooting

**"CM0 SDK sysroot not found" during Step 3** — the BSP download was
interrupted. Delete `.cache/sdk_bsp-src` and re-run Step 3 with a stable
connection.

**The screen looks cut off / wrong size** — the app assumes a **320×170**
panel. If yours differs, edit `kScreenWidth` / `kScreenHeight` in
`src/view/theme.h` and rebuild.

**The keyboard keys do nothing** — nav keys are mapped to `4`–`8` and `ESC`.
If your CardputerZero uses different key codes, adjust the mapping in
`src/platform/linux_input.cpp` (`nav_key_to_index` / `map_evdev_key`).

**A tool prints "exec failed: … No such file"** — that tool's binary isn't
installed on the device. Install it with `apt` (e.g. `sudo apt install iw`).

**A tool needs root** (evil portal, RFID/CC1101/NFC, BLE, wardrive, Responder,
raw sockets, the Wi-Fi scans) — the APPLaunch launcher starts the app
**non-root**, so every root-needing tool self-escalates with `sudo -n`: native
re-execs run the binary directly, shell-based rows go through its internal
`--root-sh` passthrough. One `/etc/sudoers.d` rule scoped to exactly this
binary covers both — and the app's own **"Install optional tools (apt)"** entry
rides the same path, so it installs packages with no password prompt.

**This is now automatic.** The package's `postinst` writes the scoped
`/etc/sudoers.d/bruce-cardputerzero` rule (validated with `visudo`) during
`apt install ./pkg.deb`, and `postrm` removes it on uninstall — no manual step.
Build with `-DAPP_INSTALL_SUDOERS=OFF` to omit it (e.g. a strict APPLaunch store
submission that forbids maintainer scripts); `./deploy.sh` still installs the
rule over SSH, or add it by hand:

```bash
sudo install -m 0440 -o root -g root \
  config/bruce-cardputerzero.sudoers /etc/sudoers.d/bruce-cardputerzero
sudo visudo -cf /etc/sudoers.d/bruce-cardputerzero   # verify
```

Without the rule, those tools no longer fail silently: the app asks for the
sudo password on-device (a masked input on the Cardputer keyboard — the image
default is often `pi`) and runs the tool via `sudo -S`, one attempt per run.
The rule is still the zero-friction path: with it, root tools start without
any prompt. Alternatively, launch the whole app from a root shell
(`sudo bruce_cardputerzero`).
(The rule must match the binary's installed path, so it only works for the
packaged `/usr/share/APPLaunch/bin/bruce_cardputerzero`; a dev build run from
the build tree always takes the password-prompt path.)

---

## Alternative: build on the device itself (advanced)

If you'd rather not use a helper computer, you can compile directly on the
CardputerZero (slower). Install the dev libraries and point the build at the
running system as its own sysroot:

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config \
  libpng-dev libjpeg-dev libfmt-dev libfreetype-dev zlib1g-dev
cmake --preset cp0-native -DCM0_SDK_ROOT=/ -DCMAKE_SYSROOT=/
cmake --build --preset cp0-native-rel
./build/cp0-native/Release/bruce_cardputerzero
```

> This path is untested by us; the cross-build in Steps 1–5 is the recommended
> route.
