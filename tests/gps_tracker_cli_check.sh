#!/bin/sh
# SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
#
# SPDX-License-Identifier: AGPL-3.0-only
#
# Check of the real executable's `--gps-tracker` (the same Tracker / Session /
# storage worker the GPS Tracker screen uses), plus `--wardrive`'s GPS side
# (config defaults, Auto cancel). The "port" is a plain file of
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

NMEA="$WORK/gps.nmea"
printf '%s\r\n' \
    '$GNGGA,152010.00,3540.87416,N,13946.02750,E,1,07,1.20,41.2,M,39.0,M,,*7B' \
    '$GNRMC,152010.00,A,3540.87416,N,13946.02750,E,0.50,10.0,221126,,,A,V*3A' > "$NMEA"
TRACK=261122_152010_gps_tracker.gpx

# 1. Normal run: one point logged, then EOF -> "GPS not Found!", track closed.
mkdir "$WORK/h1"
out=$(HOME="$WORK/h1" GPS_UART="$NMEA" timeout 30 "$BIN" --gps-tracker 2>&1)
rc=$?
echo "$out" | grep -q 'Coord: 35.681236, 139.767125' || fail "no Coord line: $out"
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
echo "$out" | grep -q 'Coord: 35.681236, 139.767125' || fail "no Coord from the configured port: $out"
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
echo "$out" | grep -q 'Coord: 35.681236, 139.767125' || fail "no Coord after Auto: $out"
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
mkdir -p "$WORK/h11/.bruce"
printf 'gpsBaudrate=19200\ngpsPort=%s\n' "$NMEA" > "$WORK/h11/.bruce/gps.conf"
out=$(cd "$WORK" && env -u GPS_UART -u GPS_BAUD HOME="$WORK/h11" timeout 30 "$BIN" --wardrive --wifi bruce-noif0 "$WORK/w1.csv" 2>&1)
rc=$?
echo "$out" | grep -q "GPS on $NMEA @ 19200 bps" || fail "--wardrive ignored gps.conf: $out"
echo "$out" | grep -q 'fix 35.68124,139.7671' || fail "--wardrive no fix from the configured port: $out"
[ "$rc" -eq 0 ] || fail "--wardrive saved-config exit $rc: $out"
out=$(cd "$WORK" && env -u GPS_UART HOME="$WORK/h11" GPS_BAUD=57600 timeout 30 "$BIN" --wardrive --wifi bruce-noif0 "$WORK/w2.csv" 2>&1)
echo "$out" | grep -q "GPS on $NMEA @ 57600 bps" || fail "GPS_BAUD did not override gps.conf: $out"
out=$(cd "$WORK" && HOME="$WORK/h11" GPS_UART=/nonexistent/gps0 timeout 30 "$BIN" --wardrive --wifi bruce-noif0 "$WORK/w3.csv" 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "GPS_UART did not override gps.conf for --wardrive (exit $rc): $out"
echo "$out" | grep -q "GPS on $NMEA" && fail "GPS_UART did not override gps.conf for --wardrive: $out"
#     The menu's form passes both options: no config is read (a HOME whose
#     gps.conf points elsewhere, or no HOME at all, changes nothing).
printf 'gpsBaudrate=9600\ngpsPort=/nonexistent/gps0\n' > "$WORK/h11/.bruce/gps.conf"
for home in "$WORK/h11" /nonexistent-home; do
    out=$(cd "$WORK" && HOME="$home" GPS_UART=/nonexistent/gps1 GPS_BAUD=4800 timeout 30 \
          "$BIN" --wardrive --wifi --port "$NMEA" --baud 38400 bruce-noif0 "$WORK/w4.csv" 2>&1)
    echo "$out" | grep -q "GPS on $NMEA @ 38400 bps" || fail "explicit --port/--baud did not win (HOME=$home): $out"
done
echo "ok   --wardrive: gps.conf used, GPS_BAUD/GPS_UART override it, explicit options win and skip the config"

# 11. --wardrive --port Auto on a silent port: SIGTERM (Back in the menu)
#     cancels the probe at once (it used to finish 5 baud windows x 1.5 s).
rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"   # a writer that never writes: the probe waits, no EOF
(cd "$WORK" && exec env HOME="$WORK/h11" BRUCE_GPS_PORT_CANDIDATES="$FIFO" \
    "$BIN" --wardrive --wifi --port Auto --baud 9600 bruce-noif0 "$WORK/w5.csv") > "$WORK/o11" 2>&1 &
pid=$!
sleep 0.5
t0=$(now_ms); kill -TERM "$pid"; wait "$pid"; rc=$?; el=$(( $(now_ms) - t0 ))
exec 3>&-
grep -q 'Auto: searching GPS on 1 port(s)' "$WORK/o11" || fail "wardrive Auto did not start: $(cat "$WORK/o11")"
grep -q 'Stopped before a GPS was found.' "$WORK/o11" || fail "wardrive Auto cancel not reported: $(cat "$WORK/o11")"
[ "$rc" -eq 0 ] || fail "wardrive Auto cancel exit $rc"
[ "$el" -lt 1000 ] || fail "wardrive Auto took ${el} ms to stop after SIGTERM"
[ ! -e "$WORK/w5.csv" ] || fail "wardrive Auto cancel created a CSV"
echo "ok   --wardrive Auto: SIGTERM stops the probe after ${el} ms, exit 0, no CSV"

echo "gps_tracker_cli_check: passed"
