#!/usr/bin/env python3
"""
Bafang UART brake probe.

The Bafang display protocol (as reverse-engineered in bbs-fw / state.c) has NO
documented brake field: the display is master and only polls a fixed set of READ
opcodes, none of which is known to carry brake state. This tool exists to FIND
where (if anywhere) a real BBSHD reports the brake lever, by acting as the
display, polling the motor, and letting you diff "brake released" vs "brake held".

The display->motor request framing is just [0x11, opcode] (CAT_READ, op), no
checksum. The motor replies with an opcode-specific number of bytes, contiguous,
with no start byte or CRC (so between polls we flush and wait for an idle gap).

Usage (motor on /dev/ttyUSB0, nothing else talking to it):

    # A/B diff -- the headline mode. Follows on-screen prompts:
    #   1. release the brake, Enter -> captures a baseline
    #   2. press & HOLD the brake, Enter -> captures held
    #   3. prints every opcode/byte that differs between the two.
    python3 tools/brake_probe.py

    # Scan the whole opcode range 0x00..0x3f (not just the 7 the fw uses),
    # so brake on an unmapped opcode is still caught.
    python3 tools/brake_probe.py --scan

    # Live table: latest reply per opcode, refreshed continuously. Hold/release
    # the brake and watch which row changes (changed bytes are marked '*').
    python3 tools/brake_probe.py --monitor

Options: --port (default /dev/ttyUSB0), --baud (default 1200), --rounds
(samples per phase / per scan, default 8).
"""

import argparse
import os
import select
import sys
import termios
import time

CAT_READ = 0x11

# The opcodes the firmware currently polls, with human labels. Reply lengths are
# only used to pretty-print; probing reads whatever the motor actually sends.
KNOWN = {
    0x01: "BRAKE?   (byte0 0=braking/1=released)",
    0x08: "STATUS   (bit 0x02 = brake)",
    0x0F: "BRAKE    (0x01 held / 0x00 released)",
    0x0A: "CURRENT  (amp_x2, chk)",
    0x11: "BATTERY  (percent, chk)",
    0x20: "SPEED    (rpm_hi, rpm_lo, chk)",
    0x21: "UNKNOWN1 (fw ignores)",
    0x22: "RANGE    (motor temp hijack)",
    0x24: "CALORIES (voltage_x10 hijack)",
    0x25: "UNKNOWN3 (fw ignores)",
    0x31: "MOVING   (0x30 still / 0x31 moving)",
}

# Default opcode set for diff/monitor: the ones the fw touches plus the two
# "unknown" replies the mock knows about. --scan widens this to 0x00..0x3f.
DEFAULT_OPS = sorted(KNOWN.keys())


def open_port(path, baud):
    """Open a serial port raw at `baud`, returning an fd. No pyserial dep."""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    baud_const = {1200: termios.B1200, 9600: termios.B9600,
                  19200: termios.B19200, 38400: termios.B38400}.get(baud)
    if baud_const is None:
        raise SystemExit(f"unsupported baud {baud}")
    # [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
    attrs[0] = 0                                        # iflag: raw
    attrs[1] = 0                                        # oflag: raw
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # 8N1, no modem ctl
    attrs[3] = 0                                        # lflag: raw
    attrs[4] = baud_const
    attrs[5] = baud_const
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def poll(fd, op, window=0.30, idle=0.05):
    """Send [0x11, op], return the reply bytes.

    Reads until `idle` seconds pass with no new byte (reply is contiguous) or
    the total `window` elapses. At 1200 baud one byte is ~8.3 ms, so a 50 ms
    idle gap reliably marks the end of a reply."""
    termios.tcflush(fd, termios.TCIFLUSH)
    os.write(fd, bytes([CAT_READ, op]))
    out = bytearray()
    start = time.monotonic()
    last = start
    while True:
        now = time.monotonic()
        if now - start > window:
            break
        if out and now - last > idle:
            break
        timeout = max(0.0, idle - (now - last))
        r, _, _ = select.select([fd], [], [], timeout)
        if r:
            try:
                chunk = os.read(fd, 64)
            except OSError:
                chunk = b""
            if chunk:
                out.extend(chunk)
                last = time.monotonic()
    return bytes(out)


def capture(fd, ops, rounds):
    """Poll each opcode `rounds` times; return {op: set(hex replies seen)}."""
    seen = {op: set() for op in ops}
    for _ in range(rounds):
        for op in ops:
            reply = poll(fd, op)
            seen[op].add(reply.hex(" ") if reply else "(no reply)")
            time.sleep(0.02)  # small gap so replies don't bleed together
    return seen


def label(op):
    return KNOWN.get(op, "?")


def cmd_diff(fd, ops, rounds):
    input("Step 1/2: RELEASE the brake fully, then press Enter to capture baseline... ")
    print("  capturing baseline...", flush=True)
    base = capture(fd, ops, rounds)

    input("Step 2/2: PRESS and HOLD the brake, then press Enter (keep holding!)... ")
    print("  capturing braked...", flush=True)
    held = capture(fd, ops, rounds)

    print("\n=== opcodes whose reply CHANGED between released and held ===")
    found = False
    for op in ops:
        if base[op] != held[op]:
            found = True
            print(f"\nop 0x{op:02x}  {label(op)}")
            print(f"    released: {sorted(base[op])}")
            print(f"    held    : {sorted(held[op])}")
    if not found:
        print("  (nothing changed)\n"
              "  The brake does not alter any polled opcode's reply. Either this\n"
              "  controller doesn't report brake on the display UART at all, or it\n"
              "  only manifests while the wheel is turning / pedaling (brake cuts\n"
              "  current). Re-run with --scan, or spin the wheel by hand while\n"
              "  holding the brake and diff again.")
    else:
        print("\n  ^ the opcode(s) above are your brake signal. Note which BYTE and\n"
              "    value distinguishes held from released.")

    # Also surface opcodes that never answered, useful context.
    dead = [op for op in ops if base[op] == {"(no reply)"} == held[op]]
    if dead:
        print("\n  (no reply to: " + " ".join(f"0x{o:02x}" for o in dead) + ")")


def cmd_monitor(fd, ops):
    prev = {}
    print("Live monitor -- hold/release the brake and watch for '*' marks. Ctrl-C to stop.\n")
    try:
        while True:
            lines = []
            for op in ops:
                reply = poll(fd, op).hex(" ") or "(no reply)"
                mark = " " if prev.get(op, reply) == reply else "*"
                prev[op] = reply
                lines.append(f" {mark} 0x{op:02x} {label(op):<32} {reply}")
            # redraw in place
            sys.stdout.write("\033[H\033[2J")  # home + clear
            sys.stdout.write("op replies ('*' = changed since last sweep):\n\n")
            sys.stdout.write("\n".join(lines) + "\n")
            sys.stdout.flush()
            time.sleep(0.15)
    except KeyboardInterrupt:
        print("\nstopped.")


def main():
    ap = argparse.ArgumentParser(description="Bafang UART brake probe")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=1200)
    ap.add_argument("--rounds", type=int, default=8, help="samples per phase")
    ap.add_argument("--scan", action="store_true",
                    help="probe opcodes 0x00..0x3f, not just the known set")
    ap.add_argument("--monitor", action="store_true",
                    help="live table instead of A/B diff")
    args = ap.parse_args()

    ops = list(range(0x00, 0x40)) if args.scan else DEFAULT_OPS

    try:
        fd = open_port(args.port, args.baud)
    except OSError as e:
        raise SystemExit(f"cannot open {args.port}: {e}")
    print(f"opened {args.port} @ {args.baud} baud; probing "
          f"{len(ops)} opcode(s)\n")

    try:
        if args.monitor:
            cmd_monitor(fd, ops)
        else:
            cmd_diff(fd, ops, args.rounds)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
