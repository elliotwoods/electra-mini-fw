"""
Read a region of the device's memory and save it to a file.

    python dumpmem.py 0 100000 --out build/stock-bootloader.bin
    python dumpmem.py 400 20 --out build/option-bytes.bin

The reason this exists: the stock bootloader occupies 0x00000000-0x000FFFFF and we have never
seen it. It configures the entire machine before handing over — clocks, NMI enables, the option
bytes at 0x00000400 that set watchdog and NMI defaults, ThreadX's SysTick, and whatever display
state it leaves behind — and nearly every hard problem in this project has been a question about
inherited state that we answered by inference rather than by reading.

One of those inferences (a stray pending SysTick) cost days before it was found. Several are
still open: why the stack-pointer monitor fires spuriously, and what state the display
controller is left in.

Reading flash is memory access and nothing more, so this is read-only and safe.
"""

import argparse
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr", help="start address, hex")
    ap.add_argument("length", help="byte count, hex")
    ap.add_argument("--port")
    ap.add_argument("--out", default="build/dump.bin")
    args = ap.parse_args()

    addr = int(args.addr, 16)
    length = int(args.length, 16)

    sys.path.insert(0, __file__.rsplit("\\", 1)[0])
    from flash_usb import Device, find_port

    dev = Device(find_port(args.port))
    dev.drain()
    dev.c.write(("dump %X %X\r" % (addr, length)).encode())

    # Header, then exactly `length` raw bytes. Length is authoritative — inferring the end from
    # a delimiter inside binary data is how framing bugs are born.
    buf = b""
    deadline = time.time() + 10
    while b"DUMP " not in buf or buf.count(b"\n") < 1:
        chunk = dev.c.read(65536)
        if chunk:
            buf += chunk
            deadline = time.time() + 2
        elif time.time() > deadline:
            raise SystemExit("no DUMP header; is this firmware new enough?")

    i = buf.index(b"DUMP ")
    nl = buf.index(b"\n", i)
    parts = buf[i:nl].split()
    got_addr, got_len = int(parts[1], 16), int(parts[2], 16)

    data = bytearray(buf[nl + 1:])
    t0 = time.time()
    deadline = time.time() + 120
    while len(data) < got_len and time.time() < deadline:
        chunk = dev.c.read(65536)
        if chunk:
            data += chunk
            deadline = time.time() + 5
    dev.close()

    elapsed = max(time.time() - t0, 1e-6)
    if len(data) < got_len:
        print("warning: got %d of %d bytes" % (len(data), got_len))
    data = bytes(data[:got_len])

    with open(args.out, "wb") as f:
        f.write(data)
    print("0x%08X + 0x%X: %d bytes in %.1f s (%.0f KiB/s) -> %s"
          % (got_addr, got_len, len(data), elapsed, len(data) / elapsed / 1024, args.out))

    # A quick sanity read, so an obviously wrong dump is caught before anyone analyses it.
    if got_addr == 0 and len(data) >= 8:
        sp = int.from_bytes(data[0:4], "little")
        pc = int.from_bytes(data[4:8], "little")
        print("  vector[0] initial SP 0x%08X" % sp)
        print("  vector[1] reset PC   0x%08X %s"
              % (pc, "(Thumb bit set — plausible)" if pc & 1 else "(NO Thumb bit — suspicious)"))
        nonff = sum(1 for b in data if b != 0xFF)
        print("  %.1f%% of bytes are not 0xFF" % (100.0 * nonff / len(data)))


if __name__ == "__main__":
    main()
