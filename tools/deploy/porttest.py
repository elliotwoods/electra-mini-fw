"""
Port diagnostic that logs to a file as it goes, so a hang or a kill still leaves evidence.

Console output through PowerShell is buffered until the process exits, which means a run that
hangs tells you nothing at all. Everything here is written and flushed immediately.

    python porttest.py            # writes build/porttest.log
"""

import sys
import time

LOG = "build/porttest.log"
_f = open(LOG, "w", buffering=1)


def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    _f.write(line + "\n")
    _f.flush()


def main():
    from serial.tools import list_ports

    port = next((p.device for p in list_ports.comports()
                 if p.vid == 0x1FC9 and p.pid == 0x82CE), None)
    log(f"enumerated port: {port}")
    if not port:
        log("device not present")
        return

    # --- 1. raw open, no SetCommState: does the bulk path work at all? ---
    log("--- raw open (no SET_LINE_CODING) ---")
    try:
        sys.path.insert(0, "tools/deploy")
        from rawcom import RawCom
        c = RawCom(port)
        log("  raw handle opened")

        got = b""
        t0 = time.time()
        while time.time() - t0 < 2.0:
            got += c.read(4096)
        log(f"  passive listen: {len(got)} bytes {got[:120]!r}")

        try:
            c.write(b"id\r")
            log("  WriteFile ok")
        except SystemExit as e:
            log(f"  WriteFile FAILED: {e}")

        got = b""
        t0 = time.time()
        while time.time() - t0 < 2.5:
            got += c.read(4096)
        log(f"  after 'id': {len(got)} bytes {got[:400]!r}")
        c.close()
        log("  raw handle closed")
    except Exception as e:
        log(f"  raw path exception: {type(e).__name__}: {e}")

    time.sleep(1.0)

    # --- 2. pyserial open, which issues SET_LINE_CODING ---
    log("--- pyserial open (issues SET_LINE_CODING) ---")
    import serial
    try:
        s = serial.Serial(port, 115200, timeout=1.0, write_timeout=2.0)
        log("  *** OPENED - SET_LINE_CODING SUCCEEDED ***")
    except Exception as e:
        log(f"  open failed: {type(e).__name__}: {str(e)[:140]}")
        return

    time.sleep(0.4)
    s.reset_input_buffer()
    for c in ("id", "help"):
        s.write((c + "\r").encode())
        s.flush()
        time.sleep(1.5)
        out = s.read(16384)
        log(f"  '{c}' -> {len(out)} bytes")
        log("    " + out.decode("latin1", "replace").replace("\r\n", "\n    "))
    s.close()
    log("done")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        log(f"FATAL {type(e).__name__}: {e}")
