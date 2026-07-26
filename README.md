# Swang Stodva

**Open-source Bafang display firmware for the SW102 handlebar display.**

![status: alpha](https://img.shields.io/badge/status-alpha-orange?style=for-the-badge)

> [!WARNING]
> ## ⚠️ Alpha software — not recommended for daily use
>
> This firmware is in **early alpha**: under active development and only
> lightly tested on real hardware, with known rough edges (see below). Expect
> bugs and breaking changes — **don't rely on it for a bike you depend on.**
k
This is a fork of [anszom/SW102_LCD](https://github.com/anszom/SW102_LCD),
ported from the **TSDZ2** motor's UART protocol to the **Bafang** display
UART protocol used by BBS02 / BBSHD mid-drive motors. It runs on the
Bafang SW102 display and speaks the same wire protocol as a stock Bafang display, so it works
against either stock Bafang firmware or [`bbs-fw`](https://github.com/danielnilsson9/bbs-fw)
(the open-source app-MCU replacement).

Upstream lineage: `casainho/Color_LCD` → `anszom/SW102_LCD` (new UI) →
this fork (Bafang wire protocol).

Features:

- runs on the Bafang SW102 handlebar display
- confirmed running on real hardware: SW102 display driving a Bafang BBSHD (HD) mid-drive motor
- talks the Bafang display UART protocol (1200 baud, `0x11`/`0x16` category bytes)
- polls the motor for status, current, battery, speed, temperature, voltage, moving state
- writes assist level (PAS), lights, and walk-assist state back to the motor
- built-in support for `bbs-fw`'s field hijacks (motor temperature via `READ_RANGE`, battery voltage ×10 via `READ_CALORIES`)
- new UI with smooth graphics, graphs, and 50 fps update rate
- terminal emulator (Rust + ratatui) for developing without hardware — see [`docs/BUILD.md`](docs/BUILD.md)
- assist level defined as percentage (100 % = motor matches rider power)

Known issues / rough edges:

- pedal cadence is stubbed at 99 — BBSHD doesn't report RPM in its display protocol, still deciding how to synthesise or hide it
- BT connectivity hasn't been tested
- real-hardware testing is still limited — it runs on a real BBSHD, but only lightly exercised so far (hence: alpha)
- startup boost menu removed (never worked, per casainho's wiki)
- virtual throttle removed

## Installation

If you have already installed casainho's SW102 software & bootloader, you can
switch to this version and back via bluetooth. See the
[wiki page](https://github.com/OpenSourceEBike/TSDZ2_wiki/wiki/Flash-the-firmware-on-SW102)
for flashing instructions. OTA update binaries are available on the
[releases page](releases/).

If you have the factory SW102 software installed, you will need to disassemble
the device [as described here](https://github.com/OpenSourceEBike/TSDZ2_wiki/wiki/Flash-the-bootloader-and-firmware-on-SW102-using-SWD),
and flash the bootloader. Afterwards you can switch to this version using the
procedure described above.

To build from source, see [docs/BUILD.md](docs/BUILD.md)
for instructions (Nix + `nix build`/`nix run` for reproducible builds, or a
system Rust + `arm-none-eabi` toolchain).

## Usage

The main screen is explained in the video above. The following elements are
visible:

- left bar: assist level (0..9 — Bafang has nine discrete PAS levels)
- right bar: instantaneous motor power
- top: battery icon & percentage (read directly from the motor)
- central large display: speed
- central small display: extra information (odometer, trip distance, trip
  time, average speed, motor power)
- bottom indicators: WALK (walk assist), BRK (braking), lights

The following key actions are available:

- **UP/DOWN**: adjust assist level
- **ESC**: toggle lights
- hold **ESC**: turn off
- **ENTER**: cycle extra information display
- hold **ENTER**: enter configuration menu
- hold **UP**: toggle Street Mode (if enabled in configuration)
- hold **DOWN**: Walk Assist (if enabled in configuration)

In the configuration screen, you use UP/DOWN for navigation, ENTER to accept,
and ESC to go back.

## Compatibility

### Motor firmware

- **Bafang stock BBS02 / BBSHD firmware** — should work; the display speaks
  the same protocol Bafang's own displays use.
- **`bbs-fw` on BBS02 / BBSHD** — designed against this codebase; supports
  the field hijacks (motor temperature via `READ_RANGE`, battery voltage via
  `READ_CALORIES`) that `bbs-fw` implements.
- **TSDZ2** — **not compatible**. This fork replaces the TSDZ2 wire protocol
  entirely (baud, framing, handshake, direction of first packet all differ).
  Use [upstream anszom/SW102_LCD](https://github.com/anszom/SW102_LCD) if
  you're on TSDZ2.

### Config format compatibility

Note that the EEPROM-persisted config format is inherited from upstream and
has not been reshaped for BBSHD yet. Some fields (torque-sensor calibration
table, TSDZ2-specific tunables) are still present but do nothing on BBSHD.
Cleaning this up is on the TODO list.

### Assist level definition

Assist level is defined as a percentage: 100% means the motor gives roughly
the same power as the rider, 50% half, 200% double. The display remaps its
level number (0..9) to Bafang's non-monotonic wire codes on the fly
(`0x00 0x01 0x0B 0x0C 0x0D 0x02 0x15 0x16 0x17 0x03`).

## Development

### Source changes relative to the anszom/SW102_LCD fork

The "backend" changes concentrate in `common/src/state.c` and
`common/include/uart.h`:

- new Bafang RX byte-level state machine (`uart_prime_rx(len)` API,
  caller-declared reply length, no CRC — checksums are opcode-specific)
- new TX round-robin over seven READ opcodes at ~100 ms/opcode
- WRITE_PAS / WRITE_LIGHTS with change-detection guards (writes only on
  user-initiated state changes)
- field mapping from parsed Bafang values → `rt_vars` / `ui8_g_battery_soc`

The UI, screen framework, and blitting code are inherited from
`anszom/SW102_LCD` unchanged.

### Emulator

A terminal emulator ([`emu-rs/`](emu-rs), Rust + ratatui) compiles the real
firmware C and runs it on the desktop — rendering the OLED to the terminal and
bridging the motor UART to a pty — for a much quicker development cycle.

```
nix run .#emu           # or: cd emu-rs && cargo run
```

For development without a real motor, use the included BBSHD motor emulator
(Python 3, no dependencies):

```
python3 -u tools/bbshd_mock.py --verbose --speed 20 --batt 60
# prints e.g. "Slave device: /dev/pts/N"

# in another shell:
cd emu-rs && cargo run -- --motor-port=/dev/pts/N
```

See [`docs/BUILD.md`](docs/BUILD.md) for controls/headless mode and
[`tools/BBSHD_MOCK.md`](tools/BBSHD_MOCK.md) for the mock's protocol coverage.
