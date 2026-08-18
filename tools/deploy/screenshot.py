"""
Read the device's framebuffer back and save it as a PNG.

    python screenshot.py                 # whole screen -> build/shot.png
    python screenshot.py --out foo.png
    python screenshot.py --region 0 0 200 100

Why this exists beyond convenience: a black panel has at least two completely different causes
— nothing was drawn, or something was drawn and is not being scanned out — and from outside the
device they look identical. Reading video memory back tells them apart in one command, and it
means the firmware's idea of what it drew can be checked against what is actually there.

768 KB at ~270 KB/s is a few seconds. That is fine for a debugging tool and is why the transfer
is raw RGB565 rather than hex.
"""

import argparse
import struct
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--out", default="build/shot.png")
    ap.add_argument("--region", nargs=4, type=int, metavar=("X", "Y", "W", "H"))
    args = ap.parse_args()

    sys.path.insert(0, __file__.rsplit("\\", 1)[0])
    from flash_usb import Device, find_port

    dev = Device(find_port(args.port))

    if args.region:
        x, y, w, h = args.region
        cmd = "shot %X %X %X %X" % (x, y, w, h)
    else:
        cmd = "shot"

    dev.drain()
    dev.c.write((cmd + "\r").encode())

    # Header first: "SHOT <w> <h>". Everything after the newline is raw pixels until the byte
    # count is satisfied — length is authoritative, exactly as in the wire protocol, because
    # inferring the end from a delimiter in binary data is how framing bugs are born.
    buf = b""
    deadline = time.time() + 10
    while b"SHOT " not in buf or buf.count(b"\n") < 1:
        chunk = dev.c.read(65536)
        if chunk:
            buf += chunk
            deadline = time.time() + 2
        elif time.time() > deadline:
            raise SystemExit("no SHOT header; is this firmware new enough?")

    i = buf.index(b"SHOT ")
    nl = buf.index(b"\n", i)
    parts = buf[i:nl].split()
    w = int(parts[1], 16)
    h = int(parts[2], 16)
    want = w * h * 2

    data = bytearray(buf[nl + 1:])
    t0 = time.time()
    deadline = time.time() + 60
    while len(data) < want and time.time() < deadline:
        chunk = dev.c.read(65536)
        if chunk:
            data += chunk
            deadline = time.time() + 5
    dev.close()

    got = len(data)
    if got < want:
        print("warning: got %d of %d bytes; image will be short" % (got, want))
    data = bytes(data[:want])
    print("%dx%d, %d bytes in %.1f s (%.0f KiB/s)"
          % (w, h, got, time.time() - t0, got / max(time.time() - t0, 1e-6) / 1024))

    from PIL import Image
    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(min(w * h, len(data) // 2)):
        c = data[i * 2] | (data[i * 2 + 1] << 8)
        px[i % w, i // w] = (((c >> 11) & 0x1F) * 255 // 31,
                             ((c >> 5) & 0x3F) * 255 // 63,
                             (c & 0x1F) * 255 // 31)
    img.save(args.out)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
