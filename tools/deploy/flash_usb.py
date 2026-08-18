"""
Flash an application image over USB. No buttons, no cables, no card.

    python flash_usb.py build/app.srec            # find the port, flash, run
    python flash_usb.py build/app.srec --port COM7
    python flash_usb.py --reboot                  # ask a running app to return to the bootloader
    python flash_usb.py --console                 # interactive terminal

The two images no longer share a transport, and this tool spans both so that you do not have to
think about it. The BOOTLOADER is CDC-ACM on a COM port and accepts erase/write/crc/run. The
APPLICATION is vendor bulk behind WinUSB and accepts `boot`, which writes the handshake word and
resets so we land back in the bootloader.

So the ordinary case is still one command: if the application is running, this reboots it over
WinUSB, waits for the bootloader's COM port, and flashes. The whole edit-build-run cycle happens
without touching the hardware.

Requires pyserial:  python -m pip install pyserial
"""

import os
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    raise SystemExit("pyserial is required:  python -m pip install pyserial")


BAUD = 115200          # ignored by CDC-ACM, but pyserial wants a number
# Bytes of image per `wr` line. The device's console line buffer is 96 chars and a line is
# "wr <hexoff> " plus 3 chars per byte, so 24 bytes gives 8 + 72 = 80 — comfortably inside it.
# At 32 the line was 104 chars and would have been silently truncated, corrupting the image
# in a way that only a CRC check at the end would catch.
CHUNK = 24


def find_port(explicit=None, quiet=False):
    if explicit:
        return explicit
    candidates = []
    for p in list_ports.comports():
        # VID 0x1FC9 / PID 0x82CE is what our firmware enumerates as. The stock firmware is
        # 0x82CF, deliberately different, so we can never talk to the wrong one.
        if p.vid == 0x1FC9 and p.pid == 0x82CE:
            candidates.append(p.device)
    if not candidates:
        # quiet: the caller is asking "is the bootloader there?", not asserting that it is.
        # The application is on a different transport entirely now, so absence here is an
        # ordinary answer rather than a failure.
        if quiet:
            return None
        ports = ", ".join(f"{p.device}({p.vid:04X}:{p.pid:04X})" for p in list_ports.comports()) or "none"
        raise SystemExit(f"No Electra Mini FW device found. Ports present: {ports}")
    if len(candidates) > 1:
        print(f"note: several candidates {candidates}, using {candidates[0]}")
    return candidates[0]


class Device:
    """Talks over a raw handle rather than pyserial.

    pyserial calls SetCommState on open (issuing CDC SET_LINE_CODING) and asserts DTR/RTS
    (SET_CONTROL_LINE_STATE). The device answers both, but usbser.sys then gates writes on
    flow control we deliberately do not implement — it is not a real UART — and every write
    times out. Opening the handle directly and skipping comm state entirely avoids the whole
    question, and is proven to work in both directions on this device.
    """

    def __init__(self, port):
        sys.path.insert(0, __file__.rsplit("\\", 1)[0])
        from rawcom import RawCom
        self.c = RawCom(port)
        self.drain()

    def drain(self, quiet=0.25, limit=3.0):
        """Read until the device has been silent for `quiet` seconds.

        A single read is not enough. The bootloader prints a banner, the application emits
        protocol traffic unprompted, and whatever is left over lands in the NEXT command's
        reply — so every response comes back one behind, and a perfectly successful command is
        judged a failure. That is what the intermittent "erase failed" was: the erase takes
        0.3 s and always worked, while the tool was reading the previous command's tail.
        """
        end = time.time() + limit
        last = time.time()
        while time.time() < end and time.time() - last < quiet:
            if self.c.read(65536):
                last = time.time()

    def cmd(self, line, settle=0.02, expect_prompt=True, timeout=3.0):
        self.drain(quiet=0.05, limit=0.5)      # start each command from a known-empty pipe
        self.c.write((line + "\r").encode())
        time.sleep(settle)
        # Wait for the PROMPT, bounded by `timeout` overall — never by a gap in the output.
        #
        # The previous version reset its deadline to 0.12 s after each chunk, so any pause
        # longer than that ended the read with a partial reply. `erase` pauses about 0.3 s
        # between echoing the command and reporting its result, so the tool routinely saw only
        # the echo and announced "erase failed" while the device was succeeding. Every one of
        # those failures was this line.
        out = b""
        end = time.time() + timeout
        quiet_since = None
        while time.time() < end:
            chunk = self.c.read(65536)
            if chunk:
                out += chunk
                quiet_since = None
                if expect_prompt and out.rstrip().endswith(b">"):
                    break
            else:
                # With no prompt expected, "it has gone quiet" is the only end there is.
                if not expect_prompt and out:
                    if quiet_since is None:
                        quiet_since = time.time()
                    elif time.time() - quiet_since > 0.3:
                        break
                time.sleep(0.005)
        return out.decode("latin1", "replace")

    def close(self):
        self.c.close()


def parse_srec(path):
    """Return (base_address, bytes, entry).

    Gaps are FILLED WITH 0xFF, not rejected. The linker legitimately leaves small holes between
    sections when alignment demands it — a four-byte hole appeared between the vector table and
    .text the moment a handler's alignment changed — and 0xFF is exactly what erased flash reads
    as, so filling is not an approximation. The region is erased immediately before writing.

    Rejecting them, as this used to, meant an ordinary linker layout change stopped the flasher
    dead with a message about discontiguity. Worse, check-image only WARNED about the same
    condition, so the two tools disagreed about whether an image was valid."""
    segs = {}
    entry = None
    addr_bytes = {"0": 2, "1": 2, "2": 3, "3": 4, "5": 2, "6": 3, "7": 4, "8": 3, "9": 2}
    with open(path, "r") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line:
                continue
            if line[0] != "S":
                raise SystemExit(f"{path}:{lineno}: not an S-record")
            t = line[1]
            count = int(line[2:4], 16)
            payload = bytes.fromhex(line[4:])
            if len(payload) != count:
                raise SystemExit(f"{path}:{lineno}: length mismatch")
            if (~(count + sum(payload[:-1])) & 0xFF) != payload[-1]:
                raise SystemExit(f"{path}:{lineno}: checksum mismatch")
            n = addr_bytes[t]
            a = int.from_bytes(payload[:n], "big")
            data = payload[n:-1]
            if t in "123":
                segs[a] = data
            elif t in "789":
                entry = a

    if not segs:
        raise SystemExit("no data records")
    base = min(segs)
    blob = bytearray()
    cur = base
    filled = 0
    for a in sorted(segs):
        if a < cur:
            raise SystemExit(f"overlapping records at 0x{a:08X}")
        if a > cur:
            gap = a - cur
            if gap > 4096:
                raise SystemExit(f"implausible gap at 0x{cur:08X}..0x{a:08X} ({gap} bytes)")
            blob += bytes([0xFF]) * gap
            filled += gap
            cur = a
        blob += segs[a]
        cur += len(segs[a])
    if filled:
        print(f"note      : filled {filled} byte(s) of inter-section gap with 0xFF")
    return base, bytes(blob), entry


def crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return ~crc & 0xFFFFFFFF


def flash(path, port):
    base, blob, entry = parse_srec(path)
    print(f"image     : {path}")
    print(f"base      : 0x{base:08X}   entry 0x{entry:08X}")
    print(f"size      : {len(blob):,} bytes")

    if base != 0x00120000:
        raise SystemExit(f"image is linked at 0x{base:08X}, expected 0x00120000 "
                         f"(is this the bootloader rather than the app?)")

    dev = Device(port)
    print(f"port      : {port}\n")

    banner = dev.cmd("id")
    if "BOOTLOADER" not in banner:
        print(banner)
        raise SystemExit("device is not in the bootloader. Run with --reboot first.")
    print(banner.strip(), "\n")

    print("erasing...")
    # Erasing the whole 2,944 KB application region is 92 block erases and can take many
    # seconds. The default 3 s deadline was passing on luck, and when it did not, the tool
    # reported "erase failed" while the device was erasing perfectly successfully — a
    # maximally misleading message to get from a flash tool.
    out = dev.cmd("erase", settle=0.3, timeout=30.0)
    if "erase rc 0x00000000" not in out:
        print(out)
        raise SystemExit("erase failed")

    # Pad to the 256-byte program unit with 0xFF; the device writes whole units and a
    # partial one is not expressible.
    padded = blob + b"\xFF" * ((-len(blob)) % 256)
    print(f"writing {len(padded):,} bytes...")

    # Pipelined. Waiting for a prompt after every single line made this ~320 s for 9 KB,
    # because the cost is round-trips, not bytes. Sending a batch back-to-back and draining
    # once is safe: the device's OUT endpoint NAKs while its receive buffer is full, so the
    # host's own write blocks — USB gives us flow control for free and we cannot overrun it.
    BATCH = 16

    t0 = time.time()
    lines = []
    for off in range(0, len(padded), CHUNK):
        piece = padded[off:off + CHUNK]
        lines.append((off, f"wr {off:X} " + " ".join(f"{b:02X}" for b in piece)))

    for i in range(0, len(lines), BATCH):
        batch = lines[i:i + BATCH]
        for _, line in batch:
            dev.c.write((line + "\r").encode())

        # Drain the batch's replies and check none of them failed.
        out = b""
        deadline = time.time() + 5.0
        while time.time() < deadline:
            chunk = dev.c.read(8192)
            if chunk:
                out += chunk
                deadline = time.time() + 0.10
            elif out:
                break
        text = out.decode("latin1", "replace")
        if "FAILED" in text or "Invalid" in text:
            print(text)
            raise SystemExit(f"write failed near offset 0x{batch[0][0]:X}")

        pct = 100.0 * (i + len(batch)) / len(lines)
        print(f"  {pct:5.1f}%  0x{batch[-1][0]:06X}", end="\r", flush=True)

    dev.cmd("flush")
    elapsed = time.time() - t0
    print(f"  100.0%  {len(padded):,} bytes in {elapsed:.1f}s "
          f"({len(padded)/elapsed/1024:.1f} KiB/s)")

    expect = crc32(padded)
    out = dev.cmd(f"crc {len(padded):X}")
    print(out.strip())
    if f"{expect:08X}".lower() not in out.lower():
        raise SystemExit(f"CRC mismatch: expected {expect:08X}")
    print(f"verified   : crc32 {expect:08X}\n")

    print("launching...")
    dev.cmd("run", expect_prompt=False)
    dev.close()
    print("done.")


def app_present():
    """Is the application enumerated? Asked WITHOUT opening it.

    Opening is not a free test: only one handle at a time can drive the interface, so a caller
    that opens the device merely to find out whether it is there leaves it unavailable to the
    code that actually wants it. That mistake cost a confusing "no device found" from a script
    holding the device open itself."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from winusb import device_paths
    except Exception:
        return False
    return bool(device_paths())


def app_over_winusb():
    """The running application, opened. None if it is not there.

    The two images no longer share a transport: the bootloader is CDC and the application is
    vendor bulk behind WinUSB. So `boot` has to be sent over a different pipe from the one the
    flashing happens on, and this is the only part of that which is awkward -- everything after
    the reboot is exactly as it was."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from winusb import WinUsbDevice, device_paths
    except Exception:
        return None
    if not device_paths():
        return None
    try:
        return WinUsbDevice()
    except SystemExit:
        return None


def reboot_to_bootloader(port=None):
    """Get the device into the bootloader, from wherever it currently is."""
    app = app_over_winusb()
    if app:
        print("asking the application to reboot into the bootloader...")
        app.write(b"boot\r")
        time.sleep(0.5)
        app.close()
        time.sleep(2.5)          # re-enumeration
        print("done — the device should now be in the bootloader.")
        return

    found = find_port(port, quiet=True)
    if not found:
        raise SystemExit(
            "no device found, as either a bootloader COM port or a WinUSB application.\n"
            "  Unplug and replug: a freshly flashed image has not proven itself, so the\n"
            "  bootloader holds rather than launching it, and it appears as a COM port.")

    dev = Device(found)
    banner = dev.cmd("id")
    if "BOOTLOADER" in banner:
        print("already in the bootloader.")
        dev.close()
        return
    print("asking the application to reboot into the bootloader...")
    dev.cmd("boot", expect_prompt=False)
    dev.close()
    time.sleep(2.5)
    print("done — the device should now be in the bootloader.")


def console(port):
    dev = Device(port)
    print(f"connected to {port}. Ctrl-C to exit.\n")
    print(dev.cmd("id"))
    try:
        while True:
            line = input()
            print(dev.cmd(line), end="")
    except (KeyboardInterrupt, EOFError):
        dev.close()
        print("\nclosed.")


def main():
    args = sys.argv[1:]
    port_idx = args.index("--port") if "--port" in args else -1
    port = args[port_idx + 1] if port_idx >= 0 else None

    if "--reboot" in args:
        reboot_to_bootloader(find_port(port))
        return
    if "--console" in args:
        console(find_port(port))
        return

    images = [a for a in args if not a.startswith("--") and a != port]
    if not images:
        raise SystemExit(__doc__)

    # If the application is running, put the device in the bootloader first rather than telling
    # the user to run a second command. It used to be one invocation before the transports
    # diverged, and there is no reason for it to be two now.
    if find_port(port, quiet=True) is None and app_present():
        reboot_to_bootloader(port)

    flash(images[0], find_port(port))


if __name__ == "__main__":
    main()
