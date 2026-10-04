# Hardware probe build

A diagnostic build of Swang Stodva that answers open hardware questions for
the next firmware (Swet102), which targets the same SW102 + stock BBSHD. Flash
it, run the tests below on the bike, photograph the screens, and fill in
[`hw-probe-results.md`](hw-probe-results.md).

Everything is behind the `HW_PROBE` compile-time flag. Without it the firmware
builds **byte-for-byte identical** to a normal build (checked by `cmp` on the
`.hex`), so the probe code can't affect normal releases.

## Building and flashing

The device has DFU **downgrade prevention** and is already at application
version ≥ 200, so the probe needs a date-based `VERSION_NUM` (`yymmddnn`). The
Makefile refuses `HW_PROBE=1` unless `VERSION_NUM` is given on the command line.

```
nix develop
make clean_project
make HW_PROBE=1 VERSION_NUM=26100401 _build/nrf51822_sw102.hex
nrfutil pkg generate \
  --application _build/nrf51822_sw102.hex \
  --key-file prebuilt/private.key \
  --application-version 26100401 \
  --hw-version 51 --sd-req 0x87 \
  _release/swang-stodva-hwprobe-26100401.zip
```

(`nrfutil pkg generate` here is pc-nrfutil 6.1.7, which is the same packager
as `nrfutil nrf5sdk-tools pkg generate`; the flags are the same.)

Send the zip over BLE with nRF Connect / nRF Toolbox DFU, like a normal OTA
update. **After the DFU, power off and long-press PWR**, or the bootloader stays
in `SW102_DFU`.

After the probe, every later normal release also needs `VERSION_NUM` above
the probe's number, or the bootloader will refuse it.

## Using it

Long-press M on the main screen to open settings. **HW probe** is the first
entry and opens a list of 11 pages. Pages use a 3×5 pixel font: 16 columns ×
18 rows, all caps.

Controls on every page:

- **PWR click**: back to the probe list
- **UP / DOWN click**: previous / next screen, on pages longer than one screen
  (a scrollbar appears on the right edge)
- page-specific keys are printed on the page and listed below

Photograph every screen of a page (scroll with DOWN).

Normal behavior is unchanged: the motor polling, BLE and power management keep
running. The exceptions are pages 6, 7, 9 and 11, and only while they're open.

## Pages

### 1. Chip ID — FICR / UICR

Raw register values in hex, plus computed lines at the top:

| Line | Meaning |
|---|---|
| `RAM KB` | sum of `SIZERAMBLOCK[i]` for `i < NUMRAMBLOCK` |
| `FLASH KB` | `CODEPAGESIZE × CODESIZE / 1024` |
| `VARIANT` | `INFO.VARIANT` decoded as 4 ASCII characters (e.g. `AAC0`) |
| `HWID` | low 16 bits of `CONFIGID`. Look it up in the nRF51 compatibility matrix |

Register abbreviations: `PGSIZE`=CODEPAGESIZE, `CODESZ`=CODESIZE, `CLENR0`,
`PPFC`, `NRAMBLK`=NUMRAMBLOCK, `RAMBLK0..3`=SIZERAMBLOCK[0..3] (`RAMBLK0` is
also the legacy SIZERAMBLOCKS), `CFGID`=CONFIGID, `DEVID0/1`=DEVICEID,
`ADDRTYP`=DEVICEADDRTYPE, `DEVADR0/1`=DEVICEADDR, `PART`, `VARIANT`, `PACKAGE`,
`INFORAM`=INFO.RAM, `INFOFLS`=INFO.FLASH, `BLADDR`=UICR.BOOTLOADERADDR. The
INFO registers may read `FFFFFFFF` on old silicon. They're shown as-is.

*Why:* earlier notes say QFAA (normally 16 KB RAM), but this firmware links for
32 KB and runs.

**Procedure:** open the page, photograph both screens.

**SWD cross-check (no firmware needed):** with the WCH-LinkE (or ST-Link)
connected, the same values can be read with OpenOCD:

```
openocd -f interface/<probe>.cfg -f target/nrf51.cfg \
  -c "init; halt; mdw 0x10000000 0x48; mdw 0x10000100 5; mdw 0x10001014 1; resume; shutdown"
```

### 2. RAM map

| Line | Meaning |
|---|---|
| `DATA` … `STKTOP` | linker symbols `__data_start__`, `__bss_end__`, `__HeapBase`, `__HeapLimit`, `__StackLimit`, `__StackTop` |
| `SP NOW` | stack pointer while drawing the page |
| `GAP B` | unused bytes between `__HeapLimit` and `__StackLimit` |
| `SD RAM BASE: IN` | app RAM base passed to `sd_ble_enable()` (= `__data_start__`) |
| `MIN` | minimum app RAM base the SoftDevice returned for the current BLE config |
| `ERR` | `sd_ble_enable()` return code (0 = success, 4 = NO_MEM) |
| `STACK: SIZE / USED / FREE` | stack high-water mark |

The startup code already fills the whole stack with `0xDEADBEEF` before
`main()`. That fill is used as the paint pattern, so `USED` is the deepest the
stack has ever gone since boot.

**Procedure:** ride or use the UI for 5+ minutes with a phone connected over
BLE, then open the page and photograph it.

### 3. Frame time

TIMER1 runs free at 1 MHz from boot (16-bit hardware counter extended to 32
bits in its overflow interrupt). Each 20 ms UI tick is timed:

| Line | Meaning |
|---|---|
| `UI AVG / MAX` | `ui_update()` minus the time spent in `lcd_refresh()`, µs |
| `LCD AVG / MAX` | one `lcd_refresh()` (blocking SPI flush), µs |
| `TOT MAX` | longest whole `ui_update()` including the flush, µs |
| `FRAMES`, `SECS` | ticks measured since reset (50 per second) |
| `>20MS` | ticks whose total exceeded the 20 ms tick period |
| `MISSED` | the main loop's existing missed-tick counter, since reset |

Interrupts (motor UART round-robin, BLE) that land inside a tick are counted in
its time.

**Keys:** UP+DOWN together, or M, reset the stats.

Timing runs on every screen, not only while this page is open.

**Procedure:** press M here to reset, go back to the main screen (PWR ×3),
ride for 1 minute (or spin the wheel on a stand), then long-press M →
HW probe → Frame time and photograph it. The few menu frames on the way back
barely affect the averages, but they can set the max values.

### 4. Flash FDS

**M** runs `eeprom_write_variables()` and times it end to end (FDS GC + record
update + waiting for the write to complete).

| Line | Meaning |
|---|---|
| `LAST MS`, `MAX MS`, `WRITES` | duration of the last / longest write, write count |
| `STAT ERR` | `fds_stat()` return code |
| `OPEN`, `VALID`, `DIRTY` | record counts |
| `RESERVD`, `USED W`, `LARGEST`, `FREEABL` | words reserved / used, largest contiguous free, reclaimable by GC |

**Procedure:** press M 5–10 times and photograph. Note the first write and the
max separately.

### 5. Motor raw

One row per polled opcode: name, the last raw reply bytes in hex (`--` if
never received), and the reply count. `ST`=0x08, `CUR`=0x0A, `BRK`=0x0F,
`BAT`=0x11, `SPD`=0x20, `RNG`=0x22, `CAL`=0x24, `MOV`=0x31. With
Motor → Firmware = stock, `RNG` and `CAL` aren't polled. With bbs-fw, `BRK`
isn't polled.

Then the decoded values: `CUR A` (amps the firmware derives: raw / 2),
`CUR X5` (the ×5 value the power calculation uses), `RPM` and `RPM MAX`, and
`STATUS HIST`: every distinct STATUS byte seen since boot as `value: count`
(up to 12, then an `OVFL` counter).

**Procedure:** bike on a stand.
1. Note the idle values.
2. Pedal or throttle under assist and note `CUR` raw/A at a known load.
3. Briefly unplug the speed sensor, then the brake sensor, and note any new
   STATUS values. Reconnect them.
4. Photograph both screens.

### 6. Speed limit (stand test)

Sends `16 1F hi lo checksum` with the selected encoding.

| Key | Action |
|---|---|
| UP | value: 15 → 20 → 25 → 30 km/h |
| DOWN | encoding: `KMH X10` (current behavior) ↔ `WHEEL RPM` |
| M | send now |
| PWR | back. If anything was sent, the configured limit is restored (km/h × 10, as normal boot sends it) |

`WHEEL RPM` = km/h × 1000 / 60 / perimeter_m, using the configured wheel
perimeter (`PERIM`, 2165 mm on this bike). `WIRE` is the 16-bit value, `TX` the
exact 5 bytes last sent. Live `SPD`, `RPM` and `PAS` are shown.

**Procedure:** rear wheel on a stand, PAS 9 (set before entering the menu).
For each encoding × {20, 25}: select, press M, pedal/throttle up, and note the
speed where assist cuts out. *Why:* bbs-fw decodes this field as wheel RPM,
while Swang Stodva sends km/h × 10. Stock firmware behavior is the question.

### 7. Walk assist

| Key | Action |
|---|---|
| M | mode: `ONCE` ↔ `REPEAT 500MS` (only while DOWN is released) |
| hold DOWN | walk assist on while held |

`ONCE` is today's behavior: PAS code `0x06` is sent once on press, and the
normal PAS level on release. `REPEAT` also re-sends `16 0B 06 27` every 500 ms
while held. The page shows `TIME` (seconds held, or the last hold's duration),
`STOP` (time at which the wheel stopped after it had been turning, `--` if it
didn't), live speed, rpm, and `RESENDS`.

**Procedure:** on a stand, hold DOWN for 30 s in each mode and note whether
and when the motor stops.

### 8. DFU entry

Shows `GPREGRET BOOT` (GPREGRET read at the very start of `main()`, before
anything clears it), `VER NUM` (this build's DFU version) and whether the
SoftDevice is in use.

**M** writes `GPREGRET = 0xB1` (`BOOTLOADER_DFU_START`) via
`sd_power_gpregret_set()` and resets with `sd_nvic_SystemReset()`.

**Procedure:** press M, then check on the phone whether `SW102_DFU` is
advertising. To get back: power off, then long-press PWR. If the app comes
straight back instead, open this page again and note `GPREGRET BOOT`.

### 9. Landscape

Cycles the SH1107 **segment remap** (`A0`/`A1`) × **COM scan direction**
(`C0`/`C8`) with **M**, sending the commands live. The framebuffer is filled as
a landscape `fb[64 rows][16 bytes]`, row-major, bit 0 = leftmost pixel, with:

- `TOP` and an arrow pointing up, at the top centre
- one lit pixel at landscape (0,0) and a 3×3 block at (127,63)
- the combination name (e.g. `A1 C8`) bottom-left, `M:NEXT` bottom-right

Leaving the page (PWR) restores the normal `A0 C0`.

**Procedure:** hold the display as mounted on the bars (buttons on the left,
PWR on the right). For each combination, note whether the arrow points up, the
text reads normally, and (0,0) is top-left. Photograph all four.

### 10. BLE addr

`FICR TYPE` (DEVICEADDRTYPE bit 0, 1 = random), `FICR` (the address derived
from DEVICEADDR with the two static-random top bits set), and the address from
`sd_ble_gap_address_get()` with its type (`RND STATIC` expected), then `MATCH`.
Addresses are printed MSB first, as phones show them.

*Why:* the new firmware relies on a static address that the phone can remember
across reboots.

**Procedure:** photograph. Optionally compare with the address the phone shows.

### 11. Btn bounce

While the page is open, the four button pins are sampled at 1 kHz from a
TIMER1 compare interrupt. A burst of edges ends after 30 ms without one.

| Column | Meaning |
|---|---|
| `MX` | most edges seen in one press or release (1 = clean) |
| `LS` | edges in the last press or release |
| `MS` | longest time from the first to the last edge of a burst, ms |
| `N` | presses + releases counted |

**Procedure:** press each of UP, DOWN and M 20× at different speeds and
pressures, then photograph. PWR can't be tested here: a click leaves the page
and a long press powers off. For reference, the current firmware samples
the buttons once per 20 ms UI tick (`buttons_clock()`).

## Implementation notes

- `src/hw_probe.c` holds all pages. The hooks into `main.c`, `lcd.c`,
  `ble_services.c`, `state.c` and `screen_cfg_tree.c` are `#ifdef HW_PROBE`.
- Probe UART frames (pages 6 and 7) go through a one-slot queue in
  `state.c`, which sends them from the `communications()` round-robin ahead of
  the next READ. They never interleave with a request or reply.
- TIMER1 interrupt priority is 3 (APP_LOW), set before the SoftDevice is
  enabled.
