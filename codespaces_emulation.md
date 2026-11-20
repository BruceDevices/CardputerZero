# Run the desktop app in Codespaces over VNC

These steps install a lightweight Xfce desktop with TigerVNC/noVNC, then run
the **C++/LVGL desktop build** in it. GitHub Codespaces does not provide this
graphical desktop by default. This is different from the Python/pygame
simulator in `sim/`.

## 1. Open a terminal in the Codespace

Run the following from the repository root:

```bash
cd /workspaces/CardputerZero
```

## 2. Install the desktop, VNC, and build dependencies

On a Debian/Ubuntu Codespace, install the packages:

```bash
sudo apt update
sudo apt install -y \
  xfce4 \
  tigervnc-standalone-server \
  novnc \
  websockify \
  build-essential \
  cmake \
  ninja-build \
  libpng-dev \
  libjpeg-dev \
  libfmt-dev \
  libsdl2-dev \
  libfreetype-dev \
  zlib1g-dev \
  wget
```

This project requires CMake 3.31 or newer. Install the repository's pinned
CMake version (3.31.12):

```bash
bash .devcontainer/install-cmake.sh
```

## 3. Configure and build

Configure the Linux desktop preset and build the Debug executable:

```bash
cmake --preset linux-x86-64
cmake --build --preset linux-x86-64-dbg
```

The executable is `build/linux-x86-64/Debug/bruce_cardputerzero`.

## 4. Configure and start the VNC desktop

Create the TigerVNC startup script so the VNC session launches Xfce, then set
a VNC password:

```bash
mkdir -p ~/.vnc
printf '#!/bin/sh\nunset SESSION_MANAGER\nunset DBUS_SESSION_BUS_ADDRESS\nexec startxfce4\n' > ~/.vnc/xstartup
chmod +x ~/.vnc/xstartup
vncpasswd
```

Start the Xfce desktop on display `:1`:

```bash
vncserver :1 -localhost yes -geometry 1280x720 -depth 24
```

In a **second terminal**, start the noVNC web client and leave the command
running:

```bash
websockify --web=/usr/share/novnc/ 6080 localhost:5901
```

Check that the web page is being served:

```bash
curl -I http://127.0.0.1:6080/vnc.html
```

The response should include `HTTP/1.1 200 OK`.

## 5. Open noVNC and start the app

In VS Code, open the **Ports** tab, make port **6080** visible/forwarded, and
open it in a browser. Use the noVNC client page:

```text
/vnc.html?autoconnect=true&resize=scale
```

You can print the Codespaces URL with:

```bash
echo "https://${CODESPACE_NAME}-6080.app.github.dev/vnc.html?autoconnect=true&resize=scale"
```

If the browser shows a file listing, add `/vnc.html` to the forwarded port
URL. Connect and enter the password set with `vncpasswd`. Port `5901` is the
underlying VNC connection; use the web client on port `6080`.

In another terminal, launch the app on the VNC display and leave it running:

```bash
export DISPLAY=:1
./build/linux-x86-64/Debug/bruce_cardputerzero
```

The app window should appear in the noVNC browser page. Close the app window
to stop it.

If `vncserver` reports that display `:1` is already active, the VNC desktop is
already running; don't start a second server. The app must use the same
display number as the VNC session (`DISPLAY=:1` here).

To stop the desktop when you're done, press `Ctrl+C` in the terminal running
`websockify`, then run:

```bash
vncserver -kill :1
```

## Controls

Use the on-screen navigation buttons or keyboard:

| Key | Action |
| --- | --- |
| `Esc` | Back or quit |
| `F`, `Z`, `↑`, `←` | Previous / up |
| `Enter` | Select / run or stop |
| `X`, `C`, `↓`, `→` | Next / down |
| `T` | Toggle light/dark theme |
