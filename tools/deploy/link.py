"""
Open the device, whichever transport it happens to be using.

The two images do not enumerate the same way any more. The APPLICATION is vendor bulk behind
WinUSB; the BOOTLOADER is CDC on a COM port, deliberately, because it is replaceable only from an
SD card and its transport must not depend on anything still being changed.

Every tool here talks to one or the other, and none of them should have to care which. So the
choice lives in one place rather than in six, and the objects returned present the same surface:
`.c.read(n)`, `.c.write(bytes)`, `.drain()`, `.cmd(line)`, `.close()`.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def open_app(port=None, required=True):
    """The running application.

    Tries WinUSB first, since that is what the current firmware uses, then falls back to a COM
    port so that an older image -- or the bootloader, which answers many of the same commands --
    still works with the same tools.
    """
    if not port:
        try:
            from winusb import WinUsbDevice, device_paths
            if device_paths():
                return WinUsbDevice()
        except Exception:
            pass

    from flash_usb import Device, find_port
    found = find_port(port, quiet=True)
    if found:
        return Device(found)

    if not required:
        return None
    raise SystemExit(
        "no device found.\n"
        "  The application is WinUSB (VID 1FC9 PID 82D0); the bootloader is a COM port\n"
        "  (PID 82CE). Neither is present. Unplug and replug, or check Device Manager.")


def open_bootloader(port=None):
    """The bootloader specifically, which is always the COM port."""
    from flash_usb import Device, find_port
    return Device(find_port(port))
