"""Native macOS vendor bulk USB, via IOKit; no driver installation required."""
import ctypes as c
from pathlib import Path
import subprocess
import time

_source = Path(__file__).with_name('usb_macos.c')
_library = _source.parents[2] / 'build' / 'host' / 'electra_usb.dylib'
_library.parent.mkdir(parents=True, exist_ok=True)
if not _library.exists() or _library.stat().st_mtime < _source.stat().st_mtime:
    subprocess.run(['cc', '-dynamiclib', '-Wno-deprecated-declarations', str(_source),
                    '-framework', 'IOKit', '-framework', 'CoreFoundation', '-o', str(_library)], check=True)
lib = c.CDLL(str(_library))
lib.electra_usb_list.argtypes = [c.POINTER(c.c_uint64), c.c_uint, c.POINTER(c.c_uint)]
lib.electra_usb_open.argtypes = [c.c_uint64, c.POINTER(c.c_void_p)]
lib.electra_usb_close.argtypes = [c.c_void_p]
lib.electra_usb_read.argtypes = [c.c_void_p, c.c_void_p, c.POINTER(c.c_uint32), c.c_uint32]
lib.electra_usb_write.argtypes = [c.c_void_p, c.c_void_p, c.c_uint32]

def check(code):
    if code: raise OSError(f'Electra IOKit error 0x{code & 0xffffffff:08x}')

def device_paths():
    ids = (c.c_uint64 * 64)(); count = c.c_uint()
    check(lib.electra_usb_list(ids, 64, c.byref(count)))
    return [str(ids[i]) for i in range(count.value)]

class WinUsbDevice:
    """Compatible deployment-tool interface; transport is IOKit on macOS."""
    def __init__(self, path=None, read_timeout_ms=60, **kwargs):
        self.handle = c.c_void_p(); self.timeout_ms = read_timeout_ms
        self.c = self; self.path = path or device_paths()[0]
        check(lib.electra_usb_open(int(path or device_paths()[0]), c.byref(self.handle)))
    def drain(self, rounds=64):
        for _ in range(rounds):
            if not self.read(): break
    def write(self, data):
        check(lib.electra_usb_write(self.handle, data, len(data)))
        return len(data)
    def read(self, size=4096):
        buf = c.create_string_buffer(size); n = c.c_uint32(size)
        check(lib.electra_usb_read(self.handle, buf, c.byref(n), self.timeout_ms))
        return buf.raw[:n.value]
    def cmd(self, line, wait=0.2):
        self.write((line+'\r').encode()); time.sleep(wait)
        result = b''
        for _ in range(20):
            chunk = self.read()
            if not chunk: break
            result += chunk
        return result.decode('latin1')
    def close(self):
        if self.handle:
            lib.electra_usb_close(self.handle); self.handle = c.c_void_p()
