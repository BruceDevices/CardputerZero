# Bruce for CardputerZero

A **Bruce-style** security toolkit UI for the [CardputerZero](https://github.com/BruceDevices/CardputerZero) —
a purple/black [LVGL](https://lvgl.io/) menu that launches Wi-Fi, Bluetooth,
RF, network and SMB tooling from the device screen. Built on the M5Stack
CardputerZero LVGL template.

> **👉 Just want to install it? See [Install the beta build](#install-the-beta-build)
> below, or [INSTALL.md](INSTALL.md) to build it yourself.** The other sections
> are for developers.

## Install the beta build

Every push to `main` is built by CI and published to the rolling
[**beta** release](https://github.com/BruceDevices/CardputerZero/releases/tag/beta),
always under the same file name, so these links always give you the latest
build. Run the commands **on the CardputerZero** (it needs internet access).

**Option 1 — one-line installer:**

```bash
curl -fsSL https://github.com/BruceDevices/CardputerZero/releases/download/beta/install.sh | bash
```

Prefer to read the script first? Download it, check it, then run it:

```bash
curl -fsSLO https://github.com/BruceDevices/CardputerZero/releases/download/beta/install.sh
less install.sh
bash install.sh
```

**Option 2 — download and install the `.deb` by hand:**

```bash
curl -fLO https://github.com/BruceDevices/CardputerZero/releases/download/beta/bruce-cardputerzero_beta_arm64.deb
sudo apt update
sudo apt install --reinstall ./bruce-cardputerzero_beta_arm64.deb
```

Use `apt install ./…`, not `dpkg -i`: apt also installs the tools the menus run
(curl, iw, bluez, nmap, hostapd, …). To update, run the same commands again —
`--reinstall` is needed because every beta build has the same version number,
so without it apt would skip the update.
Then start **BruceCardputerZero** from APPLaunch, or run `bruce_cardputerzero`.
See [INSTALL.md](INSTALL.md#step-5--run-it) for autostart, keys and
troubleshooting.

> The beta build is untested: it is whatever `main` was at the last push.

> ⚠️ **For authorized security testing, CTF and research only.** Several
> menu entries (Responder, port scans, packet capture) are only legal against
> systems you own or have written permission to test.

## How it works

Rather than reimplementing each protocol, the app **shells out to the real
Linux binaries** already on the device (`curl`, `nc`, `iw`, `bluetoothctl`,
`smbclient`, `responder`, `rtl_433`, …) and streams their output to the screen.
That keeps the port tiny and every tool behaves exactly like its CLI.

- **Tool catalog** — `src/model/tool_catalog.cpp` is a plain data list of
  categories and actions. **Add a tool by editing this one file**; no screen
  code changes needed. Each action is an `argv` run with `execvp` (no shell
  string built from user input → no injection surface).
- **Process runner** — `src/platform/process_runner.*` forks/execs a tool,
  captures stdout+stderr on a reader thread, and delivers it to the LVGL thread
  via `lv_async_call`.
- **Screens** — one reusable `MenuScreen` drives both the category menu and
  each tool list; `ToolRunScreen` runs a tool and shows its live log;
  `TerminalScreen` is the interactive shell behind the "Terminal" category;
  `AboutScreen` is static. `ScreenManager` swaps screens from the current-page
  subject.
- **Theme** — purple/black palette in `src/view/ui_const.h`; light/dark toggle
  in the nav bar.
- **Terminal** — a full-screen interactive shell (no title bar, so the log
  gets the whole panel). Press **OK** or **Type** to open the command line;
  Enter runs it, Up/Down recall history while typing, `ESC` leaves the input
  (press it again to return to the menu). A leading `sudo ` runs the command as
  root: passwordless through the installed sudoers rule, otherwise the app asks
  for the sudo password on-device once and caches it for the terminal session.
  The same screen serves the user and root rows; the tool catalog stays the
  only place tools are defined.
- **Status bar** — the title bar shows Wi-Fi strength and battery
  (charge state included) read from `/sys`, refreshed every 5 s
  (`src/platform/device_status.*`, ported from the official M5Stack template).
- **Screenshot** — pressing the PrintScreen key dumps the active screen to a
  PNG (`src/platform/screenshot_service.*`, LVGL snapshot + libpng) and
  confirms with a toast (`src/view/widgets/toast.*`).

## Architecture (MVVM around LVGL)

- **UI layer**: `BaseScreen` owns the root, title bar, nav bar, and content.
  Widgets derive from `BaseWidgets`. The 5-slot `NavBar` is generic — each
  screen configures its own buttons.
- **Data model**: `BaseModel` holds current page, theme mode, and menu selection.
- **View model**: `BaseViewModel` exposes state as LVGL observer subjects and
  provides navigation actions (open menu, run tool, back, quit, toggle theme).
- **Reactive/data binding**: `src/reactive` wraps LVGL observer subjects.
- **Platform layer**: Linux input + the process runner, plus native subcommands
  the app re-execs itself for (`--ble-spam`, `--wardrive`, `--evil-portal`, …)
  and the `--root-sh` passthrough that lets the sudoers rule carry root shell
  rows (catalog scripts and terminal commands alike) without ever authorizing
  `/bin/sh` itself.
  The nav bar maps hardware/keyboard keys `4`–`8`, plus `ESC` for back/quit.

## Repository Layout

```text
.
├── assets/                 # Runtime assets used by the app
│   ├── applications/       # Desktop/app launcher metadata
│   ├── audio/              # Audio resources
│   ├── fonts/              # TTF fonts loaded through FreeType
│   └── images/             # Image resources
├── screenshot/             # Simulator screenshots for supported desktop platforms
├── src/
│   ├── app/                # Application lifecycle, asset loading, screen management
│   ├── config/             # LVGL config headers for desktop and device builds
│   ├── logger/             # Project logging wrapper
│   ├── model/              # Base application data model
│   ├── platform/           # Platform input/hardware integration
│   ├── reactive/           # LVGL observer subjects and binding helpers
│   ├── view/               # Theme, UI constants, screens, and widgets
│   │   ├── screens/        # Page-level LVGL screens
│   │   └── widgets/        # Reusable LVGL widgets such as nav bar and icon button
│   ├── viewmodel/          # BaseViewModel and UI-facing actions/state subjects
│   └── main.cpp            # Program entry point
├── CMakeLists.txt          # Build graph, dependencies, options, targets
├── CMakePresets.json       # Desktop/device configure and build presets
└── README.md
```

### Modules

- `src/app/asset_manager.*`: resolves runtime assets from the optional CMake asset root, the source tree `assets/`, and the installed `/usr/share/APPLaunch/share` (plus `/usr/share/<APP_NAME>/`) paths.
- `src/app/screen_namager.cpp`: switches LVGL screens when the current page subject changes.
- `src/config/lv_conf_desktop.h`: desktop SDL simulator LVGL settings.
- `src/config/lv_conf_cm0.h`: embedded Linux/CardputerZero LVGL settings.
- `src/platform/linux_input.*`: Linux evdev keypad support and desktop SDL keyboard routing for nav buttons.
- `src/reactive/subjects.*`: typed wrappers around LVGL subjects.
- `src/reactive/bindings.*`: helper bindings for labels, theme, widget values, flags, states, and events.
- `src/view/widgets/navbar.*`: bottom navigation bar and icon-button action mapping.
- `src/view/widgets/icon_button.*`: transparent icon button with font/color/theme support.

## Build Options

Key CMake cache options — the presets in `CMakePresets.json` already set sensible
values per platform, so you rarely set these by hand:

| Option | Description |
| --- | --- |
| `USE_DESKTOP` | Build the SDL desktop simulator (`ON`) or the embedded Linux target (`OFF`). |
| `APP_NAME` | Application name used for installed asset lookup. |
| `APP_ASSETS_ROOT` | Optional runtime asset root (expects `fonts/`, `images/`, …). |
| `APP_KEY_INPUT_DEVICE` | Linux evdev device path; empty auto-scans `/dev/input/event*`. |
| `APP_FRAMEBUFFER_DEVICE` | Linux framebuffer device for embedded fbdev builds. |
| `APP_USE_DRM` | Use LVGL's DRM/KMS backend instead of fbdev for embedded builds. |
| `APP_DRM_DEVICE` | DRM device path when `APP_USE_DRM=ON`. |
| `APP_DRM_CONNECTOR_ID` | DRM connector id when `APP_USE_DRM=ON` (`-1` auto-selects). |

Asset lookup order:

1. `APP_ASSETS_ROOT` when provided by CMake
2. source-tree `assets/` for development
3. `/usr/share/APPLaunch/share` (and `/usr/share/<APP_NAME>/`) for installed deployments

## Desktop Builds

Desktop builds are intended for fast UI development and use SDL as the LVGL display/input backend.

### Simulator Preview

| macOS simulator | Debian simulator | Windows simulator |
| --- | --- | --- |
| ![macOS simulator screenshot](screenshot/darwin_screenshot.png) | ![Debian simulator screenshot](screenshot/linux_screenshot.png) | ![Windows simulator screenshot](screenshot/win_screenshot.png) |

Dependencies (exact versions are pinned in `CMakeLists.txt` for LVGL and in
`vcpkg.json` / `vcpkg-configuration.json` for Windows builds):

| Dependency | Source | Notes |
| --- | --- | --- |
| LVGL | `CMakeLists.txt` FetchContent | Main GUI framework. |
| fmt | system package / `vcpkg.json` | Logging and formatted strings. |
| libpng | system package / `vcpkg.json` | PNG decoding for LVGL. |
| libjpeg-turbo | system package / `vcpkg.json` | JPEG decoding for LVGL. |
| zlib | system package / `vcpkg.json` | Compression used by the image libraries. |
| SDL2 | system package / `vcpkg.json` | Desktop simulator display/input backend. |
| FreeType | system package / `vcpkg.json` | Runtime TTF font rendering. |

### Linux Desktop

Debian/Ubuntu dependencies:

```shell
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  libpng-dev \
  libjpeg-dev \
  libfmt-dev \
  libsdl2-dev \
  libfreetype-dev \
  zlib1g-dev
```

> [!NOTE]
> Minimal CMake version is 3.31.0 (in coordinate with Debian 13 trixie), for Ubuntu user you can install latest cmake via `snap`
> and run following cmake command with `/snap/bin/cmake`.


Configure:

```shell
cmake --preset linux-x86-64
```

Build:

```shell
cmake --build --preset linux-x86-64-dbg
# alternatively, you can run release build
# cmake --build --preset linux-x86-64-rel
```

Run:

```shell
./build/linux-x86-64/Debug/bruce_cardputerzero
# or launch release build
# ./build/linux-x86-64/Release/bruce_cardputerzero
```

### macOS Desktop

Install the Apple command-line tools:

```shell
xcode-select --install
```

Install dependencies with Homebrew:

```shell
brew install \
  cmake \
  ninja \
  libpng \
  jpeg \
  fmt \
  sdl2 \
  freetype \
  dpkg \
  zlib
```

Configure for Apple Silicon:

```shell
cmake --preset darwin-arm64
```

Build:

```shell
cmake --build --preset darwin-arm64-dbg
# alternatively, you can run release build
# cmake --build --preset darwin-arm64-rel
```

Run:

```shell
./build/darwin-arm64/Debug/bruce_cardputerzero
# or launch release build
# ./build/darwin-arm64/Release/bruce_cardputerzero
```

For Intel macOS, use the `darwin-x86-64` configure preset and matching build preset:

```shell
cmake --preset darwin-x86-64
cmake --build --preset darwin-x86-64-dbg
# alternatively, you can run release build
# cmake --build --preset darwin-x86-64-rel
./build/darwin-x86-64/Debug/bruce_cardputerzero
# or launch release build
# ./build/darwin-x86-64/Release/bruce_cardputerzero
```

### Windows Desktop

Install CMake and Ninja:

```powershell
winget install Kitware.CMake
winget install Ninja-build.Ninja
```

Install a C++ toolchain. Choose one of the following.

MSVC Build Tools:

```powershell
winget install Microsoft.VisualStudio.BuildTools
```

In the Visual Studio installer, enable **Desktop development with C++** and make sure MSVC and a Windows SDK are selected.

MinGW-w64:

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
```
Alternatively, you can download from [`winlibs`](https://winlibs.com/). 

Install vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

Configure environmental variables for current terminal:

```
$env:VCPKG_ROOT="C:\vcpkg"
$env:PATH="$env:VCPKG_ROOT;$env:PATH"
```

> [!TIP]
> Change `VCPKG_ROOT` to your vcpkg installation directory if it is located elsewhere.
>
> These environment variables are **session-scoped**, meaning they only apply to the current terminal session.
> Once the terminal is closed, the variables will be lost and need to be set again in a new session.
> If you want to a persist environment variable
> ```powershell
> setx VCPKG_ROOT "C:\vcpkg"
> ```


Configure and build with MSVC:

> [!IMPORTANT]
> These steps require launching CMake from a **Developer Command Prompt** or **Developer PowerShell** provided by Visual Studio.
>
> This ensures that the MSVC environment (compiler, linker, and Windows SDK paths) is properly initialized.
>
> Without this environment setup, CMake may fail to detect or correctly configure the MSVC toolchain.

```powershell
cmake --preset win32-msvc
cmake --build --preset win32-msvc-dbg
.\build\msvc\Debug\bruce_cardputerzero.exe

# alternatively for release build
# cmake --build --preset win32-msvc-rel
# .\build\msvc\Release\bruce_cardputerzero.exe
```

Configure and build with MinGW-w64:

> [!IMPORTANT]
> These steps require a properly configured MinGW-w64 toolchain in your system PATH.
>
> Please ensure the following compilers are available in the current terminal session:
> - gcc
> - g++
> - ld
> - ar
>
> You can verify the setup by running:
> ```powershell
> gcc --version
> g++ --version
> ```
>
> If these commands are not recognized, you must add MinGW-w64 `bin` directory to your PATH before configuring CMake.

```powershell
cmake --preset win32-mingw64
cmake --build --preset win32-mingw64-dbg
.\build\mingw64\Debug\bruce_cardputerzero.exe

# alternatively for release build
# cmake --build --preset win32-mingw64-rel
#.\build\mingw64\Release\bruce_cardputerzero.exe
```
> [!NOTE]
> `VCPKG` will handle the dependencies during CMake configuration process automatically, 
> this may take several minutes depend on you network connection.

## CardputerZero Cross Build

This preset builds an aarch64 Linux target from a host machine with an `aarch64-linux-gnu-gcc/g++` toolchain.
The BSP at `.cache/sdk_bsp-src` is treated as a sysroot: headers, libraries, startup objects, and pkg-config metadata are resolved from that directory first.
If that directory does not exist on the first configure, the toolchain downloads and extracts `sdk_bsp.tar.gz` automatically before CMake runs compiler checks.

Install a cross compile toolchain:

+ Debian

```shell
sudo apt install crossbuild-essential-arm64
```
+ MacOS

```shell
brew install aarch64-unknown-linux-gnu
```

> [!NOTE]
> On Windows, it's recommended to use WSL for cross build
> or you can download [cross compile tools](https://sysprogs.com/getfile/2542/raspberry64-gcc14.2.0.exe) and configure the `PATH` yourself

Configure:

```shell
cmake --preset cp0-cross
```

Build:

```shell
cmake --build --preset cp0-cross-dbg
# or release build
# cmake --build --preset cp0-cross-rel
```

Deploy the release package to your device after filling in your device user and IP:

```shell
REMOTE_USER=<user> REMOTE_HOST=<device-ip> ./deploy.sh
```

By default, the debian package is copied to '$HOME' folder, normally it's under '/home/\<user\>'.

On your device, install the copied package with `apt` and replace the package file name with the one you copied:

```shell
sudo apt install ./BruceCardputerZero_*_arm64.deb   # plain apt pulls Recommends (hostapd, dnsmasq, iw, ...)
```


## Development Guide

### Navigation Keys

The bottom nav bar maps hardware/keyboard keys from left to right:

| Key | Nav item |
| --- | --- |
| `4` | First icon button |
| `5` | Second icon button |
| `6` | Third icon button |
| `7` | Fourth icon button |
| `8` | Fifth icon button |
| `ESC` | First icon button / quit |

Per-screen slot meanings: on `ToolRunScreen` the slots are Back / Up / Run-Stop
/ Down / Clear; on `TerminalScreen` they are Back / Up / Run-Stop / Down / Type.
Pressing **OK** (Run) or **Type** while idle opens the command input; while
typing, every key feeds the input, Enter runs the command, Up/Down recall
history and `ESC` returns to nav mode (a leading `sudo ` runs it as root).

### Adding a Screen

1. Add a new page enum value in `src/model/base_model.h`.
2. Add the page transition/state logic in `BaseModel` and `BaseViewModel`.
3. Create a screen under `src/view/screens/` deriving from `BaseScreen`.
4. Extend `ScreenManager` to load the new screen when the page subject changes.
5. Update `NavBar` icons and callbacks as needed.

### Adding a Widget

1. Derive from `BaseWidgets`.
2. Implement `build()` and create LVGL objects under `parent_`.
3. Use `reactive::bind_*` helpers for text, style, state, or event bindings.
4. Keep LVGL observer lifetimes object-bound whenever possible with `lv_subject_add_observer_obj` or `reactive::observe_obj`.

### Fonts and Assets

Fonts are loaded at runtime through FreeType. Place development fonts in:

```text
assets/fonts/
```

Installed deployments can place fonts in:

```text
/usr/share/APPLaunch/share/fonts/
```

You can also provide an asset root at configure time:

```shell
cmake --preset darwin-arm64 -DAPP_ASSETS_ROOT=/path/to/assets
```


## Debian Package

CPack produces the `.deb` in `dist/`, named like
`BruceCardputerZero_<version>-bruce1_arm64.deb`. Everything installs under the
`/usr/share/APPLaunch/` prefix the app store requires — there is **no**
`/usr/bin` entry and **no** systemd service:

| Path (under `/usr/share/APPLaunch/`) | Content |
| --- | --- |
| `bin/bruce_cardputerzero` | Application executable. |
| `share/fonts/`, `share/images/`, `share/audio/` | Runtime assets. |
| `applications/bruce_cardputerzero.desktop` | APPLaunch launcher entry (icon `share/images/bruce_icon.png`). |
| `share/doc/bruce_cardputerzero/` | README + third-party asset license. |

Build and package:

```shell
cmake --preset cp0-cross
cmake --build --preset cp0-cross-rel
cpack --preset cp0-cross-deb
```

Or run the full configure, release build, and package flow with one workflow preset:

```shell
cmake --workflow --preset cp0-cross-package
```

> [!NOTE]
> Use Debian based Linux host with CPack generator 
> as other OS may not resolve the dependencies correctly
> no full dpkg, dpkg-shlibdeps, objdump and readelf support

# Contributing

If you have better framework designs on Embedded Linux or build system practice, feel free to let us know.

# License

Project released under MIT license. More license information can be found in assets folder. 
