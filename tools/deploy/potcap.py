"""
Capture the raw two-track phase pairs from one endless pot.

    python potcap.py 0 --secs 30 --out build/pot0.txt

The rotation decode has to know what these two resistive tracks actually do as the shaft
turns. That is a measurement rather than a datasheet fact: the pots are endless and two-track,
the threshold constants inherited from the stock firmware (5 / 0x200 / 0x3FE) are in 10-bit
units against our right-aligned 12-bit ADC, and the existing `pots` console command only
reports samples that already moved past a deadband -- which discards exactly the shape needed.

Turn the knob slowly and steadily through several complete revolutions while this runs.
"""

import argparse
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pot", type=int, help="panel pot index, 0 = bottom-left")
    ap.add_argument("--secs", type=int, default=30)
    ap.add_argument("--port")
    ap.add_argument("--out", default="build/potcap.txt")
    args = ap.parse_args()

    sys.path.insert(0, __file__.rsplit("\\", 1)[0])
    from link import open_app

    dev = open_app(args.port)
    dev.drain()
    dev.c.write(("potcap %X %X\r" % (args.pot, args.secs)).encode())

    # Records the whole window with no arming. An earlier version waited for movement first, so
    # the operator turned the knob to arm it, recording began silently, and by then they had
    # stopped -- a capture full of a stationary pot whose value had demonstrably changed. The
    # device cannot say "now", so ask for continuous movement across the whole window instead.
    buf = b""
    print("recording for %d s -- turn the knob NOW, continuously" % args.secs, flush=True)
    deadline = time.time() + args.secs + 20
    while b"CAPEND" not in buf and time.time() < deadline:
        chunk = dev.c.read(65536)
        if chunk:
            buf += chunk
    dev.close()

    text = buf.decode("ascii", "replace")
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2:
            try:
                rows.append((int(parts[0], 16), int(parts[1], 16)))
            except ValueError:
                pass

    with open(args.out, "w") as f:
        for a, b in rows:
            f.write("%d %d\n" % (a, b))

    print("%d samples -> %s" % (len(rows), args.out))
    if rows:
        a = [r[0] for r in rows]
        b = [r[1] for r in rows]
        print("  phase A range %d..%d   phase B range %d..%d"
              % (min(a), max(a), min(b), max(b)))
        moved = sum(1 for i in range(1, len(rows))
                    if abs(rows[i][0] - rows[i - 1][0]) > 16
                    or abs(rows[i][1] - rows[i - 1][1]) > 16)
        print("  %d samples show movement past the 16-count deadband" % moved)
        if moved < 20:
            print("  WARNING: almost nothing moved -- was the right knob turned?")


if __name__ == "__main__":
    main()
