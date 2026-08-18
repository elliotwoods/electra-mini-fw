"""
Talk to a CDC-ACM device WITHOUT configuring the port.

pyserial always calls SetCommState on open, which makes Windows issue CDC SET_LINE_CODING.
A device that mishandles that request fails to open with "The semaphore timeout period has
expired" even though it enumerated perfectly and its bulk endpoints work fine.

Since line coding is meaningless for a USB CDC device that isn't a real UART, we can simply
open the handle and do ReadFile/WriteFile without ever touching the comm state. That is
enough to reach a device whose only defect is in a control request nobody needs.

    python rawcom.py COM4 "id"
    python rawcom.py COM4            # interactive
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80
INVALID_HANDLE = ctypes.c_void_p(-1).value

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [("ReadIntervalTimeout", wt.DWORD),
                ("ReadTotalTimeoutMultiplier", wt.DWORD),
                ("ReadTotalTimeoutConstant", wt.DWORD),
                ("WriteTotalTimeoutMultiplier", wt.DWORD),
                ("WriteTotalTimeoutConstant", wt.DWORD)]


class RawCom:
    def __init__(self, port):
        path = f"\\\\.\\{port}"
        self.h = k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, None,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
        if self.h == INVALID_HANDLE:
            raise SystemExit(f"CreateFile({port}) failed: {ctypes.get_last_error()}")

        # Timeouts are a driver-side setting and do NOT generate a USB control transfer,
        # unlike SetCommState. Safe to use.
        # (ReadInterval, ReadMultiplier, ReadConstant, WriteMultiplier, WriteConstant)
        #
        # ReadIntervalTimeout is what makes a read return promptly. With it at 0 and a 400 ms
        # total, ReadFile waits the FULL 400 ms whenever fewer bytes arrive than were asked
        # for — and since callers ask for a big buffer, that is always. It made every
        # round-trip measurement read as a flat ~400 ms, which is a measurement of this line
        # and nothing else.
        #
        # 10 ms between bytes ends the read shortly after the last byte; the 60 ms total is
        # the cap when nothing arrives at all.
        t = COMMTIMEOUTS(10, 0, 60, 0, 800)
        if not k32.SetCommTimeouts(self.h, ctypes.byref(t)):
            print(f"warning: SetCommTimeouts failed ({ctypes.get_last_error()})")

    def write(self, data: bytes):
        # Loop until it is all gone. A single WriteFile may report a short write, and silently
        # returning the count leaves the caller believing it sent a whole frame when it sent
        # part of one — which on a framed protocol produces a truncated message rather than an
        # error, i.e. exactly the failure mode this protocol exists to make impossible.
        sent = 0
        view = memoryview(data)
        while sent < len(data):
            n = wt.DWORD(0)
            chunk = bytes(view[sent:])
            if not k32.WriteFile(self.h, chunk, len(chunk), ctypes.byref(n), None):
                raise SystemExit(f"WriteFile failed: {ctypes.get_last_error()}")
            if n.value == 0:
                raise SystemExit("WriteFile wrote nothing; the port is not accepting data")
            sent += n.value
        return sent

    def read(self, size=4096):
        buf = ctypes.create_string_buffer(size)
        n = wt.DWORD(0)
        if not k32.ReadFile(self.h, buf, size, ctypes.byref(n), None):
            err = ctypes.get_last_error()
            if err:
                return b""
        return buf.raw[:n.value]

    def cmd(self, line, wait=1.2):
        self.write((line + "\r").encode())
        time.sleep(wait)
        out = b""
        for _ in range(6):
            chunk = self.read()
            if chunk:
                out += chunk
            elif out:
                break
            else:
                time.sleep(0.15)
        return out.decode("latin1", "replace")

    def close(self):
        k32.CloseHandle(self.h)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    port = sys.argv[1]
    c = RawCom(port)
    print(f"opened {port} without SetCommState\n")

    if len(sys.argv) > 2:
        for cmd in sys.argv[2:]:
            print(f"--- {cmd} ---")
            print(c.cmd(cmd))
    else:
        print(c.cmd("id"))
        try:
            while True:
                line = input("> ")
                print(c.cmd(line), end="")
        except (KeyboardInterrupt, EOFError):
            pass
    c.close()


if __name__ == "__main__":
    main()
