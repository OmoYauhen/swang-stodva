# Hardware probe results

Filled in from photos of the probe build (see [`hw-probe.md`](hw-probe.md) for
what each page shows and how to run each test).

- Probe build: `VERSION_NUM` = ______  commit ______
- Date: ______
- Device: SW102 serial / notes: ______
- Motor: BBSHD, firmware: stock / bbs-fw

## 1. Chip ID (FICR / UICR)

| Item | Value |
|---|---|
| RAM KB (computed) | |
| FLASH KB (computed) | |
| VARIANT (ASCII) | |
| HWID (CONFIGID & 0xFFFF) | |
| CODEPAGESIZE | |
| CODESIZE | |
| CLENR0 | |
| PPFC | |
| NUMRAMBLOCK | |
| SIZERAMBLOCK[0..3] | |
| CONFIGID | |
| DEVICEID[0..1] | |
| DEVICEADDRTYPE | |
| DEVICEADDR[0..1] | |
| INFO.PART | |
| INFO.VARIANT | |
| INFO.PACKAGE | |
| INFO.RAM | |
| INFO.FLASH | |
| UICR.BOOTLOADERADDR | |
| SWD cross-check matches? | |

**Conclusion — real RAM size:** ______

## 2. RAM map

| Item | Value |
|---|---|
| `__data_start__` | |
| `__bss_end__` | |
| `__HeapBase` / `__HeapLimit` | |
| `__StackLimit` / `__StackTop` | |
| Gap heap→stack (bytes) | |
| SoftDevice app RAM base: passed in | |
| SoftDevice app RAM base: minimum returned | |
| `sd_ble_enable` err | |
| Stack size / used / free (bytes) | |
| Conditions (minutes, BLE connected?) | |

## 3. Frame timing

Conditions: ______ (main screen, riding / stand, duration)

| Item | Value |
|---|---|
| UI avg / max (µs) | |
| LCD flush avg / max (µs) | |
| Total max (µs) | |
| Frames / seconds | |
| Ticks > 20 ms | |
| Missed ticks | |

## 4. Flash (FDS) timing

| Item | Value |
|---|---|
| Writes performed | |
| First write (ms) | |
| Last / max (ms) | |
| fds_stat err | |
| Open / valid / dirty records | |
| Words reserved / used | |
| Largest contiguous / freeable words | |

## 5. Motor raw values

| Opcode | Raw bytes (idle) | Raw bytes (under load) | Notes |
|---|---|---|---|
| STATUS 0x08 | | | |
| CURRENT 0x0A | | | |
| BRAKE 0x0F | | | |
| BATTERY 0x11 | | | |
| SPEED 0x20 | | | |
| MOVING 0x31 | | | |

Current scale check: raw ____ → firmware A ____ ; measured/expected A ____

RPM max seen: ____

STATUS histogram:

| Value | Count | When it appeared (normal / braking / speed sensor unplugged / brake sensor unplugged / …) |
|---|---|---|
| | | |
| | | |
| | | |

## 6. Speed-limit encoding (stand, PAS 9)

| Encoding | Set km/h | Wire value | Bytes sent | Assist cut-out speed (km/h) |
|---|---|---|---|---|
| km/h × 10 | 20 | 200 | | |
| km/h × 10 | 25 | 250 | | |
| wheel RPM | 20 | | | |
| wheel RPM | 25 | | | |

**Conclusion — encoding the stock BBSHD honors:** ______

## 7. Walk-assist keep-alive (stand, 30 s hold)

| Mode | Motor stopped? | Stop time (s) | Resends | Notes |
|---|---|---|---|---|
| Once | | | — | |
| Repeat 500 ms | | | | |

## 8. DFU entry via GPREGRET

| Item | Value |
|---|---|
| GPREGRET at normal boot | |
| After "Reboot to DFU": phone sees `SW102_DFU`? | |
| How it got back to the app | |
| GPREGRET at boot after returning | |

## 9. Display landscape orientation

Held as mounted (buttons left, PWR right):

| Combination | Arrow up? | Text readable? | (0,0) top-left? |
|---|---|---|---|
| A0 C0 | | | |
| A1 C0 | | | |
| A0 C8 | | | |
| A1 C8 | | | |

**Conclusion — combination to use:** ______

## 10. BLE identity

| Item | Value |
|---|---|
| FICR DEVICEADDRTYPE | |
| Address from FICR | |
| `sd_ble_gap_address_get` address | |
| Address type | |
| Match | |
| Address shown by phone | |

## 11. Button bounce

| Button | Max edges | Max bounce span (ms) | Presses+releases |
|---|---|---|---|
| UP | | | |
| DOWN | | | |
| M | | | |
| PWR | n/a | n/a | n/a |

**Conclusion — debounce window:** ______
