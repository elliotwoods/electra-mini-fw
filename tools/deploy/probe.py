"""
Probe a CDC device that enumerates but does not talk properly.

Opens without SetCommState (so no SET_LINE_CODING is issued), then tries each direction
independently and reports exactly which one moves data. Distinguishing "we cannot send" from
"we cannot receive" from "neither" is the whole point — those are three different bugs.

    python probe.py COM4
"""

import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from rawcom import RawCom  # noqa: E402


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    c = RawCom(port)
    print(f"opened {port} (no SetCommState)\n")

    # 1. Is the device sending anything unprompted? The bootloader prints a banner when
    #    usb_flash_service() starts, so if the TX path works at all we may catch it.
    print("--- passive listen, 3s (expect the boot banner if TX works) ---")
    got = b""
    t0 = time.time()
    while time.time() - t0 < 3.0:
        chunk = c.read(4096)
        if chunk:
            got += chunk
    print(f"  {len(got)} bytes: {got!r}\n")

    # 2. Can we send? WriteFile succeeding only means the driver queued it.
    print("--- write a bare CR, then listen 2s ---")
    try:
        n = c.write(b"\r")
        print(f"  WriteFile ok, {n} bytes queued")
    except SystemExit as e:
        print(f"  WriteFile FAILED: {e}")
    got = b""
    t0 = time.time()
    while time.time() - t0 < 2.0:
        chunk = c.read(4096)
        if chunk:
            got += chunk
    print(f"  {len(got)} bytes back: {got!r}\n")

    # 3. Full command, generous wait.
    print("--- 'id' + CR, listen 3s ---")
    try:
        c.write(b"id\r")
    except SystemExit as e:
        print(f"  WriteFile FAILED: {e}")
    got = b""
    t0 = time.time()
    while time.time() - t0 < 3.0:
        chunk = c.read(4096)
        if chunk:
            got += chunk
    print(f"  {len(got)} bytes back: {got!r}\n")

    print("verdict:")
    print("  bytes received at any point -> device TX works, so the console is alive")
    print("  nothing at all              -> TX path or the poll loop is not running")
    c.close()


if __name__ == "__main__":
    main()
