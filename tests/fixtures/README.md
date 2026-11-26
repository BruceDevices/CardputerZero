# Test fixtures

Captured tool output used by `tests/wardrive_selfcheck.cpp` and
`tests/gps_tracker_cli_check.sh`. All identifiers are synthetic: MACs were
replaced with locally administered `02:00:5e:...` addresses and names/SSIDs
with placeholders; manufacturer payload bytes were zeroed.

- `bluetoothctl_scan_le_5.85.txt`: `bluetoothctl --timeout 6 scan le`
  (BlueZ 5.85) on a desktop PC, colour escapes kept as printed.
- `iw_scan_6.17.txt`: the first block is `iw dev <if> scan dump` (iw 6.17)
  from a real AP (RSN+WPA PSK); the WPS details and country element were
  dropped. The other blocks are written in iw's print format (scan.c) to
  cover each Bruce AuthMode: SAE, PSK+SAE, 802.1X, WEP, open, OWE, WPA-only,
  a hidden SSID, and SSIDs with commas, quotes, UTF-8 and escaped bytes.
