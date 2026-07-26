# Building swang-stodva

This is a fork of [anszom/SW102_LCD](https://github.com/anszom/SW102_LCD)
ported from the TSDZ2 wire protocol to the Bafang display UART protocol used
by BBS02 / BBSHD mid-drive motors. See the top-level [README](../README.md)
for lineage, compatibility notes, and feature list.

Most of the display-side settings are still compatible with upstream
`anszom/SW102_LCD` (and by extension `casainho/Color_LCD`), so these wiki
pages remain a useful reference for the UI / configuration menus, though
they describe a different motor-side protocol:
- https://github.com/OpenSource-EBike-firmware/Color_LCD/wiki/Bafang-LCD-SW102
- https://github.com/OpenSource-EBike-firmware/SW102_LCD_Bluetooth/wiki

## Nix flake (reproducible)

With flakes enabled, `flake.lock` pins nixpkgs so everyone gets the identical
toolchain:

```
nix develop            # dev shell with both toolchains (emu + firmware)
nix build .#emu        # -> result/bin/swang-stodva-emu
nix build .#firmware   # -> result/nrf51822_sw102.hex
nix run  .#emu         # build and launch the emulator
```

`nix-shell` still works too (it uses your channel's nixpkgs rather than the
lock, so it isn't pinned). The flake's dev shell reuses `shell.nix`, so both
entry points give the same environment.

## Building the on-target firmware

The `Makefile` cross-compiles the firmware
for the SW102's nRF51x22 (Cortex-M0) and produces `_build/nrf51822_sw102.hex`,
suitable for flashing via SWD (ST-Link + OpenOCD) or for wrapping into a
signed OTA DFU zip.

### Toolchain

You need `arm-none-eabi-gcc` and `arm-none-eabi-newlib`. Modern versions
work fine — this fork has been built with GCC 15.2 (nixpkgs
`gcc-arm-embedded`); the upstream README's ancient 4.9/2015q3 pin is no
longer required.

* **NixOS / Nix (any platform)**:
  ```
  nix shell nixpkgs#gcc-arm-embedded nixpkgs#gnumake nixpkgs#python3
  export GNU_INSTALL_ROOT=$(dirname $(dirname $(which arm-none-eabi-gcc)))
  ```
* **Debian / Ubuntu**: `sudo apt install gcc-arm-none-eabi python3 make`
* **Fedora**: `sudo dnf install arm-none-eabi-gcc-cs arm-none-eabi-newlib python3 make`
* **macOS + Homebrew**: `brew install --cask gcc-arm-embedded && brew install python3`

### Building `.hex`

```
make -f Makefile clean_project
make -f Makefile _build/nrf51822_sw102.hex
```

Expected size on the current `main`: roughly 48 KB `.text` + 300 B `.data`
+ 6 KB `.bss`, comfortably within the nRF51x22's 256 KB flash / 16 KB RAM.
Warnings from the newlib stubs (`_close is not implemented`) are benign —
those syscalls are unused after link-time garbage collection.

### Packaging the DFU (OTA) zip

Wraps the `.hex` into a signed Nordic DFU package that can be delivered
over BLE to a running bootloader. Uses Nordic's current `nrfutil` (Rust
rewrite, v8.x) with the `nrf5sdk-tools` subcommand — the same CLI shape
the SDK 12.3 Makefile expected. `prebuilt/private.key` is
the signing key checked into the repo.

**On NixOS**, `nrfutil` requires accepting the unfree Segger JLink
license, and the Nordic subcommand binaries are generic-linux ELFs that
need an FHS environment. Both are handled here:

```
mkdir -p _release
nix-shell --impure \
  --arg config '{ allowUnfree = true; segger-jlink.acceptLicense = true; }' \
  -p nrfutil steam-run --run '
    # one-time: install the nrf5sdk-tools subcommand into ~/.nrfutil
    steam-run nrfutil install nrf5sdk-tools

    # then package the zip
    steam-run nrfutil nrf5sdk-tools pkg generate \
      --application _build/nrf51822_sw102.hex \
      --key-file prebuilt/private.key \
      --application-version 27 \
      --hw-version 51 \
      --sd-req 0x87 \
      _release/swang-stodva-otaupdate-$(git rev-parse --short HEAD).zip
  '
```

**On non-NixOS Linux** (Debian/Ubuntu/Fedora), install `nrfutil` from
Nordic's release page, then drop the `steam-run` wrapper:

```
nrfutil install nrf5sdk-tools
nrfutil nrf5sdk-tools pkg generate \
  --application _build/nrf51822_sw102.hex \
  --key-file prebuilt/private.key \
  --application-version 27 \
  --hw-version 51 \
  --sd-req 0x87 \
  _release/swang-stodva-otaupdate-$(git rev-parse --short HEAD).zip
```

Fields:

- `--application-version` — **bump this for every release** you flash. The
  bootloader rejects packages whose version is not strictly greater than
  what's currently installed unless a debug mode is set. Track it
  alongside `VERSION_STRING` in `../common/Makefile.common`.
- `--hw-version 51` — nRF51 family.
- `--sd-req 0x87` — CRC of SoftDevice s130 2.0.1 (matches
  `nRF5_SDK_12.3.0/components/softdevice/s130/hex/`). Change this only if
  you rebuild against a different SoftDevice.
- `--key-file` — must match the public key baked into the installed
  bootloader. The one in `prebuilt/private.key` pairs with the prebuilt
  bootloader in the same directory.

The resulting zip contains `manifest.json`, `nrf51822_sw102.dat` (init
packet), and `nrf51822_sw102.bin` — deliverable to a bootloader via
`nrfutil dfu ble ...` or any BLE DFU app (see the "Debugging bluetooth
linux" section below).

## Releasing (GitHub Actions)

Releases are cut by the manual **Release** workflow
(`.github/workflows/release.yml`) — run it from the repo's Actions tab, or
`gh workflow run release.yml -f release_type=prerelease`. It:

1. **bumps the version** (`tools/bump-version.py`; `release_type` = `prerelease`
   / `finalize` / `patch` / `minor` / `major`, with a `prerelease_label`),
   updating `common/Makefile.common` — both `VERSION_STRING` (SemVer, shown in
   the UI) and the monotonic `VERSION_NUM` (DFU bootloader gate, +1 each time);
2. **builds** the `.hex` reproducibly via `nix build .#firmware`;
3. **packages** three flashable artifacts plus `SHA256SUMS`:
   - `swang-stodva-app-<ver>.hex` — application only (SWD, when bootloader +
     SoftDevice are already present)
   - `swang-stodva-full-<ver>.hex` — bootloader + SoftDevice + app + settings
     (SWD onto a blank/erased device)
   - `swang-stodva-otaupdate-<ver>.zip` — signed BLE DFU package
4. **commits** the bump to `main`, tags `v<ver>`, and **publishes** a GitHub
   Release with auto-generated notes (marked pre-release when the version has a
   `-alpha` / `-beta` / `-rc` suffix).

Current version: `0.0.1-alpha`. The workflow uses the default `GITHUB_TOKEN`
(`contents: write`) and pushes the bump commit to `main`, so `main` must be
unprotected (or swap in a PAT).

## Running the terminal emulator

The emulator is a Rust/[ratatui](https://ratatui.rs) crate in
[`emu-rs/`](../emu-rs) that **compiles the real firmware C** (UI, screens,
state machine, Bafang protocol) via a `build.rs` and drives it: it renders the
64×128 OLED framebuffer to the terminal (Unicode braille), maps keys to the
four buttons, and bridges the motor UART to a pty/serial port. This lets you
develop the UI and protocol code without flashing the SW102. The desktop HAL
shims that replace the nRF51 peripherals live in `emu-rs/csrc/` and `src/emu/`.

### With Nix

```
nix run .#emu            # build + run
# or a dev shell:
nix develop              # brings in cargo, rustc, gcc, python3 (+ arm toolchain)
cd emu-rs && cargo run
```

### Without Nix

Needs a Rust toolchain (`rustc` + `cargo`, e.g. via rustup) and a C compiler:

```
cd emu-rs
cargo run
```

### Motor

Three ways to feed the emulator motor data:

1. **Built-in BBSHD (default).** With no `--motor-port`, the emulator emulates a
   BBSHD in-process and shows a **BBSHD motor** panel to the right of the display
   with its live state (PAS, lights, speed, battery, moving, temp, current). Set
   the initial speed/voltage with `EMU_MOTOR_SPEED` (km/h) and `EMU_MOTOR_BATTERY`
   (volts), e.g. `EMU_MOTOR_SPEED=18 cargo run`.

2. **Real motor.** Plug a Bafang programming cable into your motor and your PC:
   `cargo run -- --motor-port=/dev/ttyUSB0`.

3. **External Python mock.** Run `tools/bbshd_mock.py` and point the emu at its
   pty (`--motor-port=/dev/pts/N`). See `tools/BBSHD_MOCK.md` for its options.

4. **Userspace CH340 (Android / no `/dev/ttyUSB`).** With `--features usb`, the
   emu can drive a CH340 adapter (`1a86:7523`) itself via libusb — no kernel
   `ch341-uart` driver, no `/dev/ttyUSB*` node. This is what makes it run on an
   Android phone with no root (see below). On desktop Linux you can exercise the
   same path with `--motor-usb` (opens the adapter by VID:PID; needs root or a
   udev rule since the raw USB node is root-only).

### On Android (Termux, no root)

The Bafang UART is only 1200 baud, so a phone + USB-C OTG + CH340 adapter is
enough. Android won't expose a `/dev/ttyUSB*`, so the emu talks to the adapter
in userspace over a file descriptor handed to it by `termux-usb` (which drives
the Android USB-host permission dialog — the same mechanism commercial Bafang
apps use).

```
pkg install rust clang git libusb termux-api      # + install the Termux:API app
git clone https://github.com/OmoYauhen/swang-stodva
cd swang-stodva/emu-rs
cargo build --release --features usb

termux-usb -l                                      # find the adapter, e.g. /dev/bus/usb/001/002
# run the emu with the granted fd (termux-usb passes it as $1):
termux-usb -r -e 'sh -c "./target/release/swang-stodva-emu --motor-fd=$1"' /dev/bus/usb/001/002
```

Wire the adapter to the motor's display port: adapter **TX → motor RX**, **RX →
motor TX**, **GND ↔ GND**; leave the 5 V display-power wire disconnected. The
CH340's data lines must tolerate the Bafang port's 5 V logic (use a 5 V adapter
or a level shifter). The motor must be powered on. Test with the rear wheel off
the ground — the emu applies real PAS/lights to the motor.

### Controls & extras

- Display keys: `↑ / ↓` assist · `Enter` (or `m`) menu · `Esc` (or `p`) power ·
  `Ctrl-C` quit. The bottom border shows the motor-connection dot; the button
  chips highlight while held.
- Built-in motor keys: `←` / `→` change speed, `-` / `+` change battery voltage.
  At 0 km/h the motor reports not-moving.
- `EMU_HEADLESS=1` runs without a terminal (ticks for `EMU_HEADLESS_MS`, default
  4000, then dumps the framebuffer as ASCII) — handy for CI / scripted checks.

## Debugging bluetooth linux

### Using a NRF dongle

Use https://www.nordicsemi.com/Software-and-Tools/Development-Tools/nRF-Connect-for-desktop
Install this https://github.com/NordicSemiconductor/nrf-udev

Use this command to BLE update a target:
nrfutil dfu ble -ic NRF52 -p /dev/ttyACM0 --help

### Using a regular BLE dongle

Install this fork of nrfutil https://github.com/anszom/pc-nrfutil

Use this command to BLE update a target:
nrfutil  dfu ble-native -pkg swang-stodva-otaupdate-xxx.zip  -a (your target BLE address)

### Post-installation

Note that the bootloader used in the open-source firmware has an (issue)[https://github.com/OpenSourceEBike/SW102_LCD_Bluetooth-bootloader/pull/3] which was only recently fixed. In order to avoid problems, when activating the display *for the first time after flashing* you may need to hold the power button for a long time (up to 10 seconds) until you see the boot animation. Otherwise, the bootloader's processing may be interrupted and the SW102 will return to DFU mode. In this case, please re-run the DFU procedure.
