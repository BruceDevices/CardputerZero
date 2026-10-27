"""Tool catalog — the Python port of src/model/tool_catalog.cpp.

Same contract as the C++: a plain data list of categories and actions. Add a
tool by editing this one file; no screen code changes needed.

Each ToolAction keeps the real `argv` (identical to the C++ catalog, so the
real device could still shell out to it) AND a `sim` script of output lines the
*simulator* streams instead. The pygame build has no radios, no wlan0, no CAP,
so it never runs the real argv — it plays the canned `sim` lines. Offensive /
credential-capturing rows (Evil Portal, deauth, beacon/BLE spam, Responder, MAC
flood, DHCP starvation, wardrive) are simulated as STATUS-ONLY stubs that
perform nothing; they exist so the menu matches the device, not to carry out an
attack on a desktop.

Icons are the real Phosphor-Fill codepoints from ui_const.h, rendered through
the bundled assets/fonts/Phosphor-Fill.ttf.
"""

import theme

# ── Phosphor-Fill glyph codepoints (ui_const.h :: icon::) ──────────────────
IC = {
    "wifi": "\ue4ea", "bluetooth": "\ue0da", "rf": "\ue0f2", "nfc": "\uebb6",
    "scan": "\uebe0", "ir": "\ue754", "gps": "\ue316", "eth": "\ueb5a",
    "files": "\ue260", "clock": "\ue19a", "palette": "\ue6c8", "network": "\uedde",
    "storage": "\ue2a0", "shield": "\ue412", "info": "\ue2ce", "i2c": "\ue9c2",
    "spi": "\ue9c4", "uart": "\ueae8", "onewire": "\ue27a", "gpio": "\ue610",
}


class Item:
    """One row in an interactive options menu — Bruce's loopOptions entry.
    With `children` it's a sub-menu (Select drills in); otherwise `lines` is
    the detail text shown when you open it."""

    def __init__(self, label, lines=None, children=None):
        self.label = label
        self.lines = lines or []
        self.children = children or []


class Action:
    def __init__(self, label, desc, argv, sim, needs_root=False, nav=False,
                 items=None, view=None):
        self.label = label
        self.desc = desc
        self.argv = argv          # real command (unused in sim unless --real)
        self.sim = sim            # list[str]: lines the simulator streams
        self.needs_root = needs_root
        self.nav = nav            # navigation row (no process), e.g. accents
        # When set, selecting the action opens an interactive options menu
        # (loopOptions) of these items instead of streaming the sim log.
        self.items = items or []
        # A dedicated full-screen widget to show instead of the tool log,
        # e.g. "clock" draws a themed clock face. None = normal run log.
        self.view = view


class Category:
    def __init__(self, label, desc, icon, actions=(), info_only=False):
        self.label = label
        self.desc = desc
        self.icon = icon
        self.actions = list(actions)
        self.info_only = info_only


def _ok(*lines):
    return list(lines) + ["", "[process exited 0]"]


def _stub(*lines):
    """Status-only sim for an offensive/capture row: nothing is performed."""
    return ["[sim] simulation only — nothing is transmitted, served or captured",
            *lines, "", "[process exited 0]"]


def _missing(tool, apt):
    """A plain tool row the desktop may actually have; sim shows a 'not here' note."""
    return _ok(f"{tool}: needs the real device / a {apt}", f"(install hint: {apt})")


_SCAN = _ok(
    "BSS 3c:84:6a:aa:bb:cc (on wlan0)", "    SSID: HomeNet-5G",
    "    signal: -47.00 dBm   freq: 5180",
    "BSS 9c:53:22:11:00:ff (on wlan0)", "    SSID: CoffeeShop",
    "    signal: -71.00 dBm   freq: 2437",
    "BSS 00:11:22:33:44:55 (on wlan0)", "    SSID: TP-Link_Guest",
    "    signal: -80.00 dBm   freq: 2462",
)

CATALOG = [
    # ── Wi-Fi ───────────────────────────────────────────────────────────────
    Category("Wi-Fi", "Scan, recon, 802.11 attacks and Evil Portal (wlan0; root needed)", IC["wifi"], [
        Action("Scan nearby networks", "iw dev wlan0 scan (needs root)",
               ["sh", "-c", "ip link set wlan0 up; iw dev wlan0 scan"], _SCAN, needs_root=True),
        Action("List SSIDs + signal", "iw scan | ssid/signal/freq",
               ["sh", "-c", "iw dev wlan0 scan | grep -E 'SSID:|signal:'"],
               _ok("SSID: HomeNet-5G   signal: -47 dBm", "SSID: CoffeeShop   signal: -71 dBm",
                   "SSID: TP-Link_Guest   signal: -80 dBm"), needs_root=True),
        Action("Channel usage (APs/chan)", "scan grouped by channel",
               ["sh", "-c", "iw dev wlan0 scan | ..."],
               _ok("  2437 MHz: 4 APs", "  2462 MHz: 2 APs", "  5180 MHz: 3 APs"), needs_root=True),
        Action("Show Wi-Fi status", "ip addr show wlan0", ["ip", "addr", "show", "wlan0"],
               _ok("3: wlan0: <BROADCAST,MULTICAST,UP> mtu 1500",
                   "    inet 192.168.1.42/24 brd 192.168.1.255 scope global wlan0")),
        Action("Scan LAN hosts (arp-scan)", "arp-scan --localnet",
               ["sh", "-c", "arp-scan --localnet"],
               _ok("192.168.1.1    3c:84:6a:aa:bb:cc   TP-LINK",
                   "192.168.1.42   b8:27:eb:00:11:22   Raspberry Pi",
                   "2 hosts scanned."), needs_root=True),
        Action("Scan LAN hosts (nmap)", "nmap -sn <subnet>", ["sh", "-c", "nmap -sn ..."],
               _ok("Nmap scan report for 192.168.1.1", "Host is up (0.0021s latency).",
                   "Nmap scan report for 192.168.1.42", "Host is up.",
                   "Nmap done: 256 IP addresses (2 hosts up)")),
        Action("Monitor mode ON (airmon-ng)", "airmon-ng; fallback: manual mon0 VIF",
               ["sh", "-c", "airmon-ng start wlan0 ..."],
               _ok("Killing interfering processes...",
                   "monitor interface: mon0 (airodump/mdk4 find it automatically)"), needs_root=True),
        Action("Monitor mode OFF", "remove monitor ifaces; restart NetworkManager",
               ["sh", "-c", "airmon-ng stop wlan0mon; ..."],
               _ok("removed mon0", "done"), needs_root=True),
        Action("Airodump scan (APs+clients)", "airodump-ng <monitor if>", ["sh", "-c", "airodump-ng mon0"],
               _ok(" CH  6 ][ Elapsed: 12 s", " BSSID              PWR  CH  ESSID",
                   " 3C:84:6A:AA:BB:CC  -47   6  HomeNet-5G",
                   " 9C:53:22:11:00:FF  -71  11  CoffeeShop")),
        Action("Capture handshakes -> /tmp", "airodump-ng -w /tmp/bruce-wifi <mon>",
               ["sh", "-c", "airodump-ng -w /tmp/bruce-wifi mon0"],
               _ok("writing to /tmp/bruce-wifi-01.cap",
                   " CH  6 ][ WPA handshake: 3C:84:6A:AA:BB:CC")),
        Action("Deauth flood (mdk4)", "mdk4 <monitor if> d — authorized testing only",
               ["sh", "-c", "mdk4 mon0 d"],
               _stub("would send 802.11 deauth frames on mon0 (needs monitor mode + authorization)")),
        Action("Beacon spam (mdk4)", "mdk4 <monitor if> b -s 100",
               ["sh", "-c", "mdk4 mon0 b -s 100"],
               _stub("would flood fake beacon frames on mon0")),
        Action("Rogue open AP (hostapd)", "open AP 'Free WiFi' + DHCP/DNS captive on wlan0",
               ["sh", "-c", "hostapd ...; dnsmasq ..."],
               _stub("would start an open AP 'Free WiFi' @ 192.168.4.1 with a captive DNS"),
               needs_root=True),
        Action("Evil Portal: Default (Google)", "captive portal, Google page",
               ["self", "--evil-portal", "default"],
               _stub("Evil Portal captures credentials on the real device.",
                     "The simulator starts no AP, serves no login page, captures nothing.",
                     "victims: 0   captured: 0"), needs_root=True),
        Action("Evil Portal: Router Update (verify pwd)", "verifies stolen pwd vs real network",
               ["self", "--evil-portal", "verify"],
               _stub("would serve a 'Router Update' page and verify submitted passwords"),
               needs_root=True),
        Action("Evil Portal: Custom HTML", "serves ~/.bruce/evil_portal/index.html",
               ["self", "--evil-portal", "custom"],
               _stub("would serve a custom captive page from ~/.bruce/evil_portal/index.html"),
               needs_root=True),
        Action("Evil Portal: Clone an AP", "scan nearby APs, pick one to clone",
               ["self", "--evil-portal", "clone"],
               _stub("would clone a scanned AP's SSID/channel for the portal"), needs_root=True),
        Action("Enable Wi-Fi radio", "rfkill unblock wifi", ["rfkill", "unblock", "wifi"],
               _ok("[sim] wifi unblocked")),
        Action("Disable Wi-Fi radio", "rfkill block wifi", ["rfkill", "block", "wifi"],
               _ok("[sim] wifi blocked")),
    ]),

    # ── Bluetooth LE ──────────────────────────────────────────────────────────
    Category("Bluetooth LE", "Discover and manage nearby BLE devices", IC["bluetooth"], [
        Action("Scan for BLE devices (15s)", "timeout 15 hcitool lescan",
               ["timeout", "15", "hcitool", "lescan"],
               _ok("LE Scan ...", "4C:87:5D:11:22:33 (unknown)",
                   "E0:9D:31:aa:bb:cc Mi Band 7", "7C:2E:0D:00:11:22 (unknown)")),
        Action("Show adapter info", "bluetoothctl show", ["bluetoothctl", "show"],
               _ok("Controller 00:1A:7D:DA:71:13", "    Name: cardputer", "    Powered: yes")),
        Action("BLE Spam: Apple pairing", "Broadcast Apple Continuity pairing popups",
               ["self", "--ble-spam", "apple"], _stub("would broadcast Apple Continuity pairing payloads")),
        Action("BLE Spam: Apple action", "Broadcast Apple NearbyAction setup modals",
               ["self", "--ble-spam", "apple-action"], _stub("would broadcast Apple NearbyAction payloads")),
        Action("BLE Spam: Android (Fast Pair)", "Broadcast Google Fast Pair popups",
               ["self", "--ble-spam", "google"], _stub("would broadcast Google Fast Pair payloads")),
        Action("BLE Spam: Samsung", "Broadcast Samsung EasySetup (Watch/Buds) popups",
               ["self", "--ble-spam", "samsung"], _stub("would broadcast Samsung EasySetup payloads")),
        Action("BLE Spam: Windows (Swift Pair)", "Broadcast Microsoft Swift Pair toasts",
               ["self", "--ble-spam", "microsoft"], _stub("would broadcast Microsoft Swift Pair payloads")),
        Action("BLE Spam: All (rotate)", "Rotate through all BLE spam families",
               ["self", "--ble-spam", "all"], _stub("would rotate through all BLE spam families")),
        Action("Enable Bluetooth radio", "rfkill unblock bluetooth", ["rfkill", "unblock", "bluetooth"],
               _ok("[sim] bluetooth unblocked")),
        Action("Disable Bluetooth radio", "rfkill block bluetooth", ["rfkill", "block", "bluetooth"],
               _ok("[sim] bluetooth blocked")),
    ]),

    # ── RFID ──────────────────────────────────────────────────────────────────
    Category("RFID", "13.56MHz NFC — M5 ST25R3916 CAP (native) or a PN532 reader (via libnfc)", IC["nfc"], [
        Action("ST25R: Read tag (UID/ATQA/SAK)", "read a 13.56MHz ISO14443A tag's UID/ATQA/SAK",
               ["self", "--rfid-read"],
               _ok("Waiting for tag ...", "UID : 04:A3:19:2B:5C:60:80", "ATQA: 0x0044   SAK: 0x00   (NTAG21x)")),
        Action("ST25R: Dump tag (Type-2 / NTAG)", "read UID then dump Type-2 data pages",
               ["self", "--rfid-dump"],
               _ok("PAGE 00: 04 A3 19 2B", "PAGE 01: 5C 60 80 00", "PAGE 02: ...", "216 bytes read")),
        Action("ST25R: Clone -> emulate tag", "read a tag's UID, then emulate it as NFC-A",
               ["self", "--rfid-clone"],
               _ok("Read UID 04:A3:19:2B:5C:60:80", "Emulating tag ... (press Back to stop)")),
        Action("ST25R: Detect chip", "probe ST25R3916, read IC_IDENTITY", ["self", "--st25r-detect"],
               _ok("ST25R3916 detected on /dev/spidev0.1", "IC rev: 0x05")),
        Action("PN532: Scan via I2C (/dev/i2c-1)", "test a PN532 on I2C bus 1 (nfc-list)",
               ["sh", "-c", "LIBNFC_DEFAULT_DEVICE=pn532_i2c:/dev/i2c-1 nfc-list -v"],
               _ok("NFC device: pn532_i2c:/dev/i2c-1 opened",
                   "ISO14443A (106 kbps) target: UID 04 A3 19 2B 5C 60 80")),
        Action("PN532: Scan via UART (/dev/ttyS0)", "test a PN532 on UART (nfc-list)",
               ["sh", "-c", "LIBNFC_DEFAULT_DEVICE=pn532_uart:/dev/ttyS0 nfc-list -v"],
               _ok("NFC device: pn532_uart:/dev/ttyS0 opened", "1 ISO14443A target found")),
        Action("PN532: Poll for tag (configured)", "nfc-poll using libnfc.conf / env",
               ["sh", "-c", "nfc-poll"],
               _ok("NFC reader: pn532 opened", "ISO14443A target: UID  04 A3 19 2B")),
        Action("PN532: Dump MIFARE Classic", "nfc-mfclassic read (default keys) -> /tmp/bruce-mfc.mfd",
               ["sh", "-c", "nfc-mfclassic r a /tmp/bruce-mfc.mfd"],
               _ok("Found MIFARE Classic 1K", "Reading sectors |................| done",
                   "Wrote 1024 bytes to /tmp/bruce-mfc.mfd")),
        Action("PN532: Dump Ultralight/NTAG", "nfc-mfultralight read -> /tmp/bruce-ul.mfd",
               ["sh", "-c", "nfc-mfultralight r /tmp/bruce-ul.mfd"],
               _ok("Found MIFARE Ultralight", "Writing data to file: /tmp/bruce-ul.mfd ... done")),
        Action("PN532: Emulate NDEF tag", "emulate a Type-2 NDEF tag", ["sh", "-c", "nfc-emulate-forum-tag2"],
               _ok("Emulating NDEF tag (NFC Forum Type 2) ... (Back to stop)")),
        Action("PN532: Emulate UID 01:02:03:04", "emulate a fixed UID", ["sh", "-c", "nfc-emulate-uid 01020304"],
               _ok("Emulating tag with UID 01:02:03:04 ... (Back to stop)")),
        Action("PN532: Setup / buses help", "how to point libnfc at your PN532; list buses",
               ["sh", "-c", "echo 'PN532 via libnfc ...'"],
               _ok("PN532 via libnfc. Set the device in /etc/nfc/libnfc.conf:",
                   "  device.connstring = \"pn532_i2c:/dev/i2c-1\"",
                   "I2C buses: /dev/i2c-1", "SPI: /dev/spidev0.0", "Serial: /dev/ttyS0")),
    ]),

    # ── Sub-GHz / RF ──────────────────────────────────────────────────────────
    Category("Sub-GHz / RF", "M5 CAP CC1101 over SPI, or an SDR dongle (e.g. RTL-SDR)", IC["rf"], [
        Action("Use CC1101 CAP (detect)", "probe CC1101 over SPI (CS1/G7)", ["self", "--cc1101-detect"],
               _ok("CC1101 PARTNUM=0x00 VERSION=0x14 — chip present on /dev/spidev0.0")),
        Action("Capture 433MHz (30s, RTL-SDR)", "timeout 30 rtl_433", ["sh", "-c", "timeout 30 rtl_433"],
               _ok("rtl_433 ...", "time : 2026-10-27  model : Nexus-TH  temperature_C : 21.3",
                   "time : 2026-10-27  model : Acurite-606TX  temperature_C : 19.8")),
        Action("List SDR devices (RTL-SDR)", "rtl_test -t", ["sh", "-c", "rtl_test -t"],
               _ok("Found 1 device(s):", "  0:  Realtek, RTL2838UHIDIR, SN: 00000001",
                   "Supported gain values (29): 0.0 0.9 1.4 ...")),
        Action("FM radio RX 100.0MHz (RTL-SDR)", "rtl_fm | aplay",
               ["sh", "-c", "rtl_fm -f 100M -M wbfm -s 200k -r 48k | aplay -r 48k -f S16_LE"],
               _ok("Tuned to 100.000 MHz.", "Sampling at 200000 S/s.", "[sim] audio to aplay (no audio in sim)")),
    ]),

    # ── Network Tools ─────────────────────────────────────────────────────────
    Category("Network Tools", "curl / ping / netcat helpers", IC["network"], [
        Action("My public IP (curl)", "curl -s http://ifconfig.me", ["curl", "-s", "http://ifconfig.me"],
               _ok("203.0.113.47")),
        Action("Fetch example.com headers (curl)", "curl -sI http://example.com",
               ["curl", "-sI", "http://example.com"],
               _ok("HTTP/1.1 200 OK", "Content-Type: text/html; charset=UTF-8", "Content-Length: 1256")),
        Action("Ping default gateway", "ping default gateway x4", ["sh", "-c", "ping -c4 $gw"],
               _ok("PING 192.168.1.1: 56 data bytes",
                   "64 bytes from 192.168.1.1: icmp_seq=0 ttl=64 time=1.9 ms",
                   "64 bytes from 192.168.1.1: icmp_seq=1 ttl=64 time=2.1 ms",
                   "4 packets transmitted, 4 received, 0% packet loss")),
        Action("Port scan default gateway (nc)", "nc -zv <gateway> 1-1024", ["sh", "-c", "nc -zv $gw 1-1024"],
               _ok("Connection to 192.168.1.1 22 port [tcp/ssh] succeeded!",
                   "Connection to 192.168.1.1 80 port [tcp/http] succeeded!",
                   "Connection to 192.168.1.1 443 port [tcp/https] succeeded!")),
        Action("Start netcat listener :4444", "nc -lvp 4444", ["nc", "-lvp", "4444"],
               _ok("listening on [any] 4444 ...")),
    ]),

    # ── Nmap (guided flow, flattened for the sim) ─────────────────────────────
    Category("Nmap", "Guided scan: discover hosts, pick one, then Fast or Full port scan", IC["scan"], [
        Action("1. Scan / refresh hosts", "ping-sweep the local subnet for live hosts",
               ["sh", "-c", "nmap -sn $net -oG -"],
               _ok("Discovering hosts on 192.168.1.0/24 ...", "192.168.1.1", "192.168.1.42",
                   "Done. Pick a host, then Fast or Full.")),
        Action("Fast scan 192.168.1.1 (top 100)", "nmap -F -T4", ["nmap", "-F", "-T4", "192.168.1.1"],
               _ok("PORT    STATE SERVICE", "22/tcp  open  ssh", "80/tcp  open  http",
                   "443/tcp open  https", "Nmap done: 1 host up")),
        Action("Full scan 192.168.1.1 (all 65535)", "nmap -p- -T4", ["nmap", "-p-", "-T4", "192.168.1.1"],
               _ok("PORT     STATE SERVICE", "22/tcp   open  ssh", "80/tcp   open  http",
                   "443/tcp  open  https", "8080/tcp open  http-proxy", "Nmap done: 1 host up")),
    ]),

    # ── SMB Client ────────────────────────────────────────────────────────────
    Category("SMB Client", "Anonymous SMB share discovery", IC["storage"], [
        Action("List shares on default gateway", "smbclient -L //<gateway> -N",
               ["sh", "-c", "smbclient -L //$gw -N"],
               _ok("    Sharename       Type      Comment", "    ---------       ----      -------",
                   "    public          Disk", "    IPC$            IPC       IPC Service")),
        Action("smbclient version", "smbclient --version", ["smbclient", "--version"], _ok("Version 4.19.5")),
    ]),

    # ── Responder ─────────────────────────────────────────────────────────────
    Category("Responder", "LLMNR/NBT-NS/mDNS poisoner — authorized use only", IC["shield"], [
        Action("Start Responder on wlan0", "responder -I wlan0", ["responder", "-I", "wlan0"],
               _stub("would poison LLMNR/NBT-NS/mDNS on wlan0 (authorized testing only)"), needs_root=True),
        Action("Start Responder on eth0", "responder -I eth0", ["responder", "-I", "eth0"],
               _stub("would poison LLMNR/NBT-NS/mDNS on eth0 (authorized testing only)"), needs_root=True),
    ]),

    # ── I2C Bus ───────────────────────────────────────────────────────────────
    Category("I2C Bus", "SDA=G2 SCL=G3 — scan and read I2C devices (i2c-tools)", IC["i2c"], [
        Action("List I2C buses", "i2cdetect -l", ["i2cdetect", "-l"],
               _ok("i2c-1    i2c    bcm2835 (i2c@7e804000)    I2C adapter")),
        Action("Scan bus 1", "i2cdetect -y 1", ["i2cdetect", "-y", "1"],
               _ok("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f",
                   "00:                         -- -- -- -- -- -- --",
                   "50: 50 -- -- -- -- -- -- -- -- -- -- -- -- -- -- --")),
        Action("Dump 0x50 (EEPROM)", "i2cdump -y 1 0x50", ["i2cdump", "-y", "1", "0x50"],
               _ok("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f",
                   "00: 00 ff 12 34 56 78 9a bc de f0 11 22 33 44 55 66")),
        Action("Read 0x50 reg 0x00", "i2cget -y 1 0x50 0x00", ["i2cget", "-y", "1", "0x50", "0x00"],
               _ok("0x00")),
    ]),

    # ── SPI Bus ───────────────────────────────────────────────────────────────
    Category("SPI Bus", "spidev0.0 — probe/read SPI flash (flashrom)", IC["spi"], [
        Action("List SPI devices", "ls /dev/spidev*", ["sh", "-c", "ls -l /dev/spidev*"],
               _ok("crw------- 1 root root 153, 0 /dev/spidev0.0",
                   "crw------- 1 root root 153, 1 /dev/spidev0.1")),
        Action("Probe SPI flash", "flashrom -p linux_spi:dev=/dev/spidev0.0",
               ["flashrom", "-p", "linux_spi:dev=/dev/spidev0.0,spispeed=2000"],
               _ok("Found Winbond flash chip \"W25Q32.V\" (4096 kB)")),
        Action("Read flash -> /tmp/spi.bin", "flashrom ... -r /tmp/spi.bin",
               ["flashrom", "-p", "linux_spi:dev=/dev/spidev0.0,spispeed=2000", "-r", "/tmp/spi.bin"],
               _ok("Reading flash... done", "Wrote 4096 kB to /tmp/spi.bin")),
    ]),

    # ── UART / Serial ─────────────────────────────────────────────────────────
    Category("UART / Serial", "TX=G14 RX=G15 — monitor a serial line (read only)", IC["uart"], [
        Action("List serial ports", "ls /dev/ttyS* ...", ["sh", "-c", "ls -l /dev/tty*"],
               _ok("/dev/ttyS0", "/dev/ttyAMA0", "/dev/ttyUSB0")),
        *[Action(f"Monitor ttyS0 @{b}", f"stty {b}; cat /dev/ttyS0",
                 ["sh", "-c", f"stty -F /dev/ttyS0 {b} raw -echo; cat /dev/ttyS0"],
                 _ok(f"[sim] monitoring /dev/ttyS0 @{b} (read-only) — no serial data on this desktop"))
          for b in (1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)],
    ]),

    # ── 1-Wire ────────────────────────────────────────────────────────────────
    Category("1-Wire", "needs w1-gpio overlay — list/read 1-Wire slaves", IC["onewire"], [
        Action("List 1-Wire devices", "ls /sys/bus/w1/devices/",
               ["sh", "-c", "ls /sys/bus/w1/devices/"],
               _ok("28-0000075c1e2a", "28-0000075c3f91", "w1_bus_master1")),
        Action("Read all slaves", "cat .../w1_slave",
               ["sh", "-c", "for d in /sys/bus/w1/devices/*/w1_slave; do ...; done"],
               _ok("== 28-0000075c1e2a ==", "a1 01 4b 46 7f ff 0c 10 b8 : crc=b8 YES",
                   "a1 01 4b 46 7f ff 0c 10 b8 t=26062")),
    ]),

    # ── GPIO / DIO ────────────────────────────────────────────────────────────
    Category("GPIO / DIO", "G7/G9/G10/G11/G22/G23/G26 — libgpiod (verify offsets)", IC["gpio"], [
        Action("Detect gpiochips", "gpiodetect", ["gpiodetect"],
               _ok("gpiochip0 [pinctrl-bcm2835] (54 lines)")),
        Action("Info gpiochip0", "gpioinfo gpiochip0", ["gpioinfo", "gpiochip0"],
               _ok("gpiochip0 - 54 lines:", "    line   7:  unnamed  input  active-high",
                   "    line  22:  unnamed  input  active-high")),
        Action("Read line 7", "gpioget gpiochip0 7", ["gpioget", "gpiochip0", "7"], _ok("0")),
        Action("Set line 7 = 1 (5s)", "gpioset --mode=time --sec=5 ... 7=1",
               ["gpioset", "--mode=time", "--sec=5", "gpiochip0", "7=1"],
               _ok("[sim] line 7 driven high for 5s")),
        Action("Monitor line 7", "gpiomon gpiochip0 7", ["gpiomon", "gpiochip0", "7"],
               _ok("event: RISING EDGE offset: 7", "event: FALLING EDGE offset: 7")),
    ]),

    # ── Infrared ──────────────────────────────────────────────────────────────
    Category("Infrared", "Send/receive IR via a gpio-ir overlay (/dev/lirc*)", IC["ir"], [
        Action("List IR devices", "ls /dev/lirc*", ["sh", "-c", "ls -l /dev/lirc*"], _ok("/dev/lirc0")),
        Action("IR receive (record 15s)", "ir-ctl -r -d /dev/lirc0",
               ["sh", "-c", "timeout 15 ir-ctl -r -d /dev/lirc0"],
               _ok("pulse 9000", "space 4500", "pulse 560", "space 560", "... (NEC frame)")),
        Action("IR send last capture", "ir-ctl -d /dev/lirc0 --send=/tmp/ir.txt",
               ["sh", "-c", "ir-ctl -d /dev/lirc0 --send=/tmp/ir.txt"], _ok("[sim] sent /tmp/ir.txt")),
        Action("TV-B-Gone (note)", "power-off code blast needs a code DB",
               ["sh", "-c", "echo 'TV-B-Gone needs a per-brand IR code set ...'"],
               _ok("TV-B-Gone needs a per-brand IR code set.",
                   "Capture codes with IR receive, save under ~/.bruce/scripts, run from Scripts.")),
    ]),

    # ── GPS ───────────────────────────────────────────────────────────────────
    Category("GPS", "Read a GPS on serial/USB via gpsd; feed wardriving", IC["gps"], [
        Action("GPS fix (gpspipe)", "gpspipe -w (gpsd)", ["sh", "-c", "timeout 20 gpspipe -w"],
               _ok('{"class":"TPV","lat":37.7749,"lon":-122.4194,"alt":15.2,"mode":3}')),
        Action("Raw NMEA (ttyUSB0 @9600)", "stty 9600; cat /dev/ttyUSB0",
               ["sh", "-c", "stty -F /dev/ttyUSB0 9600 raw -echo; timeout 15 cat /dev/ttyUSB0"],
               _ok("$GPGGA,123519,3747.49,N,12225.16,W,1,08,0.9,15.2,M,,,,*47",
                   "$GPRMC,123519,A,3747.49,N,12225.16,W,0.0,0.0,270926,,*62")),
        Action("Wardriving -> WiGLE CSV", "scan Wi-Fi + GPS fix -> WiGLE CSV (upload to wigle.net)",
               ["self", "--wardrive", "wlan0"],
               _stub("would scan Wi-Fi + read a GPS fix and append WiGLE rows to ~/.bruce/wardrive.csv")),
    ]),

    # ── Ethernet ──────────────────────────────────────────────────────────────
    Category("Ethernet", "Wired-LAN recon and L2 attacks on eth0 — authorized use only", IC["eth"], [
        Action("Scan hosts (arp-scan eth0)", "arp-scan -I eth0 --localnet",
               ["sh", "-c", "arp-scan -I eth0 --localnet"],
               _ok("10.0.0.1     3c:84:6a:aa:bb:cc   Gateway",
                   "10.0.0.15    b8:27:eb:00:11:22   Host", "2 hosts scanned."), needs_root=True),
        Action("MAC flood (macof)", "macof -i eth0", ["sh", "-c", "macof -i eth0"],
               _stub("would flood the switch CAM table on eth0 (dsniff/macof; authorized testing only)"),
               needs_root=True),
        Action("DHCP starvation (dhcpstarv)", "dhcpstarv -i eth0", ["sh", "-c", "dhcpstarv -i eth0"],
               _stub("would exhaust the DHCP pool on eth0 (authorized testing only)"), needs_root=True),
    ]),

    # ── Files ─────────────────────────────────────────────────────────────────
    Category("Files", "Browse storage and serve files over HTTP (WebUI)", IC["files"], [
        Action("List home + removable", "ls ~ /media /mnt", ["sh", "-c", "ls -la ~ /media/* /mnt"],
               _ok("== home ==", "drwxr-xr-x  .bruce", "-rw-r--r--  notes.txt",
                   "== /media ==", "(empty)")),
        Action("Disk usage", "df -h", ["df", "-h"],
               _ok("Filesystem      Size  Used Avail Use% Mounted on",
                   "/dev/mmcblk0p2   15G  4.2G   10G  31% /")),
        Action("WebUI: serve home :8080", "python3 -m http.server 8080",
               ["sh", "-c", "cd ~ && python3 -m http.server 8080"],
               _ok("Serving $HOME at http://192.168.1.42:8080 (back to stop)",
                   "Serving HTTP on 0.0.0.0 port 8080 ...")),
    ]),

    # ── System / Clock ────────────────────────────────────────────────────────
    Category("System / Clock", "Clock, timer, uptime and handy local tools", IC["clock"], [
        Action("Clock", "A full-screen themed clock — just the time and date",
               ["sh", "-c", "while true; do date; sleep 1; done"],
               _ok("2026-10-27 14:32:07", "..."), view="clock"),
        Action("Countdown timer (60s)", "sleep 60; beep", ["sh", "-c", "for i in $(seq 60 -1 1); ..."],
               _ok("60s remaining", "59s remaining", "...", "Time up!")),
        Action("Uptime + load", "uptime", ["uptime"],
               _ok(" 14:32:07 up 2 days,  3:11,  load average: 0.08, 0.12, 0.10")),
        Action("Install optional tools (apt)",
               "apt-get the Suggests set — passwordless via the package's sudoers rule",
               ["sh", "-c", "apt-get install -y aircrack-ng mdk4 hostapd arp-scan dnsmasq ..."],
               _ok("[sim] on the device this runs 'sudo -n ... --root-sh apt-get install -y ...'",
                   "[sim] no password prompt (sudoers rule installed by the .deb postinst)",
                   "Reading package lists... Done",
                   "The following NEW packages will be installed:",
                   "  aircrack-ng mdk4 hostapd arp-scan dnsmasq",
                   "Setting up aircrack-ng ... done.", "done."), needs_root=True),
        Action("QR code (this device URL)", "qrencode -t UTF8", ["sh", "-c", "qrencode -t UTF8 http://ip:8080"],
               _ok("█████████████████████", "████ ▄▄▄▄▄ █▀ █ ████", "████ █   █ █▀▀▀█ ████",
                   "(scan for http://192.168.1.42:8080)")),
        Action("Microphone record (5s)", "arecord -d 5 /tmp/rec.wav",
               ["sh", "-c", "arecord -d 5 -f cd /tmp/bruce-rec.wav"],
               _ok("[sim] recording 5s ...", "saved /tmp/bruce-rec.wav")),
        Action("iButton read (1-Wire ROM)", "cat /sys/bus/w1/devices/*/id",
               ["sh", "-c", "for d in /sys/bus/w1/devices/*/; do ...; done"],
               _ok("iButton/1-Wire: 01-000075c1e2a3")),
    ]),

    # ── Scripts ───────────────────────────────────────────────────────────────
    Category("Scripts", "Run your own executable scripts from ~/.bruce/scripts", IC["uart"], [
        Action("No scripts yet", "Add an executable script (shebang + chmod +x) to ~/.bruce/scripts",
               ["sh", "-c", "echo ..."],
               _ok("No executable scripts found in ~/.bruce/scripts",
                   "Add a script with a shebang, chmod +x, then restart Bruce.")),
    ]),
]

# ── Interactive drill-downs (Bruce loopOptions) ────────────────────────────
# Attach `items` to the natural list-producing actions so the user can pick a
# result and act on it, instead of only reading streamed text.

def _ssid_item(name, bssid, sig, ch, sec):
    detail = [f"SSID    : {name}", f"BSSID   : {bssid}", f"Signal  : {sig} dBm",
              f"Channel : {ch}", f"Security: {sec}",
              f"Vendor  : {bssid[:8].upper()} (OUI lookup)"]
    children = [
        Item("Show details", detail),
        Item("Clone to Evil Portal",
             _stub(f"would clone '{name}' (ch {ch}) into the captive portal")),
        Item("Deauth clients",
             _stub(f"would deauth clients on {bssid} (needs monitor mode + authorization)")),
        Item("Capture handshake",
             _stub(f"would capture the WPA handshake for '{name}' on ch {ch} -> /tmp")),
    ]
    sec_tag = "open" if sec.lower() == "open" else sec
    return Item(f"{name}   {sig}dBm  ch{ch}  {sec_tag}", children=children)


_SSIDS = [
    _ssid_item("HomeNet-5G", "3c:84:6a:aa:bb:cc", -47, 36, "WPA2"),
    _ssid_item("CoffeeShop", "9c:53:22:11:00:ff", -71, 6, "open"),
    _ssid_item("TP-Link_Guest", "00:11:22:33:44:55", -80, 11, "WPA2"),
    _ssid_item("eduroam", "a4:2b:8c:de:f0:12", -66, 40, "WPA2-Enterprise"),
]


def _ble_item(name, mac, rssi, kind):
    detail = [f"Name : {name}", f"MAC  : {mac}", f"RSSI : {rssi} dBm", f"Type : {kind}"]
    return Item(f"{name}   {rssi}dBm", children=[
        Item("Show details", detail),
        Item("Connect (GATT enum)", _stub(f"would connect to {mac} and enumerate GATT services")),
    ])


_BLE = [
    _ble_item("Mi Band 7", "e0:9d:31:aa:bb:cc", -58, "LE / wearable"),
    _ble_item("(unknown)", "4c:87:5d:11:22:33", -72, "LE"),
    _ble_item("Galaxy Buds", "7c:2e:0d:00:11:22", -64, "LE / audio"),
]


def _host_item(ip, mac, vendor):
    base = [f"Host   : {ip}", f"MAC    : {mac}", f"Vendor : {vendor}"]
    return Item(ip, children=[
        Item("Show details", base),
        Item("Fast scan (top 100)", _ok(f"nmap -F {ip}", "PORT    STATE SERVICE",
             "22/tcp  open  ssh", "80/tcp  open  http", "443/tcp open  https")),
        Item("Full scan (all ports)", _ok(f"nmap -p- {ip}", "22/tcp   open ssh",
             "80/tcp   open http", "443/tcp  open https", "8080/tcp open http-proxy")),
        Item("Ping x4", _ok(f"64 bytes from {ip}: icmp_seq=0 time=1.9 ms",
             f"64 bytes from {ip}: icmp_seq=1 time=2.1 ms", "0% packet loss")),
    ])


_HOSTS = [_host_item("192.168.1.1", "3c:84:6a:aa:bb:cc", "TP-LINK"),
          _host_item("192.168.1.42", "b8:27:eb:00:11:22", "Raspberry Pi"),
          _host_item("192.168.1.77", "a4:5e:60:de:ad:be", "Apple")]


def _share_item(name, kind, comment):
    return Item(f"{name}   ({kind})", children=[
        Item("Show details", [f"Share   : {name}", f"Type    : {kind}", f"Comment : {comment or '-'}"]),
        Item("List contents (anon)", _ok(f"smbclient //gw/{name} -N -c ls",
             "  .                                   D", "  readme.txt                       1256",
             "  backup.zip                      204800")),
    ])


_SHARES = [_share_item("public", "Disk", "Public files"),
           _share_item("backups", "Disk", "Nightly backups"),
           _share_item("IPC$", "IPC", "IPC Service")]


def _find(cat_label, act_label):
    cat = next(c for c in CATALOG if c.label == cat_label)
    return next(a for a in cat.actions if a.label == act_label)


for _lbl in ("Scan nearby networks", "List SSIDs + signal"):
    _find("Wi-Fi", _lbl).items = _SSIDS
_find("Bluetooth LE", "Scan for BLE devices (15s)").items = _BLE
_find("Nmap", "1. Scan / refresh hosts").items = _HOSTS
_find("SMB Client", "List shares on default gateway").items = _SHARES


# "Theme" category: one navigation row per accent (applied instantly, no process).
THEME_CATEGORY = Category("Theme", "Pick the UI accent color — applies instantly", IC["palette"],
                          [Action(name, "Set the UI accent color (applies instantly).", [], [], nav=True)
                           for name, _p, _i in theme.ACCENTS])
CATALOG.append(THEME_CATEGORY)

ABOUT = Category("About", "Version and usage notice", IC["info"], info_only=True)
ABOUT.text = (
    "Bruce CardputerZero\n"
    "Version 0.1.0  (pygame simulator)\n\n"
    "A security-testing toolkit UI that launches curl, netcat, iw, "
    "bluetoothctl, smbclient and Responder on the real device. This build is "
    "a desktop SIMULATION: it plays canned output and performs no real scan, "
    "transmission or capture.\n\n"
    "For authorized security testing, CTF and research use only."
)
CATALOG.append(ABOUT)

THEME_INDEX = CATALOG.index(THEME_CATEGORY)


if __name__ == "__main__":
    # ponytail self-check: catalog well-formed, icons present, sims terminate.
    assert len(CATALOG) == 21, len(CATALOG)
    for cat in CATALOG:
        assert cat.label and cat.desc and cat.icon
        if cat.info_only:
            assert not cat.actions
            continue
        assert cat.actions, cat.label
        for a in cat.actions:
            assert a.label and a.desc
            assert a.nav or a.sim, a.label          # runnable rows have sim output
            if a.sim:
                assert a.sim[-1] == "[process exited 0]", a.label  # every sim terminates
            for it in a.items:                      # interactive rows: each leaf has content
                assert it.children or it.lines, (a.label, it.label)
                for sub in it.children:
                    assert sub.children or sub.lines, (it.label, sub.label)
    assert CATALOG[0].actions[1].items, "SSID interactivity missing"
    assert CATALOG[THEME_INDEX].actions[0].nav
    print(f"catalog OK: {len(CATALOG)} categories, "
          f"{sum(len(c.actions) for c in CATALOG)} actions")
