# Recovery

Read this before flashing anything. It is the procedure that makes every other mistake reversible.

The device is an **Electra One Mini** — a Renesas Synergy S7G2. Its bootloader lives at
`0x00000000`–`0x000FFFFF` and our application at `0x00100000`. The bootloader has its own
screen, USB stack, SD-card access and S-record flasher, none of it borrowed from the
application. That is why a completely dead application is still recoverable: getting into the
bootloader does not depend on any of our code being correct.

---

## Three tiers of image, and which loop replaces each

| Image | Address | Replaced by | Needs hands? |
|---|---|---|---|
| Electra's bootloader | `0x00000000` | nothing — we never touch it | — |
| **Our bootloader (EMB)** | `0x00100000` | SD card → `/BOOT/update.srec` | yes, the chord |
| **Application** | `0x00120000` | `tools/deploy/flash_usb.py` over USB | **no** |

The split is the whole point. Our bootloader goes on once; after that the application is
replaced over USB with no button, no card and no re-plugging:

```powershell
python tools\deploy\flash_usb.py --reboot          # app returns to the bootloader
python tools\deploy\flash_usb.py build\app.srec    # erase, write, verify, run
python tools\deploy\flash_usb.py --console         # interactive terminal
```

`--reboot` works because the application writes a magic word into 32 bytes of SRAM that
survive a warm reset, then resets itself; our bootloader reads it and stays in flash mode.

**A broken application cannot lock you out.** The bootloader refuses to launch an image whose
vector table is implausible, counts launches and stops after three that never report healthy,
and gives a host a window at every boot to claim the device. Worst case is a reboot, not a
recovery cycle.

**A broken EMB bootloader** costs one SD-card cycle — the stock bootloader always programs
whatever is at `/BOOT/update.srec`, and our bootloader lives at exactly the address it writes.
So the tier below always rescues the tier above.

## The one rule

**Never write `/boot/blupdate.srec`.**

| File | What it flashes | Written by |
|---|---|---|
| `/boot/update.srec` | the **application** — ours | the bootloader |
| `/boot/blupdate.srec` | the **bootloader** itself | the application |

The two halves flash each other. Replacing the application is always reversible because the
bootloader survives to do it. Replacing the bootloader is the one operation with nothing behind
it, and this project never needs to. If you find yourself about to copy a file named
`blupdate.srec`, stop.

---

## Tier 1 — application is broken (expected path, case stays shut)

This is the recovery you will actually use. It works even if our firmware crashes instantly,
hangs, or never reaches `main`.

### Entering USB DISK MODE

1. **Disconnect USB.**
2. **Press and hold the reboot button** — the same button the system menu uses to reboot
   (5th in the front-panel row). Confirmed working on this unit.

   > The mk2 documentation says "RIGHT-BOTTOM". That is for a 2×3 grid; the Mini has one row
   > and the mk2 chord does nothing here.

3. **While still holding**, power the device: reconnect USB, or press the **`RESET`** button on
   the **rear panel** (it only power-cycles).
4. **Keep holding** until the screen shows **`USB DISK MODE`**.

### Getting a file onto the card — Windows CANNOT mount this volume

**Do not expect a drive letter.** On this unit the bootloader's mass-storage advertises
**8,388,000 sectors (4,095.7 MB)** while the MBR partition claims **15,259,648 sectors
(7,451 MB)**. The partition runs off the end of the reported device, so Windows refuses to mount
it — you get `ELECTRA MSC` in Disk Management with no partition and no letter. This is a
bootloader bug, not a damaged card: reads and writes both work, and the FAT32 filesystem is
intact.

So we drive FAT32 directly. From an **elevated** shell:

```powershell
# what is on the card
python tools\diag\fat32.py list

# full backup
python tools\diag\fat32.py copy <backup-directory>

# stage a firmware image (gate it first)
node tools\mkimage\check-image.js build\app.srec
python tools\deploy\putfile.py build\app.srec /BOOT/update.srec

# confirm it landed byte-identical
python tools\deploy\putfile.py --verify build\app.srec /BOOT/update.srec

# undo the last write, if needed
python tools\deploy\putfile.py --undo <undo-*.bin>
```

`putfile.py` writes file data only into clusters the FAT marks free, saves every sector it is
about to overwrite into an undo journal, and reads the file back before reporting success.

Then **eject and reconnect USB**. The bootloader finds `/BOOT/update.srec` and reflashes —
**about 2–3 minutes**. Do not interrupt it.

> **A staged `update.srec` arms the next power-on.** Once the file is on the card, *any*
> reconnect triggers a flash, not just a deliberate one. Do not leave one staged unless you mean
> it.

### If it boots to a hang instead

Re-enter `USB DISK MODE` and delete `/BOOT/update.srec` (roll back with the undo journal, or
extend `putfile.py`). An empty `/BOOT` means the bootloader has nothing to apply and falls
through to whatever application is already in flash.

### If `USB DISK MODE` never appears

Per Electra's own documentation this usually means a **dead SD card**, not a dead MCU — the
recovery path itself depends on the card being readable. This is why a card backup matters. Do not
escalate to tier 2 on this symptom alone; suspect the card first.

---

## Tier 2 — bootloader is broken (should never happen)

Only reachable if `blupdate.srec` was written and failed. We do not do that, so this is
insurance, not procedure.

The S7G2 has a **factory USB boot mode**: pull the **MD pin low at reset** and the chip
enumerates as *Synergy USB Boot*, after which **Renesas Flash Programmer** can write internal
flash **with no debug probe**. Electra publish `recovery-v4.0.0.mot` for exactly this case.

Requires opening the unit and finding the MD pin. **Confirm it is physically reachable before
M1**, so this tier is known-available rather than assumed.

## Tier 3

Electra state they can un-brick units on Windows or Linux. Support ticket.

---

## Card backup

Keep a byte-complete copy of the `ELECTRA` volume, taken while the device was
known-good. It is git-ignored because of its size — **it is not backed up by this repo, so do
not let it be the only copy.**

Taken 2026-08-16 by `tools/diag/fat32.py copy`, straight off the raw device:
**220 files, 3,141,985 bytes**, verified against the source listing. At that point the card had:

- `/BOOT` — empty
- `/CACHE` — empty
- `/ASSETS/ui-0.9.6.bmp` — 1,228,866 B; 1024×600 RGB565. **This is the full-size Electra One's
  UI atlas, not the Mini's screen geometry.** The Mini's panel is 800×480.
- `/CTRLV2/config4.cfg` (1,083 B), `/CTRLV2/MASTER4.DB` (24,576 B)
- Five populated preset banks, `B00`–`B04`
- The `av-frameworks` surface bundle at `B02/P00/main.lua` (43,917 B, `BUNDLE_VERSION = 7`)
- macOS residue — `.DS_Store`, `._*` resource forks, `.Spotlight-V100`, `.fseventsd` — so the
  card has been mounted on a Mac at some point

Stock firmware images are published at
<https://docs.electra.one/downloads/firmware.html> (`firmware-mini-v4.1.4.srec.zip`, and older
versions back to 4.1.2). Keep a copy of the stock `.srec` with that backup too — recovery is much
less stressful when you are not also waiting on a download.

---

## Rule for every flash from M1 onward

After flashing our own image, **verify recovery still works** before moving on: reflash stock,
confirm a normal boot, then flash ours again. Proving the loop is cheap. Discovering it is
broken while you need it is not.
