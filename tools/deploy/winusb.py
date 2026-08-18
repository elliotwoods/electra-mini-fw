"""
Talk to the device's vendor bulk interface through WinUSB. No COM port, no driver install.

    python winusb.py "id"
    python winusb.py                # interactive

This is the successor to rawcom.py, and it exists for the same reason that file did: the
Windows serial stack keeps getting in the way of a device that is not a serial port. rawcom.py
already bypassed pyserial with raw Win32 because usbser.sys "gates writes on flow control we
deliberately do not implement", and refused to open the port at all if SET_LINE_CODING was not
answered to its liking. The firmware has now dropped the serial emulation entirely, so the host
side drops it too.

The device declares a WinUSB compatible ID through MS OS 2.0 descriptors, so Windows binds
WinUSB by itself -- no INF file, no Zadig, no user step. This module finds the device by VID and
PID under the generic USB device interface class, and reads and writes the two bulk endpoints.

Dependency-free, like every other tool here: ctypes against setupapi and winusb, both of which
ship with Windows.

The BOOTLOADER still enumerates as CDC and is still reached with rawcom.py / flash_usb.py. That
is deliberate -- it is replaceable only from an SD card, so its transport must not depend on
anything still being changed.
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

# The device declares this GUID in its MS OS 2.0 registry property, and winusb.sys registers a
# device interface under it. Both sides must agree on it exactly.
#
# Going through the generic USB device interface class instead was tried and does not work: a
# WinUSB-bound device is not enumerated under it, so there is nothing to open. That is worth
# knowing, because it means the registry property is load-bearing rather than decorative -- see
# the note in src/hal/usb_desc_vendor.c.
DEVICE_INTERFACE_GUID = "{6E7A1F30-4C2B-4E8A-9B21-2D4F8C1A7E55}"

VID, PID = 0x1FC9, 0x82D0

EP_OUT = 0x01
EP_IN = 0x81

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE = ctypes.c_void_p(-1).value

DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10

# WinUSB pipe policies. PIPE_TRANSFER_TIMEOUT is the important one: without it a read with no
# data waits forever, and a tool that hangs is harder to diagnose than one that returns nothing.
PIPE_TRANSFER_TIMEOUT = 0x03
IGNORE_SHORT_PACKETS = 0x05
ALLOW_PARTIAL_READS = 0x08
AUTO_CLEAR_STALL = 0x21

setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
winusb = ctypes.WinDLL("winusb", use_last_error=True)


class GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_ulong),
                ("Data2", ctypes.c_ushort),
                ("Data3", ctypes.c_ushort),
                ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD),
                ("InterfaceClassGuid", GUID),
                ("Flags", wt.DWORD),
                ("Reserved", ctypes.POINTER(ctypes.c_ulonglong))]


def _guid(text):
    g = GUID()
    ole32 = ctypes.WinDLL("ole32", use_last_error=True)
    if ole32.CLSIDFromString(ctypes.c_wchar_p(text), ctypes.byref(g)) != 0:
        raise SystemExit("bad GUID: " + text)
    return g


def device_paths(vid=VID, pid=PID):
    """Every present device exposing our interface, as filesystem paths.

    Filtered by VID and PID as well as by GUID, so that a stale or unrelated device claiming the
    same interface cannot be picked up silently."""
    guid = _guid(DEVICE_INTERFACE_GUID)
    want = "vid_%04x&pid_%04x" % (vid, pid)

    setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
    hdev = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if hdev == INVALID_HANDLE:
        raise SystemExit("SetupDiGetClassDevs failed (%d)" % ctypes.get_last_error())

    paths = []
    try:
        idx = 0
        while True:
            iface = SP_DEVICE_INTERFACE_DATA()
            iface.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
            if not setupapi.SetupDiEnumDeviceInterfaces(ctypes.c_void_p(hdev), None,
                                                        ctypes.byref(guid), idx,
                                                        ctypes.byref(iface)):
                break

            # Asked twice: once for the size, once for the data. The detail structure is
            # variable-length and its cbSize field describes the FIXED header, not the buffer,
            # which is the classic way to get ERROR_INVALID_USER_BUFFER out of this call.
            need = wt.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(hdev), ctypes.byref(iface),
                                                      None, 0, ctypes.byref(need), None)
            buf = ctypes.create_string_buffer(need.value)
            # cbSize: 8 on 64-bit (DWORD + WCHAR + padding), 6 on 32-bit.
            ctypes.memmove(buf, ctypes.byref(wt.DWORD(8 if ctypes.sizeof(ctypes.c_void_p) == 8
                                                      else 6)), 4)
            if setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(hdev),
                                                         ctypes.byref(iface), buf, need,
                                                         None, None):
                path = ctypes.wstring_at(ctypes.addressof(buf) + 4)
                if want in path.lower():
                    paths.append(path)
            idx += 1
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(ctypes.c_void_p(hdev))

    return paths


class WinUsbDevice:
    """The same read/write/cmd surface RawCom offered, over bulk endpoints instead of a port."""

    def __init__(self, path=None, read_timeout_ms=60):
        if path is None:
            found = device_paths()
            if not found:
                raise SystemExit(
                    "no device found.\n"
                    "  - is it running the application? the BOOTLOADER is still CDC, "
                    "use rawcom.py / flash_usb.py for that\n"
                    "  - check Device Manager shows it under 'Universal Serial Bus devices' "
                    "bound to WinUSB")
            path = found[0]

        self.path = path
        k32.CreateFileW.restype = ctypes.c_void_p
        # FILE_FLAG_OVERLAPPED is REQUIRED: WinUsb_Initialize fails with ERROR_INVALID_HANDLE
        # on a synchronous handle. Passing NULL for the OVERLAPPED in Read/WritePipe is still
        # allowed and completes the transfer synchronously, which is what this tool wants.
        self.h = k32.CreateFileW(ctypes.c_wchar_p(path),
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 None, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
        if self.h == INVALID_HANDLE:
            raise SystemExit("CreateFile on %s failed (%d)" % (path, ctypes.get_last_error()))

        self.wh = ctypes.c_void_p()
        if not winusb.WinUsb_Initialize(ctypes.c_void_p(self.h), ctypes.byref(self.wh)):
            raise SystemExit("WinUsb_Initialize failed (%d)" % ctypes.get_last_error())

        self._policy(EP_IN, PIPE_TRANSFER_TIMEOUT, read_timeout_ms)
        self._policy(EP_IN, ALLOW_PARTIAL_READS, 1)
        self._policy(EP_IN, IGNORE_SHORT_PACKETS, 0)
        self._policy(EP_IN, AUTO_CLEAR_STALL, 1)
        self._policy(EP_OUT, PIPE_TRANSFER_TIMEOUT, 1000)
        self._policy(EP_OUT, AUTO_CLEAR_STALL, 1)

        # The tools reach through `.c` for raw reads and writes, because flash_usb.Device wraps
        # a separate transport object. This class IS its own transport, so it points at itself
        # and every tool works against either without knowing which it has.
        self.c = self

        self.drain()

    def drain(self, rounds=64):
        """Throw away whatever was queued before this session started.

        The device emits heartbeats unprompted, and under WinUSB nothing collects them until a
        program asks -- so on opening there is a backlog describing a device state that has since
        moved on. Worse, it is at the FRONT of the pipe, so the answer to the first command sits
        behind it. Dropping it costs nothing and makes the first command behave like the tenth."""
        n = 0
        for _ in range(rounds):
            chunk = self.read(8192)
            if not chunk:
                break
            n += len(chunk)
        return n

    def _policy(self, pipe, policy, value):
        v = wt.DWORD(value)
        winusb.WinUsb_SetPipePolicy(self.wh, ctypes.c_ubyte(pipe), wt.DWORD(policy),
                                    wt.DWORD(4), ctypes.byref(v))

    def write(self, data: bytes):
        """All of it, or raise. A short write on a framed protocol is a truncated message."""
        sent = 0
        view = memoryview(data)
        while sent < len(data):
            chunk = bytes(view[sent:])
            n = wt.DWORD(0)
            ok = winusb.WinUsb_WritePipe(self.wh, ctypes.c_ubyte(EP_OUT), chunk, len(chunk),
                                         ctypes.byref(n), None)
            if not ok:
                raise SystemExit("WinUsb_WritePipe failed (%d)" % ctypes.get_last_error())
            if n.value == 0:
                raise SystemExit("WinUsb_WritePipe wrote nothing")
            sent += n.value
        return sent

    def read(self, size=4096):
        buf = ctypes.create_string_buffer(size)
        n = wt.DWORD(0)
        ok = winusb.WinUsb_ReadPipe(self.wh, ctypes.c_ubyte(EP_IN), buf, size,
                                    ctypes.byref(n), None)
        # Return whatever arrived even when the call reports failure. A read that times out
        # part-way through has still moved those bytes off the device, and discarding them
        # loses data the device will never send again -- which showed up as a large ECHO
        # getting no answer while a small one worked, and as every message after it going
        # missing because the stream had been cut mid-frame.
        got = buf.raw[:n.value] if n.value else b""
        if not ok and not got:
            return b""                 # an ordinary idle timeout
        return got

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
        winusb.WinUsb_Free(self.wh)
        k32.CloseHandle(ctypes.c_void_p(self.h))


def main():
    found = device_paths()
    if not found:
        raise SystemExit(
            "no WinUSB device %04X:%04X.\n"
            "  - if it is in the BOOTLOADER it is a COM port, not this: use rawcom.py\n"
            "  - if Device Manager shows it with a yellow mark, WinUSB did not bind; run\n"
            "    tools\\deploy\\usbprobe.py to see what it answered during enumeration"
            % (VID, PID))
    print("device: %s" % found[0])

    d = WinUsbDevice(found[0])
    if len(sys.argv) > 1:
        for cmd in sys.argv[1:]:
            print("--- %s ---" % cmd)
            print(d.cmd(cmd))
    else:
        print(d.cmd("id"))
        try:
            while True:
                print(d.cmd(input("> ")), end="")
        except (KeyboardInterrupt, EOFError):
            pass
    d.close()


if __name__ == "__main__":
    main()
