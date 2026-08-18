"""
Does the Electra bootloader's USB mass-storage accept WRITES?

This is the gating question for the whole deploy story. Windows cannot mount the volume
(the MBR partition claims more sectors than the device reports), so the documented
"drag update.srec into /boot" flow is unavailable. Before building a raw FAT32 writer we
need to know whether writes reach the media at all.

Method, chosen to be harmless:
  * find a cluster the FAT marks FREE, well past anything in use
  * save its current contents
  * write a recognisable pattern
  * flush, re-read, compare
  * restore the original bytes and verify the restore

An unallocated cluster belongs to no file, so even a crash mid-test leaves the filesystem
structurally untouched. No FAT entry and no directory entry is modified.

Run elevated:  python writetest.py
"""

import struct
import sys

SECTOR = 512
PATTERN = b"ELECTRA-MINI-FW WRITE TEST -- if you are reading this in a file, "
PATTERN += b"the cluster was not free and something is wrong. " + b"\xa5" * 64


def find_disk():
    import subprocess
    ps = ("Get-Disk | Where-Object { $_.FriendlyName -match 'ELECTRA' } | "
          "Select-Object -First 1 -ExpandProperty Number")
    n = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                       capture_output=True, text=True).stdout.strip()
    if not n.isdigit():
        raise SystemExit("No ELECTRA disk found. Still in USB DISK MODE?")
    return int(n)


class Raw:
    def __init__(self, num, writable):
        mode = "rb+" if writable else "rb"
        self.f = open(rf"\\.\PhysicalDrive{num}", mode, buffering=0)

    def read(self, lba, n_sectors):
        self.f.seek(lba * SECTOR)
        b = self.f.read(n_sectors * SECTOR)
        if len(b) != n_sectors * SECTOR:
            raise IOError(f"short read at LBA {lba}")
        return b

    def write(self, lba, data):
        if len(data) % SECTOR:
            raise ValueError("writes must be a whole number of sectors")
        self.f.seek(lba * SECTOR)
        self.f.write(data)
        self.f.flush()

    def close(self):
        self.f.close()


def main():
    num = find_disk()
    print(f"device: \\\\.\\PhysicalDrive{num}\n")

    # --- locate the FAT32 volume -------------------------------------------------
    d = Raw(num, writable=False)
    mbr = d.read(0, 1)
    part_lba = None
    for i in range(4):
        o = 446 + i * 16
        if mbr[o + 4] in (0x0B, 0x0C, 0x06, 0x0E):
            part_lba = struct.unpack_from("<I", mbr, o + 8)[0]
            break
    if not part_lba:
        raise SystemExit("no FAT partition in MBR")

    vbr = d.read(part_lba, 1)
    sec_per_clus = vbr[13]
    reserved = struct.unpack_from("<H", vbr, 14)[0]
    num_fats = vbr[16]
    sec_per_fat = struct.unpack_from("<I", vbr, 36)[0]

    fat_lba = part_lba + reserved
    data_lba = fat_lba + num_fats * sec_per_fat
    disk_sectors = 8388000  # what the device reports; stay inside it

    print(f"partition LBA {part_lba}, cluster = {sec_per_clus} sectors, "
          f"FAT at {fat_lba}, data at {data_lba}")

    # --- find a free cluster, well clear of anything in use ----------------------
    # ~3.1 MB is in use => under ~110 clusters. Start looking much later.
    start_cluster = 4000
    fat = d.read(fat_lba, 256)  # 256 sectors = 32768 FAT entries
    free = None
    for c in range(start_cluster, 32768):
        if struct.unpack_from("<I", fat, c * 4)[0] & 0x0FFFFFFF == 0:
            lba = data_lba + (c - 2) * sec_per_clus
            if lba + sec_per_clus < disk_sectors:
                free = (c, lba)
                break
    if not free:
        raise SystemExit("no free cluster found in the scanned range")
    clus, lba = free
    print(f"using FREE cluster {clus} at LBA {lba} "
          f"({sec_per_clus} sectors, {sec_per_clus * SECTOR} bytes)\n")
    d.close()

    # --- save, write, verify, restore -------------------------------------------
    d = Raw(num, writable=True)
    ok = False
    original = None
    try:
        original = d.read(lba, sec_per_clus)
        print(f"saved original {len(original)} bytes")

        payload = bytearray(original)
        payload[0:len(PATTERN)] = PATTERN
        d.write(lba, bytes(payload))
        print("wrote pattern")

        # Re-open to defeat any host-side caching of the read path.
        d.close()
        d = Raw(num, writable=True)
        back = d.read(lba, sec_per_clus)

        if back[:len(PATTERN)] == PATTERN:
            print("\n  *** WRITES WORK -- pattern read back correctly ***")
            ok = True
        else:
            print("\n  *** WRITES DO NOT REACH THE MEDIA ***")
            print(f"      expected: {PATTERN[:32]!r}")
            print(f"      got     : {back[:32]!r}")
    except PermissionError as e:
        print(f"\n  *** WRITE REJECTED BY THE DEVICE OR OS: {e}")
    except OSError as e:
        print(f"\n  *** WRITE FAILED: {e}")
    finally:
        if original is not None:
            try:
                d.write(lba, original)
                d.close()
                d = Raw(num, writable=False)
                if d.read(lba, sec_per_clus) == original:
                    print("restored original bytes, verified")
                else:
                    print("WARNING: restore did NOT verify -- but the cluster is "
                          "unallocated, so no file is affected")
            except Exception as e:
                print(f"WARNING: could not restore ({e}) -- cluster is unallocated, "
                      f"so no file is affected")
        d.close()

    print("\nNo FAT entry and no directory entry was modified.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
