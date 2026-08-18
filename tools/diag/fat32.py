"""
Read the Electra One Mini's FAT32 volume directly from the raw disk.

Windows will not mount it: the MBR partition claims 15,259,648 sectors while the
bootloader's USB mass-storage code reports only 8,388,000, so the partition runs off
the end of the device and the volume layer rejects it. The filesystem itself is fine,
so we bypass Windows and parse FAT32 ourselves.

READ ONLY. Nothing is ever written to the device.

    python fat32.py list                 # walk the tree
    python fat32.py copy <dest-dir>      # extract everything reachable

Must be run from an elevated shell (raw disk access requires it).
"""

import os
import struct
import sys

SECTOR = 512


class RawDisk:
    """Sector-aligned reader. Windows raw device I/O must be aligned and sized in sectors."""

    def __init__(self, path):
        self.f = open(path, "rb", buffering=0)

    def read_at(self, offset, length):
        start = (offset // SECTOR) * SECTOR
        end = ((offset + length + SECTOR - 1) // SECTOR) * SECTOR
        self.f.seek(start)
        buf = self.f.read(end - start)
        if len(buf) < end - start:
            raise IOError(
                f"short read at offset {offset} (wanted {end - start}, got {len(buf)}) "
                f"- likely past the device's reported capacity"
            )
        lo = offset - start
        return buf[lo:lo + length]

    def close(self):
        self.f.close()


class Fat32:
    def __init__(self, disk, part_lba):
        self.d = disk
        self.base = part_lba * SECTOR
        b = self.d.read_at(self.base, SECTOR)

        self.bytes_per_sec = struct.unpack_from("<H", b, 11)[0]
        self.sec_per_clus = b[13]
        self.reserved = struct.unpack_from("<H", b, 14)[0]
        self.num_fats = b[16]
        self.sec_per_fat = struct.unpack_from("<I", b, 36)[0]
        self.root_clus = struct.unpack_from("<I", b, 44)[0]

        if self.bytes_per_sec != SECTOR:
            raise ValueError(f"unexpected bytes/sector {self.bytes_per_sec}")
        if self.sec_per_clus == 0:
            raise ValueError("bad sectors/cluster")

        self.fat_start = self.base + self.reserved * SECTOR
        self.data_start = self.fat_start + self.num_fats * self.sec_per_fat * SECTOR
        self.cluster_bytes = self.sec_per_clus * SECTOR

    def info(self):
        return (
            f"bytes/sec={self.bytes_per_sec} sec/clus={self.sec_per_clus} "
            f"reserved={self.reserved} fats={self.num_fats} "
            f"sec/fat={self.sec_per_fat} root_clus={self.root_clus} "
            f"cluster={self.cluster_bytes}B"
        )

    def next_cluster(self, clus):
        off = self.fat_start + clus * 4
        return struct.unpack_from("<I", self.d.read_at(off, 4), 0)[0] & 0x0FFFFFFF

    def chain(self, clus, cap=1 << 20):
        out = []
        seen = set()
        while 2 <= clus < 0x0FFFFFF8 and len(out) < cap:
            if clus in seen:
                break                      # cycle guard on a damaged FAT
            seen.add(clus)
            out.append(clus)
            clus = self.next_cluster(clus)
        return out

    def cluster_offset(self, clus):
        return self.data_start + (clus - 2) * self.cluster_bytes

    def read_chain(self, clus, size=None):
        data = bytearray()
        for c in self.chain(clus):
            data += self.d.read_at(self.cluster_offset(c), self.cluster_bytes)
            if size is not None and len(data) >= size:
                break
        return bytes(data[:size]) if size is not None else bytes(data)

    def list_dir(self, clus):
        """Yield (name, is_dir, size, first_cluster). Handles LFN entries."""
        raw = self.read_chain(clus)
        entries = []
        lfn = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if len(e) < 32 or e[0] == 0x00:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            attr = e[11]
            if attr == 0x0F:                                   # long-file-name chunk
                part = e[1:11] + e[14:26] + e[28:32]
                try:
                    s = part.decode("utf-16-le")
                except UnicodeDecodeError:
                    s = ""
                lfn.insert(0, s.split("\x00")[0])
                continue
            if attr & 0x08:                                    # volume label
                lfn = []
                continue

            if lfn:
                name = "".join(lfn)
            else:
                stem = e[0:8].decode("latin1").rstrip()
                ext = e[8:11].decode("latin1").rstrip()
                name = f"{stem}.{ext}" if ext else stem
            lfn = []

            first = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            entries.append((name, bool(attr & 0x10), size, first))
        return entries

    def walk(self, clus, path="", depth=0, out=None, max_depth=12):
        if out is None:
            out = []
        if depth > max_depth:
            return out
        for name, is_dir, size, first in self.list_dir(clus):
            if name in (".", ".."):
                continue
            full = f"{path}/{name}"
            out.append((full, is_dir, size, first))
            if is_dir and first >= 2:
                self.walk(first, full, depth + 1, out, max_depth)
        return out


def find_disk_and_partition():
    """Locate the Electra disk number via WMI, then read its MBR for the first partition."""
    import subprocess
    ps = (
        "Get-Disk | Where-Object { $_.FriendlyName -match 'ELECTRA' } | "
        "Select-Object -First 1 -ExpandProperty Number"
    )
    num = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                         capture_output=True, text=True).stdout.strip()
    if not num.isdigit():
        raise SystemExit("Could not find a disk named ELECTRA. Still in USB DISK MODE?")
    path = rf"\\.\PhysicalDrive{num}"
    d = RawDisk(path)
    mbr = d.read_at(0, SECTOR)
    if mbr[510:512] != b"\x55\xaa":
        raise SystemExit("sector 0 has no 55AA signature")
    for i in range(4):
        o = 446 + i * 16
        ptype = mbr[o + 4]
        lba = struct.unpack_from("<I", mbr, o + 8)[0]
        if ptype in (0x0B, 0x0C, 0x06, 0x04, 0x0E) and lba:
            return d, path, lba
    raise SystemExit("no FAT partition entry found in the MBR")


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "list"
    disk, path, lba = find_disk_and_partition()
    print(f"device    : {path}")
    print(f"partition : LBA {lba}")
    fs = Fat32(disk, lba)
    print(f"fat32     : {fs.info()}\n")

    tree = fs.walk(fs.root_clus)
    files = [t for t in tree if not t[1]]
    dirs = [t for t in tree if t[1]]
    total = sum(t[2] for t in files)
    print(f"{len(dirs)} directories, {len(files)} files, {total:,} bytes\n")

    if mode == "list":
        for full, is_dir, size, _ in tree:
            print(f"  {'DIR ' if is_dir else '    '} {size:>10,}  {full}")
        print("\nRead-only. Nothing was written.")
        return

    if mode == "copy":
        if len(sys.argv) < 3:
            raise SystemExit("usage: python fat32.py copy <dest-dir>")
        dest = os.path.abspath(sys.argv[2])
        ok = failed = 0
        for full, is_dir, size, first in tree:
            target = os.path.join(dest, full.lstrip("/").replace("/", os.sep))
            if is_dir:
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            try:
                data = fs.read_chain(first, size) if size and first >= 2 else b""
                with open(target, "wb") as f:
                    f.write(data)
                ok += 1
            except Exception as e:
                print(f"  FAILED {full}: {e}")
                failed += 1
        print(f"\ncopied {ok} files to {dest}" + (f", {failed} failed" if failed else ""))
        print("Read-only on the device. Nothing was written to it.")
        return

    raise SystemExit(f"unknown mode {mode!r}")


if __name__ == "__main__":
    main()
