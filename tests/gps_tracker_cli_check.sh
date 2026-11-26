#!/bin/sh
# SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
#
# SPDX-License-Identifier: AGPL-3.0-only
#
# Check of the real executable's `--gps-tracker` (the same Tracker / Session /
# storage worker the GPS Tracker screen uses), plus `--wardrive` (config
# defaults, Auto cancel, and Bruce Wardriving end to end with canned
# iw / bluetoothctl output from tests/fixtures on PATH). The "port" is a plain file of
# NMEA: it is read to EOF, which the tracker treats as an unplugged GPS.
#
#   tests/gps_tracker_cli_check.sh <path/to/bruce_cardputerzero>
#
# Runs as a normal user, with the default SIGXFSZ disposition. Cases 4-6 use the
# test-only BRUCE_GPS_TEST_IO_DELAY_MS hook to simulate storage that never
# responds (every GPX file operation is delayed by that many ms).

set -u
BIN=${1:?usage: $0 <bruce_cardputerzero binary>}
WORK=$(mktemp -d /tmp/gps_cli_check.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

NMEA="$WORK/gps.nmea"  # synthetic: near 0 N / 0 E (open sea)
printf '%s\r\n' \
    '$GNGGA,152010.00,0000.74070,N,00004.07340,E,1,07,1.20,41.2,M,39.0,M,,*7C' \
    '$GNRMC,152010.00,A,0000.74070,N,00004.07340,E,0.50,10.0,221126,,,A,V*3D' > "$NMEA"
TRACK=261122_152010_gps_tracker.gpx
SYN="$WORK/syn.nmea"   # near 0 N / 0 E (open sea)
printf '%s\r\n' \
    '$GNGGA,152002.00,0000.74070,N,00004.07340,E,1,07,1.20,12.3,M,0.0,M,,*42' \
    '$GNRMC,152002.00,A,0000.74070,N,00004.07340,E,0.50,10.0,221126,,,A,V*3E' > "$SYN"

# 1. Normal run: one point logged, then EOF -> "GPS not Found!", track closed.
mkdir "$WORK/h1"
out=$(HOME="$WORK/h1" GPS_UART="$NMEA" timeout 30 "$BIN" --gps-tracker 2>&1)
rc=$?
echo "$out" | grep -q 'Coord: 0.012345, 0.067890' || fail "no Coord line: $out"
echo "$out" | grep -q 'GPS not Found!' || fail "no 'GPS not Found!': $out"
[ "$rc" -eq 1 ] || fail "normal run exit $rc (want 1: ended by GPS not Found)"
[ "$(grep -c '<trkpt' "$WORK/h1/BruceGPS/$TRACK")" -eq 1 ] || fail "GPX point count"
tail -c 9 "$WORK/h1/BruceGPS/$TRACK" | grep -q '</gpx>' || fail "GPX not finalized"
echo "ok   normal run: 1 point, finalized GPX, exit 1 after EOF"

# 2. RLIMIT_FSIZE = 0 with the default SIGXFSZ disposition: the write fails
#    with EFBIG and is reported; the process is not killed (128+25 = 153) and
#    leaves no partial GPX behind.
mkdir "$WORK/h2"
out=$( (trap - XFSZ; ulimit -f 0; HOME="$WORK/h2" GPS_UART="$NMEA" timeout 30 "$BIN" --gps-tracker) 2>&1)
rc=$?
[ "$rc" -ne 153 ] || fail "killed by SIGXFSZ"
[ "$rc" -eq 1 ] || fail "file-size-limited run exit $rc: $out"
echo "$out" | grep -q 'Failed to write GPS file' || fail "write error not reported: $out"
[ ! -e "$WORK/h2/BruceGPS/$TRACK" ] || fail "partial GPX left: $(wc -c < "$WORK/h2/BruceGPS/$TRACK") bytes"
echo "ok   ulimit -f 0: 'Failed to write GPS file', exit 1, no partial GPX, not killed"

# 3. Root is refused before the port is opened (user namespace: euid 0 without
#    real privileges). Skipped where unprivileged user namespaces are blocked.
if unshare -r true 2>/dev/null; then
    mkdir "$WORK/h3"
    out=$(HOME="$WORK/h3" GPS_UART="$NMEA" unshare -r timeout 30 "$BIN" --gps-tracker 2>&1)
    rc=$?
    echo "$out" | grep -q 'must run as your user, not root' || fail "root not refused: $out"
    [ "$rc" -eq 1 ] || fail "root run exit $rc"
    [ ! -e "$WORK/h3/BruceGPS" ] || fail "root run created files"
    echo "ok   root (user namespace) refused before opening the port"
else
    echo "skip root refusal: unprivileged user namespaces unavailable"
fi
# 4. Storage that never responds (test hook BRUCE_GPS_TEST_IO_DELAY_MS): the
#    run ends by EOF -> "GPS not Found!", then waits at most the one shutdown
#    deadline (3 s), reports the unfinished track and exits 1.
now_ms() { date +%s%3N; }
mkdir "$WORK/h4"
t0=$(now_ms)
out=$(HOME="$WORK/h4" GPS_UART="$NMEA" BRUCE_GPS_TEST_IO_DELAY_MS=600000 timeout 60 "$BIN" --gps-tracker 2>&1)
rc=$?
el=$(( $(now_ms) - t0 ))
[ "$rc" -eq 1 ] || fail "stuck storage (EOF) exit $rc: $out"
echo "$out" | grep -q 'GPX write unfinished at shutdown' || fail "unfinished write not reported: $out"
[ "$(echo "$out" | grep -c 'GPX write unfinished')" -eq 1 ] || fail "reported more than once"
[ "$el" -lt 12000 ] || fail "EOF shutdown took ${el} ms"
echo "ok   stuck storage, EOF: reported once, exit 1 after ${el} ms (5 s pass + <=3 s deadline)"

# 5. SIGTERM while storage is stuck: the UART is released, the shutdown waits
#    at most the deadline, reports and exits 1.
FIFO="$WORK/gps.fifo"
mkfifo "$FIFO"
mkdir "$WORK/h5"
HOME="$WORK/h5" GPS_UART="$FIFO" BRUCE_GPS_TEST_IO_DELAY_MS=600000 "$BIN" --gps-tracker > "$WORK/o5" 2>&1 &
pid=$!
exec 3>"$FIFO"; cat "$NMEA" >&3   # keep the writer open: no EOF
sleep 2
t0=$(now_ms); kill -TERM "$pid"; wait "$pid"; rc=$?; el=$(( $(now_ms) - t0 ))
exec 3>&-
[ "$rc" -eq 1 ] || fail "SIGTERM with stuck storage exit $rc: $(cat "$WORK/o5")"
grep -q 'GPX write unfinished at shutdown' "$WORK/o5" || fail "not reported after SIGTERM"
[ "$el" -lt 4500 ] || fail "SIGTERM shutdown took ${el} ms"
echo "ok   SIGTERM + stuck storage: exit 1 after ${el} ms (deadline 3 s)"

# 6. A second SIGTERM during that wait forces the exit at once (128+15).
rm -f "$FIFO"; mkfifo "$FIFO"
mkdir "$WORK/h6"
HOME="$WORK/h6" GPS_UART="$FIFO" BRUCE_GPS_TEST_IO_DELAY_MS=600000 "$BIN" --gps-tracker > "$WORK/o6" 2>&1 &
pid=$!
exec 3>"$FIFO"; cat "$NMEA" >&3
sleep 2
kill -TERM "$pid"; sleep 0.5
t0=$(now_ms); kill -TERM "$pid"; wait "$pid"; rc=$?; el=$(( $(now_ms) - t0 ))
exec 3>&-
[ "$rc" -eq 143 ] || fail "second SIGTERM exit $rc"
[ "$el" -lt 500 ] || fail "second SIGTERM took ${el} ms"
echo "ok   second SIGTERM: forced exit 143 after ${el} ms"

# 7. Any mix of SIGINT/SIGTERM: SIGINT first, then SIGTERM forces exit (143).
rm -f "$FIFO"; mkfifo "$FIFO"
mkdir "$WORK/h7"
HOME="$WORK/h7" GPS_UART="$FIFO" BRUCE_GPS_TEST_IO_DELAY_MS=600000 "$BIN" --gps-tracker > "$WORK/o7" 2>&1 &
pid=$!
exec 3>"$FIFO"; cat "$NMEA" >&3
sleep 2
kill -INT "$pid"; sleep 0.5
t0=$(now_ms); kill -TERM "$pid"; wait "$pid"; rc=$?; el=$(( $(now_ms) - t0 ))
exec 3>&-
[ "$rc" -eq 143 ] || fail "SIGINT then SIGTERM exit $rc"
[ "$el" -lt 500 ] || fail "SIGINT then SIGTERM took ${el} ms"
echo "ok   SIGINT then SIGTERM: forced exit 143 after ${el} ms"

# 8. GPS > Config: with no args and no GPS_UART, the port/baud come from
#    ~/.bruce/gps.conf (an unknown baud falls back to Bruce's 9600).
mkdir -p "$WORK/h8/.bruce"
printf 'gpsBaudrate=4800\ngpsPort=%s\n' "$NMEA" > "$WORK/h8/.bruce/gps.conf"
out=$(env -u GPS_UART -u GPS_BAUD HOME="$WORK/h8" timeout 30 "$BIN" --gps-tracker 2>&1)
echo "$out" | grep -q "GPS Tracker on $NMEA @ 9600 bps" || fail "gps.conf not used: $out"
echo "$out" | grep -q 'Coord: 0.012345, 0.067890' || fail "no Coord from the configured port: $out"
#    ... and GPS_UART still overrides the file.
out=$(env -u GPS_BAUD HOME="$WORK/h8" GPS_UART=/nonexistent/gps0 timeout 30 "$BIN" --gps-tracker 2>&1)
echo "$out" | grep -q 'GPS Tracker on /nonexistent/gps0 @ 9600 bps' || fail "GPS_UART did not override: $out"
echo "ok   gps.conf port/baud used (Bruce baud validation), GPS_UART overrides"

# 9. gpsPort=Auto: the candidates are probed and the one streaming NMEA wins
#    (BRUCE_GPS_PORT_CANDIDATES = test hook); none -> a clear error, exit 1.
mkdir -p "$WORK/h9/.bruce"
printf 'gpsBaudrate=115200\ngpsPort=Auto\n' > "$WORK/h9/.bruce/gps.conf"
: > "$WORK/silent"
out=$(env -u GPS_UART -u GPS_BAUD HOME="$WORK/h9" BRUCE_GPS_PORT_CANDIDATES="/nonexistent/ttyUSB0:$WORK/silent:$NMEA" \
      timeout 60 "$BIN" --gps-tracker 2>&1)
echo "$out" | grep -q "Auto: GPS on $NMEA @ 115200 bps" || fail "Auto did not pick the NMEA port: $out"
echo "$out" | grep -q 'Coord: 0.012345, 0.067890' || fail "no Coord after Auto: $out"
cp -r "$WORK/h9" "$WORK/h10"; rm -rf "$WORK/h10/BruceGPS"
out=$(env -u GPS_UART -u GPS_BAUD HOME="$WORK/h10" BRUCE_GPS_PORT_CANDIDATES="$WORK/silent" \
      timeout 60 "$BIN" --gps-tracker 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "Auto without NMEA exit $rc: $out"
echo "$out" | grep -q 'GPS not Found! (Auto: no NMEA on 1 port)' || fail "Auto none message: $out"
[ ! -e "$WORK/h10/BruceGPS" ] || fail "Auto failure created files"
echo "ok   Auto: NMEA port picked; none -> 'GPS not Found! (Auto: ...)', exit 1"

# 10. --wardrive from the console reads GPS > Config like --gps-tracker:
#     explicit --port/--baud > GPS_UART/GPS_BAUD > ~/.bruce/gps.conf.
#     (The interface does not exist: iw fails at once and logs 0 networks.)
mkdir -p "$WORK/h11/.bruce"
printf 'gpsBaudrate=19200\ngpsPort=%s\n' "$SYN" > "$WORK/h11/.bruce/gps.conf"
out=$(cd "$WORK" && env -u GPS_UART -u GPS_BAUD HOME="$WORK/h11" timeout 30 "$BIN" --wardrive wifi bruce-noif0 2>&1)
rc=$?
echo "$out" | grep -q "GPS on $SYN @ 19200 bps" || fail "--wardrive ignored gps.conf: $out"
echo "$out" | grep -q 'Coord: 0.012345, 0.067890' || fail "--wardrive no fix from the configured port: $out"
echo "$out" | grep -q 'GPS not Found!' || fail "--wardrive EOF not reported: $out"
[ "$rc" -eq 1 ] || fail "--wardrive saved-config exit $rc (want 1: ended by GPS not Found): $out"
[ -f "$WORK/h11/BruceWardriving/261122_152002_wardriving.csv" ] || fail "--wardrive (non-root, no --home) did not use \$HOME"
out=$(cd "$WORK" && env -u GPS_UART HOME="$WORK/h11" GPS_BAUD=57600 timeout 30 "$BIN" --wardrive wifi bruce-noif0 2>&1)
echo "$out" | grep -q "GPS on $SYN @ 57600 bps" || fail "GPS_BAUD did not override gps.conf: $out"
out=$(cd "$WORK" && HOME="$WORK/h11" GPS_UART=/nonexistent/gps0 timeout 30 "$BIN" --wardrive wifi bruce-noif0 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "GPS_UART did not override gps.conf for --wardrive (exit $rc): $out"
echo "$out" | grep -q "GPS on $SYN" && fail "GPS_UART did not override gps.conf for --wardrive: $out"
echo "$out" | grep -q 'GPS not Found!' || fail "missing node not 'GPS not Found!': $out"
#     The menu's form passes both options: no config is read (a HOME whose
#     gps.conf points elsewhere, or no HOME at all, changes nothing).
printf 'gpsBaudrate=9600\ngpsPort=/nonexistent/gps0\n' > "$WORK/h11/.bruce/gps.conf"
for home in "$WORK/h11" /nonexistent-home; do
    out=$(cd "$WORK" && HOME="$home" GPS_UART=/nonexistent/gps1 GPS_BAUD=4800 timeout 30 \
          "$BIN" --wardrive --wifi --port "$SYN" --baud 38400 bruce-noif0 2>&1)
    echo "$out" | grep -q "GPS on $SYN @ 38400 bps" || fail "explicit --port/--baud did not win (HOME=$home): $out"
done
echo "ok   --wardrive: gps.conf used, GPS_BAUD/GPS_UART override it, explicit options win and skip the config"

# 11. --wardrive --port Auto on a silent port: SIGTERM (Back in the menu)
#     cancels the probe at once (it used to finish 5 baud windows x 1.5 s).
rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"   # a writer that never writes: the probe waits, no EOF
mkdir -p "$WORK/h12"
(cd "$WORK" && exec env HOME="$WORK/h12" BRUCE_GPS_PORT_CANDIDATES="$FIFO" \
    "$BIN" --wardrive wifi --port Auto --baud 9600 bruce-noif0) > "$WORK/o11" 2>&1 &
pid=$!
sleep 0.5
t0=$(now_ms); kill -TERM "$pid"; wait "$pid"; rc=$?; el=$(( $(now_ms) - t0 ))
exec 3>&-
grep -q 'Searching GPS...' "$WORK/o11" || fail "wardrive Auto did not start: $(cat "$WORK/o11")"
grep -q '^Stopped\.' "$WORK/o11" || fail "wardrive Auto cancel not reported: $(cat "$WORK/o11")"
[ "$rc" -eq 0 ] || fail "wardrive Auto cancel exit $rc"
[ "$el" -lt 1000 ] || fail "wardrive Auto took ${el} ms to stop after SIGTERM"
ls "$WORK/h12/BruceWardriving/"*_wardriving.csv >/dev/null 2>&1 && fail "wardrive Auto cancel created a CSV"
echo "ok   --wardrive Auto: SIGTERM stops the probe after ${el} ms, exit 0, no CSV"

# --- Bruce Wardriving (canned iw / bluetoothctl on PATH, synthetic NMEA) ---
REPO=$(cd "$(dirname "$0")/.." && pwd)
FAKE="$WORK/fakebin"
mkdir -p "$FAKE"
printf '#!/bin/sh\nexec cat %s\n' "$REPO/tests/fixtures/iw_scan_6.17.txt" > "$FAKE/iw"
# bluetoothctl stand-in, like BlueZ: "scan le" replays the capture and keeps
# discovering until ended (or exits at once with BT_SCAN_EXITS=1); the
# interactive form answers "info <mac>" lines from stdin with an RSSI only
# while that scan still runs (BlueZ drops RSSIs when discovery stops).
cat > "$FAKE/bluetoothctl" <<EOF
#!/bin/sh
SCANPID=\${BT_SCANPID:-$WORK/bt.scan.pid}
if [ "\$1" = --timeout ]; then
    echo \$\$ > "\$SCANPID"
    cat $REPO/tests/fixtures/bluetoothctl_scan_le_5.85.txt
    [ "\${BT_SCAN_EXITS:-0}" = 1 ] && exit 0
    exec sleep "\$2"
fi
[ \$# -eq 0 ] || exit 1
printf 'Waiting to connect to bluetoothd...'
while read -r cmd mac; do
    printf '[bluetoothctl]> %s %s\\n' "\$cmd" "\$mac"
    [ "\$cmd" = quit ] && exit 0
    if kill -0 "\$(cat "\$SCANPID" 2>/dev/null)" 2>/dev/null; then
        printf 'Device %s (random)\\n\\tPaired: no\\n\\tRSSI: 0xffffffc4 (-60)\\n' "\$mac"
    else
        printf 'Device %s (random)\\n\\tPaired: no\\n' "\$mac"
    fi
done
EOF
chmod +x "$FAKE/iw" "$FAKE/bluetoothctl"
CSV=261122_152002_wardriving.csv

# 12. The GPS menu's form: `--wardrive both --ui --home <dir>` writes the
#     WiGLE 1.6 CSV and the sample alert.txt to <dir>/BruceWardriving (not to
#     $HOME), streams the screen protocol, ends on EOF with @X notfound.
mkdir -p "$WORK/h13" "$WORK/h13-env"
out=$(cd "$WORK" && PATH="$FAKE:$PATH" HOME="$WORK/h13-env" timeout 60 "$BIN" --wardrive both --ui --port "$SYN" \
      --baud 115200 --home "$WORK/h13" --iface wlan0 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "ui run exit $rc: $out"
echo "$out" | grep -q '^@F Wardriving$' || fail "no frame: $out"
echo "$out" | grep -q '^@G 1$' || fail "gpsConnected never set: $out"
echo "$out" | grep -q '^@1 Coord: 0.012345, 0.067890$' || fail "no Coord line: $out"
echo "$out" | grep -q '^@1 Scan done. WiFi: 10 BLE: 15$' || fail "no scan summary: $out"
echo "$out" | grep -q '^@P E GPS not Found!$' || fail "no GPS not Found! popup: $out"
[ "$(echo "$out" | tail -1)" = "@X notfound" ] || fail "end marker: $(echo "$out" | tail -1)"
D="$WORK/h13/BruceWardriving"
[ -f "$D/$CSV" ] || fail "CSV not in --home: $(ls -la "$D" 2>&1)"
[ ! -e "$WORK/h13-env/BruceWardriving" ] || fail "\$HOME used although --home was given"
head -1 "$D/$CSV" | grep -q '^WigleWifi-1.6,appRelease=v[0-9.]*,model=CardputerZero,' || fail "pre-header: $(head -1 "$D/$CSV")"
[ "$(grep -c ',WIFI$' "$D/$CSV")" -eq 10 ] || fail "WiFi rows: $(grep -c ',WIFI$' "$D/$CSV")"
[ "$(grep -c ',BLE$' "$D/$CSV")" -eq 15 ] || fail "BLE rows: $(grep -c ',BLE$' "$D/$CSV")"
grep -q '^02:00:5E:20:00:02,"Cafe, ""Free""",\[WPA3_PSK\],2026-11-22 15:20:02,36,5180,-61,' "$D/$CSV" || fail "quoted SSID row"
grep -q '^02:00:5e:10:00:05,"Name-05",Misc \[BLE\],2026-11-22 15:20:02,0,,-79,0.012345,0.067890,12.300000,1.200000,,0600,BLE$' "$D/$CSV" \
    || fail "BLE row: $(grep 10:00:05 "$D/$CSV")"
grep -q '^02:00:5e:10:00:02,"",Misc \[BLE\],2026-11-22 15:20:02,0,,-60,' "$D/$CSV" \
    || fail "RSSI not queried for a device without one: $(grep 10:00:02 "$D/$CSV")"
awk -F, '$NF=="BLE" && $7==0' "$D/$CSV" | grep -q . && fail "BLE row with RSSI 0"
printf '# Alert MAC addresses - one per line\r\n# Lines starting with # are comments\r\n# Example:\r\n# aa:bb:cc:dd:ee:ff\r\n' | cmp -s - "$D/alert.txt" \
    || fail "alert.txt sample"
[ "$(stat -c %a "$D/$CSV")" = 644 ] && [ "$(stat -c %a "$D")" = 755 ] || fail "modes: $(stat -c '%a %n' "$D" "$D/$CSV")"
echo "ok   ui run: WiGLE 1.6 CSV (10 WiFi + 15 BLE rows) and alert.txt in --home, protocol frames, @X notfound"

# 13. Alerts: a MAC from alert.txt -> "Loaded 1 alert MACs", the ALERT popup,
#     " Alert: 1" in the banner; the second run appends without a new header.
printf '02:00:5e:20:00:04\n' >> "$D/alert.txt"
out=$(cd "$WORK" && PATH="$FAKE:$PATH" timeout 60 "$BIN" --wardrive wifi --ui --port "$SYN" --baud 115200 \
      --home "$WORK/h13" --iface wlan0 2>&1)
echo "$out" | grep -q '^@1 Loaded 1 alert MACs$' || fail "alert MACs not loaded: $out"
echo "$out" | grep -q '^@P E ALERT: WiFi found! Name: Corp MAC: 02:00:5E:20:00:04$' || fail "no ALERT popup: $out"
echo "$out" | grep -q '^@1 Found WiFi: 10 Alert: 1$' || fail "no Alert count: $out"
[ "$(grep -c '^WigleWifi-1.6' "$D/$CSV")" -eq 1 ] || fail "header written twice"
[ "$(grep -c ',WIFI$' "$D/$CSV")" -eq 20 ] || fail "second run did not append"
echo "ok   alerts: Loaded 1 alert MACs, ALERT popup, Alert: 1; same-second rerun appends"

# 14. Back during a scan: SIGTERM ends the scan tool and the run within ~1 s.
#     It is sent twice at once, as a wrapper that forwards the process-group
#     signal delivers it: the duplicate must not skip the orderly stop.
printf '#!/bin/sh\necho $$ > %s\nexec sleep 30\n' "$WORK/iw.pid" > "$FAKE/iw"
chmod +x "$FAKE/iw"
rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"
cat "$SYN" >&3
mkdir -p "$WORK/h14"
(cd "$WORK" && exec env PATH="$FAKE:$PATH" "$BIN" --wardrive wifi --ui --port "$FIFO" --baud 115200 \
    --home "$WORK/h14" --iface wlan0) > "$WORK/o14" 2>&1 &
pid=$!
for i in $(seq 1 50); do [ -s "$WORK/iw.pid" ] && break; sleep 0.1; done
[ -s "$WORK/iw.pid" ] || fail "scan never started: $(cat "$WORK/o14")"
t0=$(now_ms); kill -TERM "$pid"; kill -TERM "$pid"; wait "$pid"; rc=$?; el=$(( $(now_ms) - t0 ))
exec 3>&-
[ "$rc" -eq 0 ] || fail "SIGTERM during scan exit $rc: $(cat "$WORK/o14")"
[ "$el" -lt 1500 ] || fail "SIGTERM during scan took ${el} ms"
kill -0 "$(cat "$WORK/iw.pid")" 2>/dev/null && fail "the scan tool survived"
[ "$(tail -1 "$WORK/o14")" = "@X stopped" ] || fail "end marker: $(tail -1 "$WORK/o14")"
[ "$(grep -c ',WIFI$' "$WORK/h14/BruceWardriving/$CSV")" -eq 0 ] || fail "an interrupted scan was logged"
echo "ok   SIGTERM during a scan: exit 0 after ${el} ms, scan tool killed, nothing logged"

# 15. Storage errors: ~/BruceWardriving as a symlink is not followed
#     ("Failed to open file for writing", waits for a key); a missing home is
#     "Storage setup error".
mkdir -p "$WORK/h15" "$WORK/elsewhere"
ln -s "$WORK/elsewhere" "$WORK/h15/BruceWardriving"
printf '#!/bin/sh\nexec cat %s\n' "$REPO/tests/fixtures/iw_scan_6.17.txt" > "$FAKE/iw"
out=$(cd "$WORK" && PATH="$FAKE:$PATH" timeout 60 "$BIN" --wardrive wifi --ui --port "$SYN" --baud 115200 \
      --home "$WORK/h15" --iface wlan0 2>&1)
rc=$?
echo "$out" | grep -q '^@P W Failed to open file for writing$' || fail "symlinked dir: $out"
[ "$(echo "$out" | tail -1)" = "@X storage" ] && [ "$rc" -eq 1 ] || fail "symlinked dir end: rc=$rc $out"
[ -z "$(ls -A "$WORK/elsewhere")" ] || fail "wrote through the symlink: $(ls -A "$WORK/elsewhere")"
out=$(cd "$WORK" && PATH="$FAKE:$PATH" timeout 60 "$BIN" --wardrive wifi --ui --port "$SYN" --baud 115200 \
      --home /nonexistent-home --iface wlan0 2>&1)
echo "$out" | grep -q '^@P W Storage setup error$' || fail "missing home: $out"
echo "ok   storage errors: symlinked folder not followed, missing home -> Storage setup error"

# 16. As root (user namespace) without --home: the files go to the sudo
#     user's passwd home, never to $HOME. With an unknown SUDO_UID there is
#     no home: "Storage setup error", and $HOME stays untouched.
if unshare -r true 2>/dev/null; then
    mkdir -p "$WORK/h16"
    out=$(cd "$WORK" && PATH="$FAKE:$PATH" HOME="$WORK/h16" SUDO_UID=3999999 SUDO_GID=3999999 \
          unshare -r timeout 60 "$BIN" --wardrive wifi --ui --port "$SYN" --baud 115200 --iface wlan0 2>&1)
    echo "$out" | grep -q '^@P W Storage setup error$' || fail "root without --home: $out"
    [ ! -e "$WORK/h16/BruceWardriving" ] || fail "root run used \$HOME"
    echo "ok   root without --home ignores \$HOME"
else
    echo "skip root HOME check: unprivileged user namespaces unavailable"
fi

# --- review fixes ---------------------------------------------------------------
iw_fixture() { printf '#!/bin/sh\nexec cat %s\n' "$REPO/tests/fixtures/iw_scan_6.17.txt" > "$FAKE/iw"; chmod +x "$FAKE/iw"; }
stop_during_scan() {  # $1 = home; the fake iw writes $WORK/iw.pid; prints "rc el"
    rm -f "$WORK/iw.pid" "$FIFO"; mkfifo "$FIFO"
    exec 3<>"$FIFO"; cat "$SYN" >&3
    (cd "$WORK" && exec env PATH="$FAKE:$PATH" "$BIN" --wardrive wifi --ui --port "$FIFO" --baud 115200 \
        --home "$1" --iface wlan0) > "$WORK/o_stop" 2>&1 &
    p=$!
    for i in $(seq 1 50); do [ -s "$WORK/iw.pid" ] && break; sleep 0.1; done
    [ -s "$WORK/iw.pid" ] || fail "scan never started: $(cat "$WORK/o_stop")"
    sleep 0.2
    t0=$(date +%s%N); kill -TERM "$p"; wait "$p"; r=$?; t1=$(date +%s%N)
    exec 3>&-
    echo "$r $(( (t1 - t0) / 1000000 ))"
}
gone() { for i in $(seq 1 100); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.01; done; return 1; }

# 17. A scan tool that closes its output and keeps running: Back still ends
#     it and the run within the stop budget.
printf '#!/bin/sh\necho $$ > %s\nexec >&- 2>&-\nexec sleep 30\n' "$WORK/iw.pid" > "$FAKE/iw"; chmod +x "$FAKE/iw"
mkdir -p "$WORK/h17"
set -- $(stop_during_scan "$WORK/h17")
[ "$1" -eq 0 ] || fail "closed-output scan: exit $1: $(cat "$WORK/o_stop")"
[ "$2" -lt 1000 ] || fail "closed-output scan: stop took $2 ms"
gone "$(cat "$WORK/iw.pid")" || fail "closed-output scan tool survived"
echo "ok   scan tool with closed output: stopped after $2 ms, tool gone"

# 18. A scan descendant that ignores SIGTERM is killed too.
printf '#!/bin/sh\n(trap "" TERM; exec sleep 30) &\necho $! > %s.child\necho $$ > %s\nexec sleep 30\n' \
    "$WORK/iw.pid" "$WORK/iw.pid" > "$FAKE/iw"; chmod +x "$FAKE/iw"
mkdir -p "$WORK/h18"
set -- $(stop_during_scan "$WORK/h18")
[ "$1" -eq 0 ] && [ "$2" -lt 1000 ] || fail "descendant case: exit $1 after $2 ms"
gone "$(cat "$WORK/iw.pid.child")" || fail "a SIGTERM-ignoring scan descendant survived"
echo "ok   SIGTERM-ignoring scan descendant killed (stop after $2 ms)"
iw_fixture

# 19. BLE discovery fails after bluetoothctl listed a cached device: no BLE
#     row, the "Failed to init BLE" stripe (deviation: Bruce logs it only).
printf '#!/bin/sh\nprintf "[NEW] Device 02:00:5E:30:00:01 Cached\\nFailed to start discovery: org.bluez.Error.NotReady\\n"\nexit 1\n' \
    > "$WORK/btfail"; chmod +x "$WORK/btfail"
mkdir -p "$WORK/h19/fake"; cp "$WORK/btfail" "$WORK/h19/fake/bluetoothctl"
out=$(cd "$WORK" && PATH="$WORK/h19/fake:$FAKE:$PATH" timeout 60 "$BIN" --wardrive ble --ui --port "$SYN" --baud 115200 \
      --home "$WORK/h19" --iface wlan0 2>&1)
echo "$out" | grep -q '^@P E Failed to init BLE$' || fail "no BLE failure stripe: $out"
echo "$out" | grep -q '^@1 Scan done' && fail "summary after a failed BLE start: $out"
[ "$(grep -c ',BLE$' "$WORK/h19/BruceWardriving/$CSV")" -eq 0 ] || fail "cached device logged after failed discovery"
echo "ok   failed BLE discovery: nothing logged, 'Failed to init BLE' stripe"

# 20. Storage that takes 10 s to sync: Back still ends the run within the
#     bounded shutdown wait (2.5 s), reports it and exits 1.
mkdir -p "$WORK/h20"; rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"; cat "$SYN" >&3
(cd "$WORK" && exec env PATH="$FAKE:$PATH" BRUCE_WARDRIVE_TEST_SYNC_DELAY_MS=10000 "$BIN" --wardrive wifi --ui \
    --port "$FIFO" --baud 115200 --home "$WORK/h20" --iface wlan0) > "$WORK/o20" 2>&1 &
pid=$!
for i in $(seq 1 100); do grep -q '^@1 Scan done' "$WORK/o20" 2>/dev/null && break; sleep 0.1; done
grep -q '^@1 Scan done' "$WORK/o20" || fail "slow sync: no pass: $(cat "$WORK/o20")"
t0=$(date +%s%N); kill -TERM "$pid"; wait "$pid"; rc=$?; el=$(( ($(date +%s%N) - t0) / 1000000 ))
exec 3>&-
[ "$rc" -eq 1 ] || fail "slow sync exit $rc: $(cat "$WORK/o20")"
[ "$el" -lt 3500 ] || fail "slow sync: stop took $el ms"
grep -q 'CSV sync unfinished at shutdown' "$WORK/o20" || fail "unfinished sync not reported: $(cat "$WORK/o20")"
grep -q '^@X stopped$' "$WORK/o20" || fail "slow sync end marker"
[ "$(grep -c ',WIFI$' "$WORK/h20/BruceWardriving/$CSV")" -eq 10 ] || fail "slow sync rows"
echo "ok   storage slow to sync: stop after $el ms, 'CSV sync unfinished at shutdown', exit 1"

# 21. The GPS port can be reopened right after Back (no exclusive lock left),
#     also while something else keeps the tty open (a pty slave held here).
if command -v python3 >/dev/null 2>&1; then
    python3 -I -c '
import os, pty, sys, time
m, s = pty.openpty()
print(os.ttyname(s), flush=True)
nmea = open(sys.argv[1], "rb").read()
end = time.time() + 40
while time.time() < end:
    os.write(m, nmea); time.sleep(0.5)
' "$SYN" > "$WORK/pty.name" &
    ptyp=$!
    for i in $(seq 1 30); do [ -s "$WORK/pty.name" ] && break; sleep 0.1; done
    PTY=$(cat "$WORK/pty.name")
    mkdir -p "$WORK/h21"
    for run in 1 2; do
        (cd "$WORK" && exec env PATH="$FAKE:$PATH" "$BIN" --wardrive wifi --ui --port "$PTY" --baud 115200 \
            --home "$WORK/h21" --iface wlan0) > "$WORK/o21.$run" 2>&1 &
        p=$!
        for i in $(seq 1 60); do grep -q '^@G 1$' "$WORK/o21.$run" 2>/dev/null && break; sleep 0.1; done
        kill -TERM "$p"; wait "$p"
        grep -q "GPS on $PTY @ 115200 bps" "$WORK/o21.$run" || fail "pty run $run: $(cat "$WORK/o21.$run")"
    done
    kill "$ptyp" 2>/dev/null; wait "$ptyp" 2>/dev/null
    echo "ok   GPS pty reopened after Back (held slave): no 'busy'"
else
    echo "skip pty reopen check: python3 unavailable"
fi

# --- review round 2 ---------------------------------------------------------------
if command -v python3 >/dev/null 2>&1; then
    # A pty GPS that keeps its slave open (so a leftover TIOCEXCL would show):
    # "fix" streams one fix sentence pair every 0.5 s; "trigger" streams no-fix
    # sentences and sends exactly one fix pair once $WORK/sendfix exists.
    cat > "$WORK/ptygps.py" <<'PY'
import os, pty, sys, time
def n(b):
    c = 0
    for ch in b: c ^= ord(ch)
    return ("$%s*%02X\r\n" % (b, c)).encode()
m, s = pty.openpty()
print(os.ttyname(s), flush=True)
fix = (n("GNGGA,152002.00,0000.74070,N,00004.07340,E,1,07,1.20,12.3,M,0.0,M,,") +
       n("GNRMC,152002.00,A,0000.74070,N,00004.07340,E,0.50,10.0,221126,,,A,V"))
# same UTC as the fix: the CSV name is the same whether it comes from a
# no-fix pass (Bruce's year rule) or from the fix's scan
nofix = n("GNGGA,152002.00,,,,,0,00,25.50,,,,,,") + n("GNRMC,152002.00,V,,,,,,,221126,,,N,V")
end = time.time() + 60; sent = False
while time.time() < end:
    if sys.argv[1] == "fix":
        os.write(m, fix)
    elif not sent and os.path.exists(sys.argv[2]):
        os.write(m, fix); sent = True
    else:
        os.write(m, nofix)
    time.sleep(0.5)
PY
    start_pty() {  # $1 = mode; sets PTY and PTYP
        rm -f "$WORK/pty.$1"
        python3 -I "$WORK/ptygps.py" "$1" "$WORK/sendfix" > "$WORK/pty.$1" &
        PTYP=$!
        for i in $(seq 1 30); do [ -s "$WORK/pty.$1" ] && break; sleep 0.1; done
        PTY=$(cat "$WORK/pty.$1")
    }

    # 22. A file size limit with the DEFAULT SIGXFSZ disposition: the write
    #     fails (EFBIG) instead of killing the run, "Failed to write file"
    #     waits for OK, and the GPS port is released (a held pty reopens).
    start_pty fix
    # alert.txt exists, so the first write past the limit (the CSV header)
    # comes after the GPS port was opened
    mkdir -p "$WORK/h22/BruceWardriving"; printf '# none\n' > "$WORK/h22/BruceWardriving/alert.txt"
    out=$( (trap - XFSZ; ulimit -f 0; cd "$WORK" && exec env PATH="$FAKE:$PATH" timeout 60 "$BIN" --wardrive wifi --ui \
          --port "$PTY" --baud 115200 --home "$WORK/h22" --iface wlan0) 2>&1)
    rc=$?
    [ "$rc" -ne 153 ] || fail "killed by SIGXFSZ"
    [ "$rc" -eq 1 ] || fail "file size limit exit $rc: $out"
    echo "$out" | grep -q '^@P W Failed to write file$' || fail "no write error popup: $out"
    [ "$(echo "$out" | tail -1)" = "@X storage" ] || fail "end marker: $(echo "$out" | tail -1)"
    out=$(cd "$WORK" && PATH="$FAKE:$PATH" timeout 5 "$BIN" --wardrive wifi --ui --port "$PTY" --baud 115200 \
          --home "$WORK/h22b" --iface wlan0 2>&1)
    echo "$out" | grep -q "GPS on $PTY @ 115200 bps" || fail "GPS port not released after the write error: $out"
    kill "$PTYP" 2>/dev/null; wait "$PTYP" 2>/dev/null
    echo "ok   SIGXFSZ (default disposition): 'Failed to write file', @X storage, exit 1, GPS port reopens"

    # 23. fdatasync() of the CSV fails (EIO, injected for the CSV only) after
    #     a completed scan pass, and the GPS then sends only no-fix data: the
    #     run still ends with the storage error by itself, in the inter-pass
    #     wait. alert.txt exists, so no startup write is involved; the one fix
    #     is sent only once the run is connected, so it must lead to a scan.
    rm -f "$WORK/sendfix" "$WORK/iw.calls"
    printf '#!/bin/sh\necho x >> %s\nexec cat %s\n' "$WORK/iw.calls" "$REPO/tests/fixtures/iw_scan_6.17.txt" > "$FAKE/iw"
    start_pty trigger
    mkdir -p "$WORK/h23/BruceWardriving"; printf '# none\n' > "$WORK/h23/BruceWardriving/alert.txt"
    (cd "$WORK" && exec env PATH="$FAKE:$PATH" BRUCE_WARDRIVE_TEST_SYNC_ERRNO=5 ${WARDRIVE_TEST_PRELOAD:+LD_PRELOAD=$WARDRIVE_TEST_PRELOAD} \
        timeout 40 "$BIN" --wardrive wifi --ui --port "$PTY" --baud 115200 --home "$WORK/h23" --iface wlan0) \
        > "$WORK/o23" 2>&1 &
    pid=$!
    for i in $(seq 1 100); do grep -q '^@G 1$' "$WORK/o23" 2>/dev/null && break; sleep 0.1; done
    grep -q '^@G 1$' "$WORK/o23" || fail "case 23: never connected: $(cat "$WORK/o23")"
    touch "$WORK/sendfix"
    t0=$(date +%s%N)
    wait "$pid"; rc=$?; el=$(( ($(date +%s%N) - t0) / 1000000 ))
    kill "$PTYP" 2>/dev/null; wait "$PTYP" 2>/dev/null
    [ "$(grep -c '^@1 Scan done. WiFi: 10$' "$WORK/o23")" -ge 1 ] || fail "case 23: no completed scan: $(cat "$WORK/o23")"
    set -- "$WORK"/h23/BruceWardriving/*_wardriving.csv
    [ "$#" -eq 1 ] && [ "$(basename "$1")" = "$CSV" ] || fail "case 23: want exactly $CSV, got: $*"
    [ "$(grep -c ',WIFI$' "$1")" -eq 10 ] || fail "case 23: CSV rows"
    [ "$(wc -l < "$WORK/iw.calls")" -eq 1 ] || fail "case 23: $(wc -l < "$WORK/iw.calls") scans, want exactly 1"
    [ "$rc" -eq 1 ] || fail "case 23: exit $rc (124 = never ended by itself): $(tail -3 "$WORK/o23")"
    grep -q 'sync: Input/output error' "$WORK/o23" || fail "case 23: sync error not logged"
    grep -q '^@P W Failed to write file$' "$WORK/o23" || fail "case 23: no storage popup"
    [ "$(tail -1 "$WORK/o23")" = "@X storage" ] || fail "case 23: end marker: $(tail -1 "$WORK/o23")"
    echo "ok   CSV sync error after one scan, then no-fix only: ended by itself ${el} ms after the fix, @X storage"
    iw_fixture
else
    echo "skip SIGXFSZ / deferred sync cases: python3 unavailable"
fi

# 24. The CSV sync error surfaces only at shutdown: Back (SIGTERM) right
#     after a completed pass, while the 1 s sync is still running and then
#     fails (EIO, CSV only). It is reported over "stopped", so the screen
#     shows it and waits for OK; GPS-first cleanup, bounded wait.
mkdir -p "$WORK/h24/BruceWardriving"; printf '# none\n' > "$WORK/h24/BruceWardriving/alert.txt"
rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"; cat "$SYN" >&3
(cd "$WORK" && exec env PATH="$FAKE:$PATH" BRUCE_WARDRIVE_TEST_SYNC_DELAY_MS=1000 BRUCE_WARDRIVE_TEST_SYNC_ERRNO=5 \
    ${WARDRIVE_TEST_PRELOAD:+LD_PRELOAD=$WARDRIVE_TEST_PRELOAD} \
    "$BIN" --wardrive wifi --ui --port "$FIFO" --baud 115200 --home "$WORK/h24" --iface wlan0) > "$WORK/o24" 2>&1 &
pid=$!
for i in $(seq 1 200); do grep -q '^@1 Scan done' "$WORK/o24" 2>/dev/null && break; sleep 0.02; done
grep -q '^@1 Scan done. WiFi: 10$' "$WORK/o24" || { kill "$pid" 2>/dev/null; fail "case 24: no completed pass: $(cat "$WORK/o24")"; }
kill -0 "$pid" 2>/dev/null || fail "case 24: the run ended before Back: $(cat "$WORK/o24")"
grep -q 'Failed to write file' "$WORK/o24" && fail "case 24: the error came before Back (not a shutdown case)"
kill -TERM "$pid" || fail "case 24: SIGTERM not delivered"
wait "$pid"; rc=$?
exec 3>&-
[ "$(grep -c ',WIFI$' "$WORK/h24/BruceWardriving/$CSV")" -eq 10 ] || fail "case 24: CSV rows"
grep -q 'sync: Input/output error' "$WORK/o24" || fail "case 24: sync error not logged: $(cat "$WORK/o24")"
grep -q '^@P W Failed to write file$' "$WORK/o24" || fail "case 24: shutdown sync error not drawn: $(tail -5 "$WORK/o24")"
[ "$(tail -1 "$WORK/o24")" = "@X storage" ] || fail "case 24: end marker: $(tail -1 "$WORK/o24")"
[ "$rc" -eq 1 ] || fail "case 24: exit $rc"
echo "ok   CSV sync error found at shutdown (Back after a pass): drawn, @X storage (not stopped), exit 1"

# 25. A scan that ends before the RSSI lookup (BlueZ has then dropped the
#     RSSIs): the 5 devices bluetoothctl showed without an RSSI are not
#     logged (never with RSSI 0); the 10 with one are.
mkdir -p "$WORK/h25"
out=$(cd "$WORK" && PATH="$FAKE:$PATH" BT_SCAN_EXITS=1 timeout 60 "$BIN" --wardrive ble --ui --port "$SYN" \
      --baud 115200 --home "$WORK/h25" --iface wlan0 2>&1)
echo "$out" | grep -q '^@1 Scan done. BLE: 15$' || fail "case 25: no scan summary: $out"
[ "$(grep -c ',BLE$' "$WORK/h25/BruceWardriving/$CSV")" -eq 10 ] || fail "case 25: BLE rows: $(grep -c ',BLE$' "$WORK/h25/BruceWardriving/$CSV")"
awk -F, '$NF=="BLE" && $7==0' "$WORK/h25/BruceWardriving/$CSV" | grep -q . && fail "case 25: BLE row with RSSI 0"
grep -q '^02:00:5e:10:00:02,' "$WORK/h25/BruceWardriving/$CSV" && fail "case 25: a device without RSSI was logged"
echo "ok   BLE devices without a known RSSI are not logged (no RSSI 0 rows)"

echo "gps_tracker_cli_check: passed"
