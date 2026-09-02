# platform/rf — CC1101 Sub-GHz hardware layer

Hardware half of the Sub-GHz port. The pure protocol logic lives in
[`src/model/rf`](../../model/rf); this directory only talks to the radio and the
GDO0 line. Everything here compiles on desktop but only *runs* on the CM0.

## Pieces

- **`cc1101_regs.{h,cpp}`** — pure CC1101 register math (FREQ/data-rate/RX-BW/
  deviation) + register/strobe constants. Unit-tested by
  `tests/cc1101_regs_selfcheck.cpp` (freq anchor 433.92 MHz → `10 B0 71`).
- **`cc1101_driver.{h,cpp}`** — spidev I/O: register read/write/burst, strobes,
  reset/detect, `set_frequency_hz`, `configure_ook_async`, `apply_preset`,
  `enter_rx_async` / `enter_tx_async`. Xtal assumed **26 MHz**.
- **`rf_capture.{h,cpp}`** — `GpioEdgeCapture`: libgpiod edge-event capture on
  GDO0 → signed-µs durations (the decoder's input). Worker thread + mutex buffer
  + `poll()`, mirroring `ProcessRunner` (worker never touches LVGL).
- **`rf_transmit.{h,cpp}`** — `GpioBitBangTx`: drives GDO0 per a duration list
  with absolute `clock_nanosleep` deadlines.

libgpiod is optional at build time (`APP_HAVE_LIBGPIOD`, wired in the root
`CMakeLists.txt`). Without it, capture/transmit are safe no-op stubs so the
desktop emulator links. **v1 API** (`gpiod.h`, `gpiod_line_request_*`) is used.

## Still needs on-device bring-up (not verifiable off-hardware)

1. **GDO0 kernel line offset** — the CAP's GDO0 must be mapped to a
   `gpiochipN` + line offset. The silkscreen `G#` is NOT the kernel offset;
   confirm with `gpioinfo`. Config defaults preload the M5 CAP reference wiring
   (GDO0, RF switch, GDO2) — see `docs/cardputerzero-gpio.md` for how to resolve
   each signal to a real CM0 offset. Wire the resolved chip/offset where the RF
   screens construct `GpioEdgeCapture` / `GpioBitBangTx`. Note the CC1101 CAP
   also has an **RF_SW0 antenna switch** the driver must drive for TX vs RX.
2. **Baseline OOK register block** (`kOokAsyncConfig` in `cc1101_driver.cpp`) —
   SmartRF-derived starting values. AGC (`AGCCTRL*`) and PA (`set_ook_patable`)
   almost certainly need tuning against the real CAP for clean RX/TX.
3. **spidev CS / mode** — detection already works over `/dev/spidev0.0`; confirm
   RX/TX at 1 MHz SPI, adjust if the CAP needs a different node or speed.
4. **TX timing jitter** — userspace bit-bang. If a picky receiver rejects
   frames, pin the process to a CPU and/or raise scheduling priority
   (`SCHED_FIFO`) around `GpioBitBangTx::transmit`.

## Deferred from Phase 1

The KeeLoq `/mfcodes` encrypted keystore reader + full `keeloq_identify` loop
(needs mbedTLS + the Momentum data blob) are not ported yet. The cipher core and
all learning schemes already are (`model/rf/rf_keeloq`).
