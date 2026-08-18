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

# The generic USB device interface class. Every WinUSB-bound device appears under it, and the
# path carries the VID and PID, so no custom GUID has to be agreed between the two sides -- which
# also keeps the device's MS OS 2.0 descriptor set down to a single 64-byte packet. See the note
# in src/hal/usb_desc_vendor.c about why that mattered.
GUID_DEVINTERFACE_USB_DEVICE = "{A5DCBF10-6530-11D2-901F-00C04FB951ED}"

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
    if setupapi.CLSIDFromString is None:
        raise SystemExit("CLSIDFromString unavailable")
    ole32 = ctypes.WinDLL("ole32", use_last_error=True)
    if ole32.CLSIDFromString(ctypes.c_wchar_p(text), ctypes.byref(g)) != 0:
        raise SystemExit("bad GUID: " + text)
    return g


def device_paths(vid=VID, pid=PID):
    """Every present USB device with this VID and PID, as filesystem paths.

    Matching on the path rather than on a dedicated interface GUID: the path for a device
    interface always contains vid_xxxx&pid_xxxx, so this works for anything WinUSB has bound
    without the firmware having to carry a GUID at all."""
    guid = _guid(GUID_DEVINTERFACE_USB_DEVICE)
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
        if not ok:
            return b""                 # a timeout with nothing waiting; not an error
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
