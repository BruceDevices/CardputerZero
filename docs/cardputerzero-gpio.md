# Choosing GPIOs on the CardputerZero (for CAP modules)

How to map an M5Stack **CAP** module (CC1101, LoRa-1262, and the ones coming
next) to the right pins on the **CardputerZero**, which is a full Linux device
(CM0, aarch64) rather than an ESP32.

## The core idea: signal → connector pin → kernel address

A GPIO on Linux is **not** a single global number. It is addressed by two
things, which is exactly why the Config screen has two fields per line:

- **chip** — which GPIO controller, a `gpiochipN` node in `/dev` (a board can
  have several: the SoC bank, an I²C expander, …).
- **line** (offset) — the line number **inside that chip** (0, 1, 2, …).

So "GDO0 chip = `gpiochip0`" + "GDO0 line = `23`" means *line 23 of gpiochip0*.
The code passes exactly that pair to `gpiod_chip_open_by_name(chip)` +
`gpiod_chip_get_line(chip, offset)`.

`line = -1` shows as **"unset (NC)"** — not connected / not configured yet.

## Why the M5 pin tables are only a starting point

M5Stack publishes the CAP pinouts against the **Cardputer-Adv** (an ESP32
host). The numbers there — `G5`, `G13`, `G40`, … — are **ESP32 GPIO numbers**.
The CAP connector is physically the same on the CardputerZero, so the *signal
assignment* (which pad is GDO0, which is BUSY, …) carries over, **but the ESP32
number is not the CM0 Linux kernel offset**. Treat the M5 tables as "which
signal is on which pad", then resolve each signal to a `gpiochip`+offset on the
CM0 with the procedure below.

The bundled defaults (`src/model/config/device_config.h`) preload the M5
reference numbers on `gpiochip0` so you have a concrete starting point — but you
**must confirm them on the device**.

## Step-by-step: resolve a signal to gpiochip + offset

1. **List the controllers**

   ```bash
   gpiodetect
   # e.g. gpiochip0 [<soc>] (54 lines)
   #      gpiochip1 [<expander>] (8 lines)
   ```

2. **Dump a controller's lines** — names hint at what each line is:

   ```bash
   gpioinfo gpiochip0
   # line  23:  "GPIO23"  unused  input  active-high
   ```

   The silkscreen `G#` on the board is **not** guaranteed to equal the kernel
   offset. Use the name/consumer columns, the board schematic, or the CM0
   device-tree to tie a header pin to a line number.

3. **Confirm before trusting it** — with the CAP unplugged, monitor the
   candidate line and plug the CAP / trigger the signal:

   ```bash
   gpiomon gpiochip0 23     # should show edges when the module toggles GDO0
   ```

4. Enter the resolved **chip** + **line** in Config → CC1101 (or edit
   `~/.bruce/pins.conf` directly).

## SPI and UART are kernel devices, not GPIO lines

- **SPI** (SCK/MISO/MOSI/CS) is owned by the kernel SPI driver and exposed as a
  **spidev node** (`/dev/spidev0.0`). You do **not** set SCK/MOSI/MISO/CS as
  GPIO lines here — enable the SPI overlay in the device-tree and point the
  config at the right node. CS/CE is part of the spidev node. Confirm with
  `ls /dev/spidev*`. The CC1101 detection already works over `/dev/spidev0.0`.
- **UART** (the LoRa CAP's GPS) is a **tty node** (`/dev/ttyS0`, `/dev/ttyAMA0`,
  …), configured as device + baud, not as GPIO lines.

Only **one CAP fits the connector at a time**, and the CC1101 and LoRa CAPs
share the same SPI bus + CS, so both default to `/dev/spidev0.0`.

## M5 CAP reference wiring (signal → Cardputer-Adv G#)

Source: [Cap CC1101](https://docs.m5stack.com/en/cap/Cap_CC1101),
[Cap LoRa-1262](https://docs.m5stack.com/en/cap/Cap_LoRa-1262). These are the
**Cardputer-Adv (ESP32)** numbers — the signal column is what carries to the
CardputerZero; resolve the offset per the procedure above.

### CAP CC1101 (Sub-GHz)

| CC1101 signal | Role | Adv G# | Config field |
|---|---|---|---|
| SCLK | SPI clock | G40 | spidev (kernel) |
| MOSI | SPI out | G14 | spidev (kernel) |
| MISO | SPI in | G39 | spidev (kernel) |
| CS | SPI chip-select | G5 | spidev CE (kernel) |
| G0 / **GDO0** | async serial data (RX/TX) | G15 | **GDO0** (aux0) |
| **RF_SW0** | antenna TX/RX switch | G13 | **RF switch** (aux1) |
| GDO2 | (not broken out on this CAP) | — | GDO2 (aux2) = NC |

### CAP LoRa-1262 (SX1262)

| LoRa signal | Role | Adv G# | Config field |
|---|---|---|---|
| SCK | SPI clock | G40 | spidev (kernel) |
| MOSI | SPI out | G14 | spidev (kernel) |
| MISO | SPI in | G39 | spidev (kernel) |
| NSS | SPI chip-select | G5 | spidev CE (kernel) |
| **DIO1/IRQ** | interrupt | G4 | **DIO1/IRQ** (aux0) |
| **RST** | reset | G3 | **RESET** (aux1) |
| **BUSY** | busy | G6 | **BUSY** (aux2) |
| GPS-RX | GPS UART RX | G8 | gps device (ttyS) |
| GPS-TX | GPS UART TX | G9 | gps device (ttyS) |

> LoRa also requires enabling the RF antenna switch by setting **P0 of the
> PI4IOE I²C IO-expander high** during init — that is an I²C expander pin, not a
> host GPIO, and is handled by the LoRa driver when it is ported (not a Config
> field).

## What the driver still needs (bring-up)

- The CC1101 **RF switch** (aux1) must be driven for TX vs RX; the current
  `Cc1101` driver does not yet toggle it — wire it in when validating TX/RX.
- Confirm the spidev node/CE and each GPIO offset on the real device before
  trusting the preloaded defaults. See `src/platform/rf/README.md`.
