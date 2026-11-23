#!/bin/sh
# SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
#
# SPDX-License-Identifier: AGPL-3.0-only
#
# Check of the real executable's `--gps-tracker` (the same Tracker / Session /
# storage worker the GPS Tracker screen uses). The "port" is a plain file of
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

echo "gps_tracker_cli_check: passed"
