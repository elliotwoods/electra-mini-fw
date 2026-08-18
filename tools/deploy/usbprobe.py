"""
Ask a USB device for its descriptors THROUGH ITS PARENT HUB, with no driver bound.

    python usbprobe.py                 # find our VID/PID and dump what the device answers
    python usbprobe.py --vid 1FC9 --pid 82D0

Why this exists: a device whose descriptors are wrong gets no driver, and a device with no
driver cannot be opened -- so the console that would explain the problem is behind the problem.
That is a genuinely circular position to be in, and it cost a physical replug to escape before
this file existed.

The hub driver is the way out. It answers IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION for any
device on any of its ports, regardless of whether that device has a function driver, because the
hub is what enumerated it in the first place. So we can ask the device for its BOS descriptor
directly and find out whether it answers -- which is the exact question that separates "Windows
never asked" from "the device refused" from "the bytes were wrong".

Read-only, and it touches nothing but the hub.
"""

import argparse
import ctypes
import ctypes.wintypes as wt

setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ole32 = ctypes.WinDLL("ole32", use_last_error=True)

GENERIC_WRITE = 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_EXISTING = 3
INVALID_HANDLE = ctypes.c_void_p(-1).value
DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x02, 0x10

GUID_DEVINTERFACE_USB_HUB = "{F18A0E88-C30C-11D0-8815-00A0C906BED8}"

IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION = 0x220410

DESC_NAMES = {0x01: "DEVICE", 0x02: "CONFIGURATION", 0x03: "STRING", 0x0F: "BOS"}


class GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_ulong), ("Data2", ctypes.c_ushort),
                ("Data3", ctypes.c_ushort), ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wt.DWORD), ("Reserved", ctypes.POINTER(ctypes.c_ulonglong))]


class USB_SETUP_PACKET(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("bmRequest", ctypes.c_ubyte), ("bRequest", ctypes.c_ubyte),
                ("wValue", ctypes.c_ushort), ("wIndex", ctypes.c_ushort),
                ("wLength", ctypes.c_ushort)]


class DESCRIPTOR_REQUEST(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("ConnectionIndex", wt.ULONG),
                ("SetupPacket", USB_SETUP_PACKET),
                ("Data", ctypes.c_ubyte * 1024)]


def _guid(text):
    g = GUID()
    if ole32.CLSIDFromString(ctypes.c_wchar_p(text), ctypes.byref(g)) != 0:
        raise SystemExit("bad GUID")
    return g


def interface_paths(guid_text):
    guid = _guid(guid_text)
    setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
    hdev = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if hdev == INVALID_HANDLE:
        raise SystemExit("SetupDiGetClassDevs failed")

    out, idx = [], 0
    try:
        while True:
            iface = SP_DEVICE_INTERFACE_DATA()
            iface.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
            if not setupapi.SetupDiEnumDeviceInterfaces(ctypes.c_void_p(hdev), None,
                                                        ctypes.byref(guid), idx,
                                                        ctypes.byref(iface)):
                break
            need = wt.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(hdev), ctypes.byref(iface),
                                                      None, 0, ctypes.byref(need), None)
            buf = ctypes.create_string_buffer(need.value)
            fixed = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            ctypes.memmove(buf, ctypes.byref(wt.DWORD(fixed)), 4)
            if setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(hdev),
                                                         ctypes.byref(iface), buf, need,
                                                         None, None):
                out.append(ctypes.wstring_at(ctypes.addressof(buf) + 4))
            idx += 1
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(ctypes.c_void_p(hdev))
    return out


def get_descriptor(hub_path, port, desc_type, index=0, length=512, langid=0):
    """One GET_DESCRIPTOR, asked of the device through the hub. Returns bytes or None."""
    k32.CreateFileW.restype = ctypes.c_void_p
    h = k32.CreateFileW(ctypes.c_wchar_p(hub_path), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
    if h == INVALID_HANDLE:
        return None

    try:
        req = DESCRIPTOR_REQUEST()
        req.ConnectionIndex = port
        req.SetupPacket.bmRequest = 0x80
        req.SetupPacket.bRequest = 0x06
        req.SetupPacket.wValue = (desc_type << 8) | index
        req.SetupPacket.wIndex = langid
        req.SetupPacket.wLength = length

        size = ctypes.sizeof(wt.ULONG) + ctypes.sizeof(USB_SETUP_PACKET) + length
        ret = wt.DWORD(0)
        ok = k32.DeviceIoControl(ctypes.c_void_p(h),
                                 wt.DWORD(IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION),
                                 ctypes.byref(req), wt.DWORD(size),
                                 ctypes.byref(req), wt.DWORD(size),
                                 ctypes.byref(ret), None)
        if not ok:
            return None
        got = ret.value - ctypes.sizeof(wt.ULONG) - ctypes.sizeof(USB_SETUP_PACKET)
        return bytes(bytearray(req.Data)[:max(0, got)])
    finally:
        k32.CloseHandle(ctypes.c_void_p(h))


def vendor_request(hub_path, port, bRequest, wValue, wIndex, length):
    """A vendor control-IN through the hub. This is how the MS OS 2.0 descriptor set is fetched,
    and asking it here answers the question that matters: whether the DEVICE can serve it, which
    is a different question from whether WINDOWS ever asked."""
    k32.CreateFileW.restype = ctypes.c_void_p
    h = k32.CreateFileW(ctypes.c_wchar_p(hub_path), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
    if h == INVALID_HANDLE:
        return None
    try:
        req = DESCRIPTOR_REQUEST()
        req.ConnectionIndex = port
        req.SetupPacket.bmRequest = 0xC0
        req.SetupPacket.bRequest = bRequest
        req.SetupPacket.wValue = wValue
        req.SetupPacket.wIndex = wIndex
        req.SetupPacket.wLength = length

        size = ctypes.sizeof(wt.ULONG) + ctypes.sizeof(USB_SETUP_PACKET) + length
        ret = wt.DWORD(0)
        ok = k32.DeviceIoControl(ctypes.c_void_p(h),
                                 wt.DWORD(IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION),
                                 ctypes.byref(req), wt.DWORD(size),
                                 ctypes.byref(req), wt.DWORD(size),
                                 ctypes.byref(ret), None)
        if not ok:
            return None
        got = ret.value - ctypes.sizeof(wt.ULONG) - ctypes.sizeof(USB_SETUP_PACKET)
        return bytes(bytearray(req.Data)[:max(0, got)])
    finally:
        k32.CloseHandle(ctypes.c_void_p(h))


def find_device(vid, pid):
    """(hub_path, port) for the first device matching, using the Enum registry for location."""
    import winreg
    key = r"SYSTEM\CurrentControlSet\Enum\USB\VID_%04X&PID_%04X" % (vid, pid)
    try:
        k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key)
    except OSError:
        return None, None

    port = None
    i = 0
    while True:
        try:
            inst = winreg.EnumKey(k, i)
        except OSError:
            break
        i += 1
        try:
            ik = winreg.OpenKey(k, inst)
            loc, _ = winreg.QueryValueEx(ik, "LocationInformation")
        except OSError:
            continue
        # "Port_#0004.Hub_#0002"
        if loc.startswith("Port_#"):
            port = int(loc[6:10])
            break

    if port is None:
        return None, None

    # Which hub? Ask every hub for the descriptor and take the one that answers with our ids.
    for hub in interface_paths(GUID_DEVINTERFACE_USB_HUB):
        d = get_descriptor(hub, port, 0x01, length=18)
        if d and len(d) >= 12:
            got_vid = d[8] | (d[9] << 8)
            got_pid = d[10] | (d[11] << 8)
            if got_vid == vid and got_pid == pid:
                return hub, port
    return None, None


def hexdump(b, base=0):
    for i in range(0, len(b), 16):
        row = b[i:i + 16]
        print("  %04X  %-47s  %s" % (
            base + i, " ".join("%02x" % c for c in row),
            "".join(chr(c) if 32 <= c < 127 else "." for c in row)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vid", default="1FC9")
    ap.add_argument("--pid", default="82D0")
    args = ap.parse_args()

    vid, pid = int(args.vid, 16), int(args.pid, 16)
    hub, port = find_device(vid, pid)
    if not hub:
        raise SystemExit("no device %04X:%04X found on any hub" % (vid, pid))
    print("device %04X:%04X on port %d of\n  %s\n" % (vid, pid, port, hub))

    for t in (0x01, 0x02, 0x0F):
        d = get_descriptor(hub, port, t)
        name = DESC_NAMES.get(t, "0x%02X" % t)
        if not d:
            print("%s: NO ANSWER — the device did not return this descriptor" % name)
        else:
            print("%s: %d bytes" % (name, len(d)))
            hexdump(d)
        print()

    # bcdUSB decides whether Windows ever asks for the BOS at all, so say it out loud.
    dev = get_descriptor(hub, port, 0x01, length=18)
    if dev and len(dev) >= 4:
        bcd = dev[2] | (dev[3] << 8)
        print("bcdUSB = 0x%04X  (BOS is only requested at 0x0201 or above)\n" % bcd)

    # If the BOS declares MS OS 2.0, follow it: fetch the set the way Windows would. The two
    # failures this separates are the whole point of the tool -- a device that cannot serve the
    # set, and a Windows that never asked because it cached an earlier answer.
    bos = get_descriptor(hub, port, 0x0F)
    if bos and len(bos) >= 33 and bos[5] == 0x1C and bos[7] == 0x05:
        total = bos[29] | (bos[30] << 8)
        vendor_code = bos[31]
        print("BOS declares MS OS 2.0: %d bytes via vendor request 0x%02X" % (total, vendor_code))
        s = vendor_request(hub, port, vendor_code, 0, 7, total)
        if not s:
            print("MS OS 2.0 SET: no answer through the hub.")
            print("  NOT proof the device refused it. IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_"
                  "CONNECTION")
            print("  appears to build a GET_DESCRIPTOR request from the setup packet's wValue/")
            print("  wIndex/wLength and ignore bmRequest/bRequest, so a vendor request cannot")
            print("  be posed this way. The device-side counters (`usb` on the console, or the")
            print("  breadcrumb the bootloader prints after a handback) are the honest test.")
        else:
            print("MS OS 2.0 SET: %d bytes (expected %d)" % (len(s), total))
            hexdump(s)
            if b"WINUSB" in s:
                print("\n  contains the WINUSB compatible id")


if __name__ == "__main__":
    main()
