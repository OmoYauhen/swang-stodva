# BBSHD motor emulator

`bbshd_mock.py` is a Python script that emulates a Bafang BBSHD motor
controller speaking the Bafang display UART protocol. Together with the
emulator's `--motor-port` flag, it lets you run the SW102 firmware emulator
against a synthetic BBSHD entirely on your desktop — no motor, no display, no
cable.

Reference implementation: `bbs-fw` (`src/firmware/extcom.c`) from
<https://github.com/danielnilsson9/bbs-fw>.

## Usage

Terminal 1 — start the mock:

```
python3 -u tools/bbshd_mock.py --verbose --speed 18 --batt 78
```

It prints the pty slave device it created, e.g. `/dev/pts/5`.

Terminal 2 — run the emulator pointed at that path:

```
cd emu-rs && cargo run -- --motor-port=/dev/pts/5
```

Options:

| Flag | Default | Purpose |
|------|--------:|---------|
| `--speed KPH`  | `0`  | Fixed fake wheel speed (ignored if `--speed-wave` is set) |
| `--speed-wave MODE` | off | Sweep speed over time to test the speed UI: `sine`, `triangle`, or `sawtooth` |
| `--speed-min KPH` | `0`  | Low end of the `--speed-wave` sweep |
| `--speed-max KPH` | `45` | High end of the `--speed-wave` sweep |
| `--speed-period S` | `20` | Seconds for one full `--speed-wave` cycle |
| `--batt PCT`   | `90` | Initial fake battery percent |
| `--baud N`     | `1200` | UART baud (Bafang is 1200) |
| `--verbose`    | off  | Log every packet, both directions |

To exercise the speed display, sweep it instead of pinning it:

```
python3 -u tools/bbshd_mock.py --speed-wave sine --speed-min 0 --speed-max 45 --speed-period 20
```

## Protocol coverage

Implements every opcode `bbs-fw` handles for a Bafang display:

- **Reads:** STATUS `0x08`, CURRENT `0x0A`, BATTERY `0x11`, SPEED `0x20`,
  UNKNOWN1 `0x21`, RANGE `0x22`, CALORIES `0x24`, UNKNOWN3 `0x25`,
  MOVING `0x31`.
- **Writes:** PAS `0x0B`, MODE `0x0C`, LIGHTS `0x1A`, SPEED_LIM `0x1F`.
- **Non-monotonic Bafang PAS-level encoding** (see `bbshd.md` §3.2 in the
  PET project docs, or `extcom.c` `process_bafang_display_write_pas`).
- **bbs-fw's "field hijacks":** the CALORIES field returns battery
  voltage × 10; the RANGE field returns motor temperature.
- **Weird checksums:** the READ_SPEED reply uses `(sum + 0x20) & 0xFF`;
  degenerate 1-byte checksums for CURRENT/BATTERY/MOVING.

## Reactive state

The mock isn't fully static — every tick it:

- Slowly drops the battery percent and voltage.
- Oscillates the motor current so the RANGE field changes on each read.
- Reports "moving" when speed > 0.
- Optionally sweeps the wheel speed across `[--speed-min, --speed-max]`
  when `--speed-wave` is given, so the SPEED field changes every read.

Enough motion to prove the display's UI is actually reacting to bytes on
the wire, without needing to model actual physics.

## Sending real Bafang packets by hand

To test the mock outside the emulator, connect any UART tool to the pty
slave path it prints and send bytes yourself. Example (Python):

```python
import os, time
fd = os.open("/dev/pts/5", os.O_RDWR | os.O_NOCTTY)
os.write(fd, bytes([0x11, 0x11]))   # READ_BATTERY
time.sleep(0.1)
print(os.read(fd, 2).hex(" "))       # -> "4d 4d" for 77%
```

Or `socat` to bridge the pty to a physical UART for talking to a real
display:

```
socat -d -d /dev/pts/5 /dev/ttyUSB0,b1200,raw,echo=0
```
