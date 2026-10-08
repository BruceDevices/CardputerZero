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
> iproute2, nmap, i2c-tools, gpiod, flashrom) are **Recommends**: `apt install`
> pulls them in automatically (use `--no-install-recommends` to skip them, or
> `dpkg -i` — either still installs the app). Heavier attack tooling
> (aircrack-ng, mdk4, hostapd, arp-scan, dnsmasq, rtl-433) and `responder` are
> **Suggests** — install per feature, e.g. `sudo apt install aircrack-ng mdk4`.
> The RFID menu's **PN532** entries use libnfc: `sudo apt install libnfc-bin
> libnfc-examples`, then point it at your reader in `/etc/nfc/libnfc.conf`
> (e.g. `device.connstring = "pn532_i2c:/dev/i2c-1"`). The ST25R3916 entries use
> the M5 CAP directly and need no extra packages.

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

Inside a tool screen: `6` = **Run/Stop**, `7` = **Clear** log, `4` = **Back**.
The tool's live output streams into the log area.

Menu contents: **Wi-Fi**, **Bluetooth LE**, **Sub-GHz/RF**, **Network Tools**,
**SMB Client**, **Responder**, **About**. Add or edit tools by editing one
file — `src/model/tool_catalog.cpp` — then rebuild.

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

**A tool needs root** (Responder, raw sockets, rfkill) — run the app from a
root shell or configure the appropriate capabilities/sudoers on the device.

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
