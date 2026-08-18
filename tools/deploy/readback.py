"""Read a file back off the card and write it locally, so the copy that is actually on the
device can be gated independently of the copy we think we wrote.

    python readback.py /BOOT/update.srec out.srec

Run elevated. Read-only with respect to the device.
"""

import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import putfile  # noqa: E402


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "/BOOT/update.srec"
    dst = sys.argv[2] if len(sys.argv) > 2 else "readback.bin"

    num = putfile.find_disk()
    disk = putfile.Raw(num, writable=False)
    try:
        mbr = disk.read(0)
        part = None
        for i in range(4):
            if mbr[446 + i * 16 + 4] in (0x0B, 0x0C, 0x06, 0x0E):
                part = struct.unpack_from("<I", mbr, 446 + i * 16 + 8)[0]
                break
        if not part:
            raise SystemExit("no FAT partition")

        fs = putfile.Fat32(disk, part)
        dirpath, _, name = src.strip("/").rpartition("/")
        clus = fs.resolve_dir(dirpath)
        entry = next((x for x in fs.list_dir(clus) if x[0].upper() == name.upper()), None)
        if not entry:
            raise SystemExit(f"{src} not found on the card")

        data = fs.read_chain(entry[3], entry[2])
    finally:
        disk.close()

    with open(dst, "wb") as f:
        f.write(data)
    print(f"read {len(data):,} bytes from {src} -> {dst}")


if __name__ == "__main__":
    main()
