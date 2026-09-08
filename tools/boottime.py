#!/usr/bin/env python3
"""Measure wall-clock time from reset to a login prompt.

    python tools/boottime.py COM17 [runs]

Kernel timestamps (`[ 1.527253]`) only measure from the point the kernel
starts, which on this board is well after the interesting part -- the
bootloader has already read and CRC'd a 10 MB partition by then, or the
ESP-IDF app has already initialised. So this times from the DTR/RTS reset
itself and reports where the time actually goes.

Markers are matched on first appearance, and any that never appear are simply
left out of the report rather than failing the run -- the native and
hypervisor builds print different things.
"""
import sys
import time

import serial

BAUD = 4000000

# (label, marker) in the order they should appear. Anything not seen is
# skipped. Covers both the native build and the hypervisor build so the same
# script measures either.
MARKERS = [
    ("first output",        b"p4boot:"),
    ("ESP-IDF app",         b"app_init:"),
    ("bootloader done",     b"jumping to"),
    ("kernel start",        b"Linux version"),
    ("root mounted",        b"Mounted root"),
    ("init exec'd",         b"Run /"),
    ("LOGIN PROMPT",        b"login:"),
]


def reset(ser):
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.05)
    ser.setDTR(False)


def one_run(port, timeout):
    """Return {label: seconds-since-reset} for the markers that appeared."""
    seen = {}
    with serial.Serial(port, BAUD, timeout=0.05) as ser:
        ser.reset_input_buffer()
        reset(ser)
        t0 = time.time()          # the instant the board is released
        buf = b""
        deadline = t0 + timeout
        while time.time() < deadline:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            now = time.time() - t0
            for label, marker in MARKERS:
                if label not in seen and marker in buf:
                    seen[label] = now
            if "LOGIN PROMPT" in seen:
                break
    return seen


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    port = sys.argv[1] if len(sys.argv) > 1 else "COM17"
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3

    results = []
    for i in range(runs):
        print(f"run {i + 1}/{runs} ...", flush=True)
        seen = one_run(port, timeout=180)
        if "LOGIN PROMPT" not in seen:
            print("   no login prompt within 180 s", flush=True)
        results.append(seen)
        time.sleep(1)

    print("\n%-18s %s" % ("marker", "  ".join(f"run{i+1:>7}"
                                              for i in range(runs))))
    print("-" * (18 + 11 * runs))
    for label, _ in MARKERS:
        vals = [r.get(label) for r in results]
        if all(v is None for v in vals):
            continue
        cells = "  ".join(f"{v:10.2f}" if v is not None else f"{'-':>10}"
                          for v in vals)
        print(f"{label:<18} {cells}")

    got = [r["LOGIN PROMPT"] for r in results if "LOGIN PROMPT" in r]
    if got:
        print(f"\nreset -> login prompt: min {min(got):.2f}s  "
              f"max {max(got):.2f}s  mean {sum(got) / len(got):.2f}s "
              f"over {len(got)} run(s)")

    # Where the time goes, using the last complete run.
    full = next((r for r in reversed(results) if "LOGIN PROMPT" in r), None)
    if full:
        print("\nphase breakdown (last run):")
        prev_label, prev_t = "reset", 0.0
        for label, _ in MARKERS:
            if label in full:
                print(f"  {prev_label:>16} -> {label:<16} "
                      f"{full[label] - prev_t:6.2f}s")
                prev_label, prev_t = label, full[label]


if __name__ == "__main__":
    sys.exit(main())
