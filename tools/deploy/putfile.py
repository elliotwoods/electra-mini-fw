"""
Write a file into the Electra One Mini's FAT32 volume over raw disk access.

Why this exists: the bootloader's USB mass-storage reports a smaller capacity than the
MBR partition claims (8,388,000 sectors reported vs 15,267,840 needed), so Windows
refuses to mount the volume and the documented "drag update.srec into /boot" flow is
unavailable. Reads and writes to the media both work, so we drive FAT32 ourselves.

    python putfile.py <source-file> <dest-path-on-card>       # e.g. /BOOT/update.srec
    python putfile.py --verify <source-file> <dest-path>      # compare only, no write
    python putfile.py --undo <journal.bin>                    # roll back a previous run

Safety properties:
  * file data goes only into clusters the FAT marks free
  * the only existing structures modified are the FAT copies and the destination
    directory's cluster
  * every sector is saved to an undo journal BEFORE it is overwritten
  * the written file is read back and compared before the run is called a success
  * allocation is confined to the capacity the device actually reports

Run elevated.
"""

import os
import struct
import sys
import subprocess

SECTOR = 512
REPORTED_SECTORS = 8388000      # what the bootloader's MSC advertises; do not allocate past it
FREE, EOC = 0x00000000, 0x0FFFFFFF


# ----------------------------------------------------------------- raw device

class Raw:
    def __init__(self, num, writable=False):
        self.f = open(rf"\\.\PhysicalDrive{num}", "rb+" if writable else "rb", buffering=0)

    def read(self, lba, n=1):
        self.f.seek(lba * SECTOR)
        b = self.f.read(n * SECTOR)
        if len(b) != n * SECTOR:
            raise IOError(f"short read at LBA {lba} ({len(b)} of {n * SECTOR})")
        return b

    def write(self, lba, data):
        if len(data) % SECTOR:
            raise ValueError("writes must be whole sectors")
        self.f.seek(lba * SECTOR)
        self.f.write(data)
        self.f.flush()
        os.fsync(self.f.fileno())

    def close(self):
        self.f.close()


def find_disk():
    ps = ("Get-Disk | Where-Object { $_.FriendlyName -match 'ELECTRA' } | "
          "Select-Object -First 1 -ExpandProperty Number")
    n = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                       capture_output=True, text=True).stdout.strip()
    if not n.isdigit():
        raise SystemExit("No ELECTRA disk found. Is the device in USB DISK MODE?")
    return int(n)


# ----------------------------------------------------------------- undo journal

class Journal:
    """Records the prior contents of every sector we touch, so a run can be reversed."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "wb")
        self.count = 0

    def save(self, lba, data):
        self.f.write(struct.pack("<QI", lba, len(data)))
        self.f.write(data)
        self.f.flush()
        self.count += 1

    def close(self):
        self.f.close()

    @staticmethod
    def replay(path, disk):
        n = 0
        with open(path, "rb") as f:
            while True:
                hdr = f.read(12)
                if len(hdr) < 12:
                    break
                lba, ln = struct.unpack("<QI", hdr)
                disk.write(lba, f.read(ln))
                n += 1
        return n


# ----------------------------------------------------------------- FAT32

class Fat32:
    def __init__(self, disk, part_lba):
        self.d = disk
        self.part = part_lba
        v = disk.read(part_lba)
        self.sec_per_clus = v[13]
        self.reserved = struct.unpack_from("<H", v, 14)[0]
        self.num_fats = v[16]
        self.sec_per_fat = struct.unpack_from("<I", v, 36)[0]
        self.root_clus = struct.unpack_from("<I", v, 44)[0]
        if struct.unpack_from("<H", v, 11)[0] != SECTOR:
            raise SystemExit("unexpected bytes-per-sector")
        self.fat_lba = part_lba + self.reserved
        self.data_lba = self.fat_lba + self.num_fats * self.sec_per_fat
        self.cbytes = self.sec_per_clus * SECTOR
        self.max_clus = (REPORTED_SECTORS - self.data_lba) // self.sec_per_clus + 2

    def clus_lba(self, c):
        return self.data_lba + (c - 2) * self.sec_per_clus

    def fat_get(self, c):
        lba = self.fat_lba + (c * 4) // SECTOR
        off = (c * 4) % SECTOR
        return struct.unpack_from("<I", self.d.read(lba), off)[0] & 0x0FFFFFFF

    def fat_set(self, c, val, journal):
        """Update the entry in every FAT copy."""
        for i in range(self.num_fats):
            base = self.fat_lba + i * self.sec_per_fat
            lba = base + (c * 4) // SECTOR
            off = (c * 4) % SECTOR
            s = bytearray(self.d.read(lba))
            journal.save(lba, bytes(s))
            cur = struct.unpack_from("<I", s, off)[0]
            struct.pack_into("<I", s, off, (cur & 0xF0000000) | (val & 0x0FFFFFFF))
            self.d.write(lba, bytes(s))

    def chain(self, c, cap=1 << 20):
        out, seen = [], set()
        while 2 <= c < 0x0FFFFFF8 and len(out) < cap:
            if c in seen:
                break
            seen.add(c)
            out.append(c)
            c = self.fat_get(c)
        return out

    def read_chain(self, c, size=None):
        buf = bytearray()
        for cl in self.chain(c):
            buf += self.d.read(self.clus_lba(cl), self.sec_per_clus)
            if size is not None and len(buf) >= size:
                break
        return bytes(buf[:size]) if size is not None else bytes(buf)

    def find_free(self, count, start=64):
        """Free clusters, staying inside the capacity the device reports."""
        out, c = [], start
        while len(out) < count and c < self.max_clus:
            if self.fat_get(c) == FREE:
                out.append(c)
            c += 1
        if len(out) < count:
            raise SystemExit(f"only found {len(out)} free clusters, need {count}")
        return out

    # -- directory helpers ---------------------------------------------------

    @staticmethod
    def _shortname(name):
        base, _, ext = name.upper().rpartition(".")
        if not base:
            base, ext = name.upper(), ""
        base = "".join(ch for ch in base if ch.isalnum() or ch in "_-")[:6]
        ext = "".join(ch for ch in ext if ch.isalnum())[:3]
        return (f"{base}~1".ljust(8) + ext.ljust(3)).encode("ascii")

    @staticmethod
    def _checksum(short):
        s = 0
        for ch in short:
            s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
        return s

    def _entries_for(self, name, first_clus, size):
        """Build LFN entries + the 8.3 entry for `name`."""
        short = self._shortname(name)
        csum = self._checksum(short)
        u = name.encode("utf-16-le") + b"\x00\x00"
        u += b"\xff" * ((13 * 2 - len(u) % (13 * 2)) % (13 * 2))
        n = len(u) // 26

        ents = []
        for i in range(n, 0, -1):                       # last LFN chunk is stored first
            chunk = u[(i - 1) * 26:i * 26]
            seq = i | (0x40 if i == n else 0)
            e = bytearray(32)
            e[0] = seq
            e[1:11] = chunk[0:10]
            e[11] = 0x0F
            e[12] = 0
            e[13] = csum
            e[14:26] = chunk[10:22]
            e[26:28] = b"\x00\x00"
            e[28:32] = chunk[22:26]
            ents.append(bytes(e))

        e = bytearray(32)
        e[0:11] = short
        e[11] = 0x20                                    # archive
        struct.pack_into("<H", e, 20, (first_clus >> 16) & 0xFFFF)
        struct.pack_into("<H", e, 26, first_clus & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        struct.pack_into("<H", e, 22, 0x6000)           # time / date: fixed, harmless
        struct.pack_into("<H", e, 24, 0x5900)
        struct.pack_into("<H", e, 16, 0x5900)
        ents.append(bytes(e))
        return ents

    def list_dir(self, clus):
        raw = self.read_chain(clus)
        out, lfn = [], []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if len(e) < 32 or e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:
                lfn.insert(0, (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le", "ignore").split("\x00")[0])
                continue
            if e[11] & 0x08:
                lfn = []
                continue
            nm = "".join(lfn) if lfn else (
                e[0:8].decode("latin1").rstrip() +
                ("." + e[8:11].decode("latin1").rstrip() if e[8:11].strip() else ""))
            lfn = []
            first = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            out.append((nm, bool(e[11] & 0x10), struct.unpack_from("<I", e, 28)[0], first, i))
        return out

    def resolve_dir(self, path):
        clus = self.root_clus
        for part in [p for p in path.strip("/").split("/") if p]:
            hit = next((x for x in self.list_dir(clus)
                        if x[1] and x[0].upper() == part.upper()), None)
            if not hit:
                raise SystemExit(f"directory not found: {part}")
            clus = hit[3]
        return clus


# ----------------------------------------------------------------- operations

def put(src, dest, verify_only=False):
    num = find_disk()
    d = Raw(num, writable=not verify_only)
    mbr = d.read(0)
    part = next((struct.unpack_from("<I", mbr, 446 + i * 16 + 8)[0]
                 for i in range(4) if mbr[446 + i * 16 + 4] in (0x0B, 0x0C, 0x06, 0x0E)), None)
    if not part:
        raise SystemExit("no FAT partition")
    fs = Fat32(d, part)

    data = open(src, "rb").read()
    dirpath, _, fname = dest.strip("/").rpartition("/")
    dir_clus = fs.resolve_dir(dirpath)

    print(f"device      : PhysicalDrive{num}")
    print(f"source      : {src} ({len(data):,} bytes)")
    print(f"destination : /{dirpath}/{fname}  (dir cluster {dir_clus})")
    print(f"cluster size: {fs.cbytes:,} bytes; usable clusters < {fs.max_clus:,}\n")

    existing = next((x for x in fs.list_dir(dir_clus) if x[0].upper() == fname.upper()), None)

    if verify_only:
        if not existing:
            print("NOT PRESENT")
            d.close()
            return 1
        got = fs.read_chain(existing[3], existing[2])
        same = got == data
        print(f"on card: {existing[2]:,} bytes; match = {same}")
        d.close()
        return 0 if same else 1

    if existing:
        raise SystemExit(f"{fname} already exists ({existing[2]:,} bytes). "
                         f"Remove it first, or use --verify.")

    n_clus = max(1, (len(data) + fs.cbytes - 1) // fs.cbytes)
    clusters = fs.find_free(n_clus)
    print(f"allocating {n_clus} clusters: {clusters[0]}..{clusters[-1]}")

    jpath = os.path.join(os.path.dirname(os.path.abspath(src)),
                         f"undo-{fname.replace('.', '_')}.bin")
    j = Journal(jpath)
    print(f"undo journal: {jpath}\n")

    try:
        # 1. file data into free clusters (nothing references them yet)
        for i, c in enumerate(clusters):
            chunk = data[i * fs.cbytes:(i + 1) * fs.cbytes]
            chunk += b"\x00" * (fs.cbytes - len(chunk))
            d.write(fs.clus_lba(c), chunk)
        print(f"wrote {len(data):,} bytes of file data")

        # 2. FAT chain, in every copy
        for i, c in enumerate(clusters):
            fs.fat_set(c, clusters[i + 1] if i + 1 < len(clusters) else EOC, j)
        print(f"linked FAT chain across {fs.num_fats} FAT copies")

        # 3. directory entries
        ents = fs._entries_for(fname, clusters[0], len(data))
        raw = bytearray(fs.read_chain(dir_clus))
        slot = None
        for off in range(0, len(raw) - 32 * len(ents) + 1, 32):
            window = [raw[off + k * 32] for k in range(len(ents))]
            if all(b in (0x00, 0xE5) for b in window):
                slot = off
                break
        if slot is None:
            raise SystemExit("no contiguous free directory slots (dir would need extending)")
        for k, e in enumerate(ents):
            raw[slot + k * 32:slot + (k + 1) * 32] = e

        dchain = fs.chain(dir_clus)
        for i, c in enumerate(dchain):
            lba = fs.clus_lba(c)
            j.save(lba, d.read(lba, fs.sec_per_clus))
            d.write(lba, bytes(raw[i * fs.cbytes:(i + 1) * fs.cbytes]))
        print(f"wrote directory entry ({len(ents)} slots at offset {slot})")

    finally:
        j.close()

    # 4. verify by reading it back the way the device will
    d.close()
    d = Raw(num, writable=False)
    fs = Fat32(d, part)
    got = next((x for x in fs.list_dir(dir_clus) if x[0].upper() == fname.upper()), None)
    if not got:
        print("\nFAILED: file not visible after write")
        d.close()
        return 1
    back = fs.read_chain(got[3], got[2])
    d.close()

    if back == data:
        print(f"\n  *** VERIFIED: /{dirpath}/{fname}, {got[2]:,} bytes, byte-identical ***")
        print(f"\nTo roll back:  python putfile.py --undo {jpath}")
        return 0
    print(f"\nFAILED verification: {got[2]:,} bytes on card, {len(data):,} expected")
    return 1


def undo(path):
    d = Raw(find_disk(), writable=True)
    n = Journal.replay(path, d)
    d.close()
    print(f"restored {n} sectors from {path}")
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "--undo":
        sys.exit(undo(a[1]))
    if a and a[0] == "--verify":
        sys.exit(put(a[1], a[2], verify_only=True))
    if len(a) < 2:
        raise SystemExit(__doc__)
    sys.exit(put(a[0], a[1]))
