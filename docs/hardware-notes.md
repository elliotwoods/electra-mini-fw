# Hardware notes

Findings about the Electra One Mini, derived from analysis of the stock firmware v4.1.4, the
SD-card contents, and vendor documentation.

Every claim here should be checkable. Addresses are offsets into the stock image, quoted so a
finding can be re-derived rather than taken on trust. Claims are marked **[certain]** (directly
observed) or **[inferred]** (reasoned from evidence, not yet confirmed on hardware) -- and where
a later measurement contradicted an early reading, both are kept, because knowing which
inferences failed is worth more than a tidy document.

---

## Part and memory map

**[certain]** Renesas Synergy **S7G2**, Cortex-M4F. Confirmed by the banner string at
`0x0031D92C`: *"Built with Renesas Synergy (TM) Software Package version 2.4.0+build.dc9b89f…"*,
and corroborated by the feature set the firmware uses (D/AVE 2D via the `d1 heap` /
`g_d1_isr_semaphore` objects, USB host + device, SDHI, 4 MB-class flash).

| Region | Range | Notes |
|---|---|---|
| Bootloader | `0x00000000`–`0x000FFFFF` | Not in our dump. Sets `VTOR`; owns OFS/ID-code at `0x00000400`. |
| Application | `0x00100000`–`0x003AE3DC` | 2,745 KiB. This is what we replace. |
| Free flash | `0x003AE3DC`–`0x00400000` | 4 MB part |
| SRAM | `0x1FFE0000`–`0x20080000` | 640 KB. Initial SP `0x20010360`. |
| **QSPI serial flash** | `0x60000000` (regs `0x64000000`) | quad + 4-byte addressing, XIP — part ID below |
| **SDRAM** | `0x90000000` | External, 16-bit bus, controller at `0x40003C00` |
| Data flash | `0x40100000` | E2 emulation |
| Factory MCU info | `0x407FB19C` | ROM; part-number chars validated at startup |

**[certain]** SRAM detail:

| Range | Size | Contents |
|---|---|---|
| `0x1FFE01C0`–`0x1FFE4D5B` | 0x4B9C | `.data`, loaded from flash `0x003A9840` |
| `0x1FFE4D5C`–`0x1FFE4D9F` | 68 B | no-init: canary seeds, SSP feature ptr, CGC base, IOPORT bases, `'IMF'` magic |
| `0x1FFE4DA0`–`0x2000C35B` | 0x275BC | `.bss` |
| `0x2000C360`–`0x2001035F` | 16 KB | main stack (MSP top `0x20010360`) |
| `0x20014360`–`0x2003035F` | 112 KB | ThreadX thread stacks |

**[certain]** SDRAM is where the graphics memory lives — which resolves the framebuffer
question. `tx_byte_pool_create(&pool, "d1 heap", 0x90000000, 0x50000)` allocates a **320 KB pool
at `0x90000000` for the D/AVE 2D engine**, and `_sbrk` is a bump allocator
running from `0x90050000` to `0x91050000` — a **16 MB C heap in SDRAM** immediately after it.

**[certain]** The unidentified `0x64000000` window is the **QSPI controller**, not the display and
not NAND. Registers decode as `SFMCOM` (`+0x10`, byte port), `SFMCMD` (`+0x14`, bit 0 = DCOM
"execute direct transfer"), `SFMCST` (`+0x18`, bit 0 = busy), `SFMSMD` (`+0x00`). the stock firmware
releases the module stop, resets the flash, reads JEDEC ID `0x9F`, enables quad + 4-byte
addressing, then enters memory-mapped read mode at `0x60000000`.

**[certain]** There are **two device paths**, selected on `DAT_1FFE4E18`, which lives in `.bss`
and is **never written** — so it is always 0 and the *else* branch is the live one:

| Path | Taken? | Expects | Distinctive opcodes |
|---|---|---|---|
| `DAT_1FFE4E18 == 0` | **yes** | `20 BA 19` = **Micron MT25QL256** (32 MB) | `0x70` Read Flag Status (Micron-specific), `0xB5`/`0xB1` RD/WR NVCR, `0xB7`/`0xE9` enter/exit 4-byte |
| `DAT_1FFE4E18 == 1` | dead | `C2 20 1B/18` = Macronix MX25 | `0xAF` RDID-QPI, `0x35` EQIO |

The `0x70` write that looked like a NAND READ STATUS is Micron's **Read Flag Status Register**.

> An earlier note recorded the part as Macronix — that was the dead branch. **Confirm by reading
> the chip marking** when the unit is open; both paths exist in the binary, so the image alone
> cannot settle which part is fitted, only which one the code expects by default.

**[certain]** Entry: vector table at `0x00100000`; initial SP `0x20010360`; reset vector
`0x0010228D` (Thumb, so `0x0010228C`). The reset handler is trivial —
`push {r7,lr}`, `bl 0x001022DC`, `bl 0x001090AC` (`main`), then `b .` at `0x00102298` if `main`
ever returns.

### Startup contract — what our app must do itself

**[certain]** the stock firmware (228 B) is the real startup, in this order:

1. **FPU enable** — `CPACR (0xE000ED88) |= 0x00F00000`. FPCCR untouched, so lazy stacking stays at reset default.
2. `R_BSP_WarmStart(PRE_CLOCK)` — empty stub.
3. Zero 12 words at `0x1FFE4DBC` (NMI callback table).
4. **SSP init** — checksums a table at `0x0037271C`, reads the factory MCU-info pointer at `0x407FB19C`, validates the part-number characters `'R','7','F','S'`, and on success sets `0x1FFE4D9C = 0x464D49` (`'IMF'`). Every later `ssp_feature_to_base_address()` requires this. Failure → `BKPT` loop.
5. **Full clock init** (see below).
6. **IOPORT/PFS** — apply the 100-pin table.
7. **SDRAM controller init** at `0x40003C00`.
8. **Zero .bss** — `0x1FFE4DA0`–`0x2000C35B`, 161,212 B.
9. **Copy .data** — flash `0x003A9840` → RAM `0x1FFE01C0`, 19,356 B. Source end is exactly `0x003AE3DC`, the end of the image.
10. **MSP monitor** (Renesas, *not* the ARM MPU) armed over `0x2000C360`–`0x2001435F`, NMI on violation.
11. `SystemCoreClock` ← ICLK freq, stored at `0x1FFE4E28`.
12. **23 C++ static constructors** — an inlined init_array loop over `0x1FFE30B4`, run after the .data copy.
13. Module-stop bookkeeping (108 module/channel pairs); no `MSTPCR` writes here — drivers release modules lazily in `open()`.
14. **25 ICU event-link slots** — `IELSR[i] (0x40006300 + 4i) = table[i] & 0x3FF` from `0x00100504`.
15. ELC open; stack-guard canary; **QSPI init**.

**[certain]** Clocks are configured from scratch, **not inherited**. CGC base `0x4001E000`,
reached through `ssp_feature_to_base_address(3)` which is why no literal appears. Main osc start
→ PLL ×20 → `SCKDIVCR`: **ICLK ÷1, PCLKA ÷2, PCLKB ÷4, PCLKC ÷4, PCLKD ÷2, BCLK ÷2, FCLK ÷4**;
`SCKDIVCR2.UCK = ÷5` (48 MHz USB, independently confirming ICLK = 240 MHz). All writes bracketed
by `PRCR (0x4001E3FE)` unlock/lock.

Wait states are raised **before** the clock goes up: `FLWT (0x4001C11C) = 2` above 160 MHz;
`SRAMWTSC (0x40002008)` bits 0–3 above 120 MHz and bit 4 above 200 MHz, guarded by
`SRAMPRCR (0x40002004) = 0xF1/0xF0`. Flash cache enabled via `FCACHEIV (0x4001C104)` /
`FCACHEE (0x4001C100)`.

**[certain]** `main` is three instructions: `__disable_irq(); _tx_initialize_kernel_enter();`.
All object creation happens in `tx_application_define` at `0x00109060` — 9 calls creating the
mutexes, queues, semaphores and 9 threads listed below. Application logic begins inside the
`Application Thread` entry at `0x00108F60`, after the scheduler starts. SysTick is programmed
from `SystemCoreClock / 1000` → **1 kHz tick**.

**[certain]** **Never done anywhere in the image:** `SCB->VTOR`, the ARM MPU, `__libc_init_array`
as a symbol (it is the inlined loop), or any stack setup (MSP comes from `vector[0]`).

---

## Boot handoff: we bring up the board ourselves

> **Correction.** An earlier pass concluded pin muxing was inherited from the bootloader. That
> was wrong. The scan behind it bounded `pin_cfg` at `< 0x08000000`, but real entries are
> `0x0b010c00`, so the run broke after 8 entries and fell under the minimum length. A relaxed
> rescan did surface it, but only the longest runs were inspected and the real table was further
> down the list. The absence of PFS literals — genuine — has an innocent explanation: the SSP
> IOPORT driver reaches the PFS block through `ssp_feature_to_base_address()`, so no literal
> base address ever appears at the call site.

**[certain]** The application configures **100 pins** itself. the stock firmware(&DAT_0031E3CC)` in
the startup path opens IOPORT and applies the table:

```
ioport_cfg_t @0x0031E3CC = { count = 100, table = 0x0031E0AC }
ioport_pin_cfg_t = { uint32_t pin_cfg; uint32_t pin; }   // 8 bytes, cfg FIRST
pin = (port << 8) | pin_number
```

Fully decoded in [`pinmap.txt`](pinmap.txt); regenerate with
`node ../tools/pinmap/decode-pinmap.js <fw.bin>`. Summary:

| Function | Count | Pins |
|---|---|---|
| External bus (`PSEL=0xB`) | 39 | P100–P107, P10B–P10F, P301–P30C, P601–P60E, P800, P801 |
| `PSEL=0x15` | 7 | P205, P206, P40A–P40E |
| GPIO out | 21 | P30D, P30E, P402–P406, P50D, P606, P607, P700–P706, P900, P901, P905, PB00 |
| IRQ inputs | 7 | P000, P001, P002, P004, P005, P200, P707 |
| `PSEL=0x11` | 4 | P500–P503 |
| `PSEL=0x5` | 4 | P109, P10A, P408, P409 |
| `PSEL=0x6` | 4 | P202, P203, P204, P207 |
| `PSEL=0x7` | 4 | P400, P401, P50B, P50C |
| **Analog** | **2** | **P00E, P00F** |
| `PSEL=0x4` | 2 | P505, P506 |
| GPIO in | 2 | P003, P201 |
| `PSEL=0x0` | 2 | P108, P300 |
| `PSEL=0x13` / `0x14` | 1 + 1 | P407 / PB01 |

**[inferred]** Reading the groups against the interrupt table: 39 external-bus pins match the
SDRAM controller init and 16-bit bus; the 4 `PSEL=0x6` pins match the two RIIC channels driving
IRQ 6–13 (pot touch + LCD touch); the 4 `PSEL=0x5` pins match the two SCI channels on IRQ 14–21
(the MIDI DIN ports); the 7 `PSEL=0x15` pins match SDHI (4-bit SD + CLK/CMD/CD).

**[inferred, high confidence]** **Only two analog inputs exist for twelve pots.** Combined with
21 GPIO outputs and the dedicated `MUX Thread`, the pots are almost certainly analog and read
through an external analog multiplexer, with GPIO driving the select lines. This is the
single most useful hint for M3.

**[certain]** `SCB->VTOR` (`0xE000ED08`) is **never written or read** anywhere in the 2.8 MB
image — zero occurrences of the literal, zero references in the whole stock image. The
bootloader must therefore already point it at `0x00100000`. This is the one thing we genuinely
inherit; keep our vector table at that address, or set VTOR ourselves rather than trusting it.

---

## Display

> **Correction — the panel is 800 × 480, not 1024 × 600.** An earlier note took the resolution
> from `/assets/ui-0.9.6.bmp` (1024 × 600 RGB565). That asset is inherited from the full-size
> Electra One. The Mini's actual panel registers are programmed to 800 × 480. Do not size
> anything from that BMP.

**[certain]** **800 × 480 @ 59.84 Hz, RGB565.** Decoded from the panel-config struct in the
`.data` ROM image at `0x003AA7A4` (RAM `0x1FFE1124`), and arithmetic-checked independently:

| Field | Value |
|---|---|
| Width × Height | 800 × 480 |
| Pixel clock | 33,300 kHz |
| HFP / HBP / HSW | 40 / 88 / 128 |
| VFP / VBP / VSW | 13 / 32 / 2 |
| Totals | 1056 × 527 = 556,512 px/frame |
| Implied refresh | 33.3 MHz ÷ 556,512 = **59.84 Hz** ✔ |

Corroborated by the boot splash centring text in width 800, and the logo blit streaming
`0x640` = 1600 bytes per row (= 800 px × 2 B).

**[certain]** The panel is driven by an **external RAiO RA8876 with its own 16 MB SDRAM frame
RAM**, which generates all panel timing and scans out by itself. **The MCU's GLCDC is not used
at all** — zero references to the `0x400E0000` block across all 36 segments. There is no
MCU-side framebuffer: budget **zero** MCU SDRAM for pixels.

**[certain]** Transport is **SPI, not a parallel bus**: `R_SPI1` at `0x40072100`, module-stop
`MSTPCRB` bit 18, **SPI mode 3**, `SPBR = 3` → PCLKA/8 (≈15 MHz if PCLKA = 120 MHz, *inferred*).
Constructor the stock firmware binds `R_SPI1` plus `R_PORT2`/`R_PORT3`. **RA8876 nRESET is on P313**
— the stock firmware drives it low for 5 ms, then high with a 1 ms settle.

Standard RA8876 4-wire SPI prefixes: `0x00` write cmd/addr, `0x80` write data, `0x40` read
status, `0xC0` read data. Helpers: `writeReg8` `0x001214A8`, `writeReg16` `0x001214E0`,
`writeReg32` `0x00121538`, `readStatus` `0x00121A38`.

**[certain]** RA8876 internals: 10 MHz reference crystal; frame RAM configured as 4 banks × 12
row × 9 col × 16-bit = **16 MB**; `AW_COLOR (0x5E) = 1` for 16 bpp; three 256-byte gamma LUTs
uploaded from `0x1FFE1148/0x1FFE1248/0x1FFE1348`; **backlight is the RA8876's own PWM** (compare
register `0x8C`), not an MCU GPT.

**[certain]** All drawing is **hardware-accelerated on the RA8876** — no software rasterisation
anywhere. Primitives map to its draw-control registers (`DCR0 0x67`, `DCR1 0x76`) and the
block-transfer engine (`BTE_CTRL0 0x90`, `BTE_CTRL1 0x91`). Text is per-glyph BTE copies from a
font atlas resident in VRAM page `0xE10000`. Double buffering uses `MISA (0x20)` for the scanned
page and `CVSSA (0x50)` for the drawing page.

**[certain]** The **D/AVE 2D engine is initialised but unused.** `Graphics::begin` opens the
device, allocates a 51,200-byte (800 × 32 RGB565) surface and uploads four 32×32 textures — but
there is **not a single `d2_render*` call in the image**. A leftover render path. Dropping it
reclaims the 320 KB `"d1 heap"` pool at `0x90000000` and the DRW module.

> **[certain] The hard constraint for M2 and M5.** Raw pixel pushes cost **2 SPI bytes per data
> byte** (each data byte carries an `0x80` prefix), so a full 800 × 480 frame is ~1.5 MB on the
> wire ≈ **0.8 s at 15 MHz**. Full-frame repaints from the MCU are not viable. The stock firmware
> therefore keeps every font and bitmap resident in RA8876 VRAM and composites with the BTE — any
> custom firmware must do the same.
>
> **Free win:** the bulk path today is a polled loop on `SPSR.IDLNF` with **no DMA**. Adding
> DMA/DTC is unclaimed headroom.

**[inferred, strong]** VRAM pages are sized `0x12C000` = exactly 1024 × 600 × 2, while every
hardware width register is programmed to 800 and the active window is 800 × 480 — the Mini
firmware is a port of the 1024 × 600 Electra One with page granularity left unchanged. About
40 % of each page is wasted; at 800 × 480 a page is only `0xBB800`, so ~6 MB of the 16 MB is
reclaimable.

---

## Inputs

> **Correction — there are 8 pots, not 12.** The Lua string `"potId must be between 1 and 12"`
> is stale; the actual range test in `Control.setPot` is `if (potId < 1 || 8 < potId)`, verified
> in `seg_001C0000.c:10579`. Only 8 pot objects are constructed, and the cap-touch chip is
> programmed with keys 0–7 enabled and 8–11 disabled. `POT_1`…`POT_12` constants are legacy.

### Pots — 8 endless analog pots, multiplexed into two ADCs

**[certain]** Not encoders and not absolute pots: **two-track endless potentiometers**
(Alps RDC style, two wipers producing overlapping ramps).

An external **8:1 analog mux** selects one pot. Address is driven on **P900 / P901 / P905**,
bit-packed as `(ch & 4) << 3 | (ch & 3)`; **P314** is the strobe/enable
(the stock firmware clears, the stock firmware sets). The two mux outputs land on the only two analog
pins, **P014 and P015**, read *simultaneously* by two ADC units:

- phase A → **S12AD1 channel 5**
- phase B → **S12AD0 channel 6**

Decode: deadband of 16 counts; magnitude `max(|dA|,|dB|) >> 4`; sign chosen by
quadrant using thresholds `5 / 0x200 / 0x3FE` on A with B disambiguating the half-cycle; then a
4-tap moving average. Values behave as a 0–1023 span.

Scan loop: for each of 8 channels set the mux, read the pot,
then poll both button banks; `tx_thread_sleep(12)` → **~12 ms per full scan**.

**Panel pot ↔ mux channel**:

| mux ch | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| pot ID | 0 | 4 | 5 | 1 | 6 | 3 | 7 | 2 |

### Pot capacitive touch — AT42QT2120 @ 0x1C on RIIC0

**[certain]** Microchip/Atmel **AT42QT2120**, I²C address **0x1C**, 400 kHz, on **RIIC0**
(**P400 SCL / P401 SDA**). Config struct at `0x0031E4A8`. Register map is an exact match:

| Operation | Register(s) |
|---|---|
| Reset | `0x07 = 1` |
| Calibrate | `0x06 = 1` |
| Key detect threshold, keys 0–7 | `0x10`–`0x17 = 8` |
| Key control — enable 0–7, disable 8–11 | `0x1C`–`0x23 = 0`, `0x24`–`0x27 = 1` |
| Status — 5-byte read from reg 0 | ChipID, FW ver, detection, **12-bit key status** |
| Key signals — 24-byte read from `0x34` | 12 channels × u16 **little-endian** (an earlier note said big-endian; measured values proved otherwise — see below) |

The CHANGE line is on **external IRQ channel 6 → P000**; its callback posts the
`"Pot Touch Change"` semaphore. The thread otherwise wakes on a 1000 ms timeout and does a
keep-alive read.

**Pot → QT2120 key map** (`DAT_1FFE14D4`, recovered from the `.data` ROM image at `0x003AAB54`):

| pot | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| key | 5 | 7 | 6 | 4 | 3 | 0 | 1 | 2 |

### Pot pushes and front-panel buttons — same mux, two sense pins

**[certain]** 16 button objects are built (stride `0x3C`, the stock firmware), read through the same
8:1 mux with two sense lines:

- **bank 0 → P004**: indices 0–7, of which **0–5 are the six front-panel buttons**
- **bank 1 → P003**: indices 8–15, the **8 pot push switches**, one per mux channel

State machine the stock firmware returns press / **long press (>1000 ms)** / release.

Config-name mapping: `context1`→hw id 0, `context2`→3, `context3`→4,
`context4`→5. Hardware ids **1 and 2 are the two fixed system buttons** — MENU and CONTEXT — and
are not assignable. *(Which of 1/2 is which is inferred, not proven.)*

### Touchscreen — THE MINI HAS NONE. Do not implement this.

> **Confirmed on the hardware by the device's owner (2026-08-17): the Electra One Mini has no
> touchscreen.** The FT5x06 code path in the stock image is inherited from the full-size
> Electra One, exactly like the 1024x600 `ui-0.9.6.bmp` asset that earlier led us to the wrong
> panel resolution. Two independent pieces of dead inherited code, both of which cost time.
>
> This is consistent with everything measured: bit-banging P511/P512 shows SDA never returning
> high after being pulled low, i.e. **no working pull-up**, which is what an unpopulated bus
> looks like. Every address then false-ACKs, because the master samples a line that is stuck
> low. All 18 GPIO outputs were pulsed in turn looking for an enable or reset — the technique
> that did find the QT2120's reset on P402 — with no effect whatsoever on that bus.
>
> **Lesson, now twice over: the stock firmware supports a superset of this device's hardware.**
> A driver existing in the image is not evidence the peripheral is fitted. Check for a physical
> sign of life on the bus before implementing anything established only by inspection.

The original finding is kept below for reference, because it accurately describes the full-size
Electra One and may matter if this firmware is ever ported to it.

### (Full-size Electra One only) Touchscreen — FT5x06 @ 0x38 on RIIC2

**[certain]** FocalTech **FT5x06** family, I²C address **0x38**, 400 kHz, on **RIIC2**
(**P511 SDA / P512 SCL**). Address 0x38 rules out Goodix GT911 (0x5D/0x14) and Ilitek.

One **31-byte read** from register 0 per poll. Five points at offsets `3 + 6i`
(`3, 9, 15, 21, 27`). Per point: event flag `buf[off] >> 5`, touch id `buf[off+2] >> 4`, and

```
X = 800 - (((buf[off]   & 0x0F) << 8) | buf[off+1])
Y = 480 - (((buf[off+2] & 0x0F) << 8) | buf[off+3])
```

**Both axes are inverted — the panel is mounted 180°.** This is also a third independent
confirmation of the 800 × 480 geometry. `Lcd Touch Thread` polls every **5 ms**, gated on a
data-ready flag.

**Not determined:** where the data-ready flag `DAT_20003BA0` is *set*, so the FT5x06 INT pin is
unidentified. Remaining ISEL candidates after P000: **P001 (IRQ7), P002 (IRQ8), P005 (IRQ10),
P707**.

### USB

**[certain]** **USBHS (`0x40060000`) is the host; USBFS (`0x40090000`) is the device.** Pin table
agrees: P407 `PSEL=0x13` (USBFS), P1101 `PSEL=0x14` (USBHS).

> **[certain] The device port is FULL SPEED, not High Speed.** The device descriptor at
> `0x00373897` reads `12 01 10 01 ...` — **`bcdUSB = 0x0110`**, i.e. USB 1.1. A high-speed
> capable device is required to declare `0x0200`. `bMaxPacketSize0` is 64 and the bulk
> endpoints are 64 bytes.
>
> This matters more than it looks. It caps the transport at **12 Mbit/s** — roughly
> 1 MB/s for bulk in practice, and a hard **64 bytes per 1 ms frame (64 KB/s)** for HID,
> because an interrupt endpoint is polled once per `bInterval` no matter how large the
> report is. Any plan that assumed 480 Mbit/s High Speed is wrong by a factor of 40.
>
> The project's rationale survives — 1 MB/s is still around 30× what the SysEx path
> achieves — but the bulk-vs-HID choice is now a choice between ~1 MB/s and ~64 KB/s, which
> is decisive rather than marginal.
>
> **[inferred]** Whether this is a board-routing constraint or merely the stock firmware's
> choice is not answerable from the image. The MCU has both PHYs and both are muxed
> (P407 / PB01), but they use separate pins, so the USB-C connector is almost certainly
> wired to the FS PHY alone. Confirm on the PCB before assuming High Speed is reachable.

> Note `0x40047000`/`0x40047004` are **not USB** — they are `R_MSTP` MSTPCRB/MSTPCRC
> (module-stop), which is what an earlier note guessed wrong.

Device descriptor is hard-coded at `0x00373897`: **VID `0x1FC9` (NXP), PID `0x82CF`**, bcdUSB
1.10, EP0 64 B, bus-powered 500 mA. Configuration is Audio Control + MIDIStreaming with
**3 embedded IN and 3 OUT jacks — three virtual MIDI cables** — on `EP 0x02 OUT` / `EP 0x81 IN`,
bulk, 64 bytes.

USB strings are **not** in flash: they live in **data flash** and are user-editable. Defaults
written by the stock firmware to `0x40100080` (`"Electra Controller"`, `"Electra Port 1"`,
`"Electra Port 2"`, `"Electra CTRL"`) and `0x4010F000` (serial, default `"XXX-0001"`; runtime
serial is `"EM1-%lx"` from the MCU unique ID). Each block is guarded by a `0xAA` magic byte.

### Note on SWD

**[certain]** **P108 and P300 are muxed as SWDIO / SWCLK** (`PSEL=0x00`). The signals exist and
are configured; whether they are broken out to reachable pads is a separate, physical question.

**Not yet resolved.** Whether the pots are ADC-read absolute pots or quadrature encoders; the
I²C slave addresses and part numbers of both touch controllers; how the 12 pot switches and 6
buttons are read.

---

## RTOS structure (reference only — we are not obliged to copy it)

**[certain]** ThreadX, with a thread per I/O domain:

| Thread | Paired objects |
|---|---|
| `Application Thread` | `Initialization Flags`, `Application Flags` |
| `Lcd Thread` | `Repaint Queue mutex` |
| `Lcd Touch Thread` | `g_sf_i2c_lcd_bus` |
| `Pot Touch Thread` | `g_sf_i2c_pot_touch_bus`, `Pot Touch Change` |
| `MIDI Thread` | `incoming_midi_queue`, `Sysex Mutex` |
| `MIDI IO Thread` | `outgoing_midi_io_queue`, `MIDI IO TX Complete` |
| `MIDI USB Device Thread` | `outgoing_midi_usb_dev_queue` |
| `MIDI USB Host Thread` | `outgoing_midi_usb_host_queue` |
| `MUX Thread` | — |

Global mutexes: `Malloc`, `RAM Storage`, `Lua`, `Preset`, `Midi Monitor`. FileX mounts the card
as `Volume 1` via `g_fx_sdcard`. USBX supplies both host and device MIDI classes
(`ux_host_class_midi_*`, `ux_slave_class_midi`). There is **no USB mass-storage class in the
application** — `USB DISK MODE` belongs to the bootloader.

---

## Firmware update mechanism (the channel we deploy through)

**[certain]** Two files in `/boot`, each flashed by the *other* half of the system:

| File | Flashes | Written by |
|---|---|---|
| `update.srec` | the application | the bootloader |
| `blupdate.srec` | the bootloader | the application |

the stock firmware checks for `/boot/blupdate.srec`, attempts the write up to 3 times, then deletes
the file. **We never write `blupdate.srec`** — see `../RECOVERY.md`.

**[certain]** The application's SysEx staging path: files arrive into `/cache/stage%03d.dat`,
then a commit request names `{id, location, type, md5}`. `location` maps to a directory
(`slots`→`/ctrlv2/slots`, `updates`→`/boot`, `assets`→`/assets`, `modules`→`/ctrlv2/lua`,
`presets`→`/ctrlv2/presets`, `root`→`/ctrlv2`, resolved at `0x00147A0C`); `type` maps to a
filename (`firmware`→`update.srec`, `bootloader`→`blupdate.srec`, at `0x00147A78`).

**[certain]** `verifyStagedFile` (`0x001491DC`) reads the `md5` key and hashes **only if it is
present** — the stock routine opens with `if (local_18 != 0)` where `local_18` is the digest
string. Omit the field and it returns success without hashing. Even when supplied, the digest
travels in the same request as the file, so it is a transport-integrity check, not an
authenticity one. There is no signature verification anywhere in the application image.

---

## Peripheral addresses observed

Referenced by the application, **not yet cross-referenced against the S7G2 hardware manual** —
treat this as a to-do list for M1/M3, not as a map.

```
0xE000E000  Cortex-M NVIC/SCB (77 references — by far the most used)
0x40006xxx  45 refs        0x40060000  44 refs        0x40003Cxx  41 refs
0x4001Exxx  17 refs        0x40002xxx  14 refs        0x40090000  12 refs
0x4001Cxxx  11 refs        0x40047000  10 refs        0x40100000   9 refs (data flash)
0x407FBxxx   8 refs        0x40040040/60  3 refs      0x40080000   2 refs
```

`0x40047000`/`0x40060000`/`0x40090000` are the candidates for USBFS / USBHS / SDHI; which is
which is unconfirmed.

---

## Open items

1. **Boot contract** — what `0x001022DC` does before `main`: `.data` copy, `.bss` zero, VTOR,
   FPU enable, MPU, caches, C++ static constructors; and whether clocks are configured or
   inherited (candidate CGC accesses at `0x4001E00C`, `0x4001E3FE`, `0x4001E4BB`). Determines
   M1's startup code.
2. **Display path** — GLCDC vs external RA8876 frame RAM; identity of the `0x64000000` window.
   Determines M2's shape.
3. **Input details** — pot mechanism, I²C addresses and part identities, switch/button reading.
   Determines M3's shape.

---

## Quirks found on hardware, not in the image

Three bugs that each presented as "the device is dead", found only by running our own code on
the unit. All three are recorded here because none of them is visible in the stock image —
they are consequences of *our* code meeting this silicon, and each one cost days.

### 1. The stock bootloader leaves a SysTick pending, and the handoff releases it

**Symptom:** the application never enumerated, never blinked, never reached a single line of
its reset handler. Every build failed identically no matter what was changed inside it.

**Cause:** the stock bootloader runs ThreadX, so SysTick is live before any of our code exists,
and it hands off to us with interrupts masked — which is why our own bootloader never sees it.
`app_launch()` set `SCB->VTOR` to the application base and *then* executed `cpsie i`. That was
the first unmask since the stock bootloader, so the latched tick fired immediately, vectored
through the **application's** table into its default handler, and spun there forever.

Clearing `SYST_CSR` stops the counter; it does **not** clear an exception the counter already
latched. That distinction is the whole bug.

**Fix, in two places, because either alone is insufficient:**
- `app_launch()` writes `ICSR_PENDSTCLR | ICSR_PENDSVCLR` and hands off with interrupts still
  masked; `Reset_Handler` unmasks after ICU routing, which is the first point at which taking
  an interrupt means anything.
- `SysTick_Handler` and `PendSV_Handler` are **not** aliased to the spinning default handler.
  A stray tick is absorbed, the timer stopped, and a counter incremented in the boot handshake
  so the console can confirm it happened. This is what lets a new application boot on a
  bootloader flashed *before* the fix — which mattered, because the bootloader can only be
  replaced through the SD card.

Confirmed, not assumed: `id` reports `stray_systicks 0x00000001` on every launch.

### 2. Pin names: the tools print hex nibbles, Renesas counts in decimal

**This is a notation trap, not a hardware bug, and it produced a confident false diagnosis.**

`tools/pinmap/decode-pinmap.js` and `docs/pinmap.txt` name pins with a **hex** nibble for the
pin number, so port 3 pin 13 prints as `P30D`. Renesas names the same pin **`P313`**, counting
in decimal. They are the same pad. The mapping is `Pm<hex>` here ↔ `Pm<decimal>` in the
datasheet — so `P30D` = `P313`, `P30E` = `P314`, `P50B` = `P511`, `P10F` = `P115`.

Reading `docs/pinmap.txt` for "P313", finding nothing, and concluding the display reset line was
undeclared was wrong: `P30D` is right there, already configured `OUT_HIGH | DIR_OUTPUT`. The
hardware agreed — declaring the pin an output changed nothing, and quirk 3 below was the entire
fault. The redundant `bsp_pin_cfg()` in `hw_reset()` is kept only so the driver visibly owns the
line it drives.

When cross-referencing the pin table against these notes or the datasheet, **convert the pin
number**. Several M3 pins are affected: the mux strobe `P314` is `P30E`, and the RIIC2 pair
`P511`/`P512` are `P50B`/`P50C`. Likewise the pot ADC inputs the notes call `P014`/`P015` appear
as `P00E`/`P00F` — the only two `ANALOG` entries in the table, and not a contradiction.

### 3. RSPI discards all received data once OVRF is set

**Symptom:** every RA8876 register and the status byte read back `0xFF`, indistinguishable from
a controller that is not responding or is held in reset.

**Cause:** RSPI is full duplex — every byte clocked out clocks a byte in. The register-write
path (`frame32`) never read `SPDR`, so the second write overran the receive buffer and set
`SPSR.OVRF`. **Once OVRF is set the receiver discards everything until the flag is cleared.**
The controller was answering the whole time; we were throwing the answers away, and one write
was enough to poison every subsequent read.

**Fix:** every transfer drains `SPDR` and clears `SPSR` error flags. The drain must match the
current `SPDCR.SPLW` width or `SPDR` does not return what was received.

Diagnosed by reading `SPSR` over the console with `peek b 40072103` — `0x21` = `SPTEF | OVRF`.
Worth noting that no amount of reasoning about the display got there; a register read did.

### The lesson that generalises

Every one of these was found by making the device able to report on itself, and every hour
spent before that was wasted. In order:

1. USB console first, before anything that can fail.
2. Breadcrumbs into memory that survives a warm reset, for failures earlier than USB. The
   bootloader's `id` prints `app_faults`, so that field doubles as the crumb channel — the
   reporting side was already in flash and unchangeable, so the channel was fitted to it.
3. `peek`/`poke`/`pfs` over the console, so a hardware question can be answered in seconds
   rather than by a 22-second reflash per hypothesis.

Fault handlers must never spin silently. Ours now record the exception number and the startup
stage it fired in; `0x00000FF0` — exception 15, stage 0 — is what finally identified quirk 1.

---

## M2 verified on hardware (2026-08-17)

Our own firmware, launched by our own bootloader through the stock one, driving our own pixels
onto the real panel. Confirmed visually against the test pattern:

- **800 x 480 exact.** The one-pixel white frame is visible hard against all four edges, so the
  active window and the porch arithmetic are right, not merely close.
- **All 480 rows scan out** — the bottom-left blue marker is visible.
- **RGB565 byte order is correct** — the eight bars read white/yellow/cyan/green/magenta/red/
  blue/black in order.
- **The drawn image is NOT rotated.** Red renders top-left and green top-right, exactly as
  drawn.

That last point matters for M3. The stock firmware inverts touch coordinates by 180°
(`X = 800-x`, `Y = 480-y`), which was read as "the panel is mounted upside down". It is not —
at least not the way stock drives it. Either stock also flips the *display* in the controller
(RA8876 can mirror via the memory-access and display-configuration registers) so that its
inversion cancels out, or the touch digitiser is mounted opposite the LCD. **Do not copy
stock's touch inversion blindly**; establish the mapping empirically once the FT5x06 is up, or
the touch axes will end up inverted relative to what we draw.

### Backlight brightness runs backwards

`compare = 0x2000 - brightness`, so **0x0800 is full brightness and 0x2000 is off**. Values
below 0x0800 wrap above the 0x1800 period and also turn the backlight off, which is how stock
blanks the screen (it writes 0xFFFF). This is counter-intuitive enough that it cost a debugging
round on its own: an attempt to raise the brightness to maximum turned the panel off, and the
resulting black screen was indistinguishable from the failure being chased.

---

## M3 findings on hardware (2026-08-17)

### The pot-touch sensor is held mute by P402

**[certain, measured]** The AT42QT2120 does not answer on I2C at all — not even an address ACK —
until **P402 is driven low and released**. Established from cold: a bus scan found nothing, a
pulse was applied, and the same scan then found `0x1C` every time, repeatably across resets.

This is a board fact, not a software one, and it is not visible in the stock image: SSP's I2C
framework hides the reset line in a config struct. `qt2120_hw_reset()` now pulses it (20 ms low,
150 ms settle) before any transfer, as part of application startup.

With that done the part identifies cleanly: **chip ID `0x3E`, firmware `0x20`** — an exact match
for the AT42QT2120, confirming the identification made from the register map alone.

### RIIC receive runs one byte ahead of the programmer

**[certain]** When `ICSR2.RDRF` signals that byte *k* has arrived, the hardware is **already
clocking byte *k+1***. So the NACK that terminates a read has to be armed while byte *N-1* is in
hand, and `ICMR3.WAIT` asserted a byte before that to stop the hardware running past the end.

Written the natural way — "set `ACKBT` when the last byte arrives" — it is one byte too late:
the final byte is ACKed, the slave keeps driving, and the STOP never completes. **The symptom is
a transfer that returns every byte correctly and then times out**, which is a peculiarly
misleading failure because the data looks perfect.

`riic.c` therefore counts bytes *remaining* rather than indexing forwards, so the off-by-one
cannot reappear. A failed transfer also clears `WAIT` on the way out: leaving it set holds SCL
low and poisons every later transfer on that bus.

### Panel layout

**[certain, from the device's owner]** The 8 pots are in **two rows of four**, below and above
the screen. Bottom row is left-to-right first, then the top row — that is the panel ordering the
protocol should use.

### Diagnostics worth keeping

The bit-banged I2C scanner (`bb <scl> <sda>`) earned its place and should not be deleted. It is
the only thing that can distinguish "the peripheral is misconfigured and never drives the pads"
from "no device is answering" — no register on the MCU side can tell those apart, and they need
opposite fixes. It is also what proved the touchscreen bus has no pull-up, by showing SDA never
recovering after being pulled low.

`gpio <pin> <0|1>` plus a scan is the general technique for finding an unknown enable or reset
line, and is how P402 was found. Both are cheap to keep and expensive to re-derive.

### Panel ordering: stock counts the top row first, we count the bottom row first

**[certain, measured]** Touching the eight knobs in panel order (bottom row left-to-right, then
top row) lit the reported bits in the order 4,5,6,7,0,1,2,3 — a **constant rotation of exactly
4**. Pushing them in the same order cleared the sense bits in the same order. Four is half of
eight and the knobs are two rows of four, so the rotation is a **row swap**.

So the recovered key table `{5,7,6,4,3,0,1,2}` was correct; only its origin differed. Rotated to
panel order (pot 0 = bottom-left) it is **`{3,0,1,2,5,7,6,4}`**, which is what `qt2120.c` uses.

**[certain, measured] Pot index ↔ multiplexer channel is `(n + 4) mod 8`**, and 4 is its own
inverse modulo 8 so the same expression converts both ways.

**This settles open question 3 from the plan — and the plan was right.** It asked whether the
pot-switch index was panel-ordered or mux-ordered, noting that the stock rotation path remaps
(`{0,4,5,1,6,3,7,2}`) while the switch path does not. **There really are two mappings:**

| Input | Multiplexer channel -> panel pot |
|---|---|
| Capacitive touch, push switch | `(ch + 4) mod 8` — a plain row swap |
| **Pot rotation** | **`{4,0,1,5,2,7,3,6}`** — scrambled |
| Front-panel buttons | `{2,1,0,3,4,5}` — first three reversed |

The rotation table is exactly the stock firmware's `{0,4,5,1,6,3,7,2}` with the same +4 row swap
applied to convert stock's top-row-first numbering to ours. All three were established by
turning, pushing and pressing each control and reading which cell responded on screen.

> **Correction.** An earlier pass here recorded the opposite — that one mapping served both, and
> that copying stock's remap "would have been wrong". That conclusion came from noticing, in an
> interleaved capture, that a knob's push and its phase movement appeared on the same channel.
> That was a coincidence of timing between two events a second apart, not evidence. Watching
> each knob individually showed the remap is real. The general lesson: correlation inside a
> capture where the operator is doing several things in sequence is worth very little; drive one
> input at a time and observe one output.

### Input verification (2026-08-17)

All three input classes confirmed on the real panel in a single self-paced capture:

| Input | Result |
|---|---|
| Pot capacitive touch | All 8 knobs produce distinct bits, in a consistent rotation |
| Pot rotation | All 8 channels show phase-pair movement; **S12AD1 ch5 = phase A, S12AD0 ch6 = phase B confirmed** |
| Pot push switches | All 8, active low, on sense bits 0–7 |
| Front-panel buttons | All 6, active low, on sense bits 8–13 |

That confirms the ADC channel assignment, which was listed as an open question — an inferred
channel number is exactly the sort of thing that is wrong in a way nothing detects.

**One anomaly worth chasing:** the first knob touched kept its bit asserted for the rest of the
capture rather than releasing. That may simply be a hand resting on it, or it may be drift
compensation on that key. Re-test deliberately: touch one knob, release, and confirm the bit
clears.

---

## M4: measured transport throughput (2026-08-17)

**Bulk, on this device, through our own polled stack: 268.5 KiB/s, zero bytes dropped**
(131,142 bytes in 0.48 s, payload a repeating counter so loss would have been visible as well
as timed).

Context for the number:

| Path | Throughput | Note |
|---|---|---|
| Stock SysEx over USB-MIDI | ~27 KiB/s | 43,535 B script read back in ~1.6 s |
| **Our bulk, measured** | **268.5 KiB/s** | ~10x the SysEx path, unoptimised |
| HID interrupt, theoretical max | 62.5 KiB/s | 64 B per 1 ms frame, hard ceiling |
| Full-speed bulk, theoretical max | ~1.2 MB/s | 19 x 64 B packets per frame |

**The transport decision is settled, and on numbers rather than preference: bulk.** It already
beats HID's *theoretical best* by more than 4x, and HID cannot be improved — an interrupt
endpoint is polled once per `bInterval` regardless of report size, so a bigger report buys
nothing. This was the M4 gate.

We are at roughly 23% of the full-speed bulk ceiling, which is expected and is ours to reclaim
later: the driver is polled, sends one 64-byte packet at a time through a 512-byte buffer, and
runs through CDC-ACM's usbser.sys rather than a vendor interface. Double buffering, a larger
staging buffer, or moving to a vendor-specific interface with WinUSB are all available. None of
that is needed to start — 268 KiB/s is already an order of magnitude more than the protocol was
ever going to need for control-surface traffic.

Note the port is **Full Speed, not High Speed**: the stock descriptor declares `bcdUSB 0x0110`
and 64-byte endpoints. USBHS serves the *host* socket; USBFS is the device port. So 1.2 MB/s is
the hard ceiling for any design here, and no amount of protocol work moves it.

### ADC sample-and-hold carries charge between multiplexer channels

**[certain, measured]** Turning any one knob visibly moved the reading on the **next**
multiplexer channel as well as its own — in all eight cases, without exception, wrapping from
channel 7 to channel 0. A defect that regular is not crosstalk in the wiring; it is the ADC's
sample-and-hold arriving at a newly selected channel still holding charge from the previous one.

The pot wiper and the multiplexer's on-resistance sit in series with the sampling capacitor, so
it charges far more slowly than a default sampling window allows. Two independent remedies, both
applied because either alone may be marginal:

- **Wait longer after switching the multiplexer.** Was 20 us; now runtime-tunable, default 200.
- **Discard conversions before believing one.** Each conversion re-samples, so extra passes give
  the capacitor further sampling windows to reach the new voltage. Default 2 discarded.

Both are tunable from the console with `settle <us> [discards]`, so the minimum can be found by
measurement rather than chosen defensively — the cost is scan rate, paid eight times per sweep.

The proper fix, if these prove expensive, is the ADC's own sampling-time registers (`ADSSTR`),
which extend the sampling window without burning wall-clock in a delay loop.

---

## The multiplexer strobe is an ENABLE, and it must be LOW during a conversion

**[certain, measured] This was the whole crosstalk problem, and it is worth reading before
touching the input code.**

P314 (P30E in the pinmap tool's hex notation) is an **active-low output enable** on the analog
multiplexer, not a latch. The first implementation pulsed it low and left it **high** — disabled
— for the entire conversion. The analog node then floated, and the ADC sampled whatever charge
it still held from the previous channel.

That produces a signature which is easy to misread: contamination that depends on the previous
channel but **does not improve with time**. Settle delays of 0, 5, 20, 200, 500, 1000 and
2000 us all measured the same ~320 counts of spread. It looks like crosstalk and it is not; the
node was simply never being driven.

| Strobe held during conversion | Worst spread (counts) |
|---|---|
| HIGH — mux disabled (original) | 316 |
| **LOW — mux enabled** | **5** |

Note the digital sense lines are unaffected: button and switch readings are identical either
way, measured channel by channel. The strobe gates only the analog path. That is why the
buttons worked perfectly while the pots did not, which made the fault look analog-specific in a
misleading way.

### ADC sampling-state registers

`ADSSTR<n>` is at **`base + 0xE0 + channel`**, 8-bit, reset default `0x0B`. Found by dumping the
peripheral's address space and spotting the run of `0x0B`, then **confirmed by experiment**:
writing 0x20 / 0x80 / 0xFF moved the measured cost from 412 to 572 / 1332 / 2372 cycles per
conversion pair — linear at ~4 core cycles per state, which puts ADCLK at PCLKC = 60 MHz.

Worth knowing, but **it is not the lever here**: with the multiplexer enabled, raising the
sampling window from 11 to 255 states changes the spread by only 64 -> 53 counts. The settling
of the multiplexer's own node dominates, and that is bought with `settle_us`.

### Settling versus rate, measured

With the strobe correct, residual spread falls roughly as 1/t:

| Settle | 10 us | 25 us | 50 us | 75 us | 100 us | 150 us |
|---|---|---|---|---|---|---|
| Worst spread | 64 | 37 | 21 | 15 | 12 | 8 |

The decode deadband is 16 counts, so **150 us puts the residue at half the deadband** and it
cannot register as movement. Reaching the noise floor of 1 would need something like 1.2 ms per
channel — which is almost exactly the stock firmware's 1.5 ms, and explains why its full sweep
takes 12 ms. Stock simply waits it out.

### The strategy: let the touch sensor aim the ADC

We do not have to wait it out, because of two measured facts:

1. Contamination only ever appears on the **first** sample after the multiplexer moves. Hold it
   still and readings sit at the noise floor.
2. **A knob cannot be turned without being touched**, and the AT42QT2120 already reports which
   knob a hand is on.

So `inputs_service()`:

- sweeps the **digital** senses every pass — they need no analog settling, so all 8 channels
  cost about 16 us, and every button stays live at all times;
- spends the analog budget on the **one knob being touched**, parking the multiplexer there and
  sampling it repeatedly with no channel change and therefore no contamination;
- falls back to a slow round-robin when nothing is touched, purely to keep baselines fresh —
  absolute position is meaningless on an endless pot, only change matters, and change only
  happens under a finger.

Cost is about 0.2 ms per pass instead of 12 ms for a fair sweep, and the knob in use is sampled
far more often than round-robin could manage. Full accuracy exactly where it is needed.

---

## Capacitive touch: sticking on, and dropping out

**Both symptoms are present in the stock firmware too**, reported by the device's owner. They
have opposite causes and cannot both be fixed by moving the detection threshold — which is
presumably why stock has never fixed either.

**Sticking on.** The AT42QT2120 suspends drift compensation on a key while that key is in
detect. So a key that crosses its threshold because of an environmental change rather than a
finger can never drift back out: the condition that would correct it is switched off by the
condition it is in. Nothing recovers it until the max-on timer (`TRD`, address 12) fires, and
its reset default is 255 — about **41 seconds**.

**Dropping out during rotation.** A finger rolls across the knob as it turns, the contact area
momentarily shrinks, and the delta falls back under the threshold.

Raising the threshold cures the first and worsens the second; lowering it does the reverse.
So neither is treated with the threshold. Instead, two mechanisms using information the sensor
does not have:

- **Rotation implies touch.** A pot cannot physically turn without a hand on it, so a moving pot
  asserts its own touch bit regardless of the sensor. This fixes dropouts exactly where they
  were reported — during rotation — without desensitising anything.
- **Acquisition is instant, release is debounced** (`QT2120_RELEASE_MS`, 180 ms) and additionally
  requires the pot to be still. A dropout shorter than that never reaches anything upstream.

For sticking, a watchdog recalibrates a key that claims touch while its pot has not moved for
`QT2120_STUCK_MS` **and** whose signal sits only marginally past its threshold. Both conditions
are required: a real finger gives a delta far larger than the threshold, a latched key sits just
past it, and demanding both is what stops the watchdog firing on somebody simply resting a hand
on a knob. `TRD` is left long (10 s) as a backstop rather than made aggressive, because the
chip's own timer cannot tell a stuck key from a held one, and **recalibrating with a finger
present bakes the finger into the reference** — trading one wrong reading for another.

Ordering matters in the main loop: the input scan dwells on the *filtered* touch mask, so a
momentary sensor dropout does not stop us sampling that pot. Otherwise the dropout would hide
the very movement that repairs it.

### Signal registers are little-endian

An earlier note said the key signal and reference blocks (`0x34`, `0x4C`)
are big-endian u16. **They are little-endian.** Read as big-endian every value came back with
its high byte pinned in the 0x02–0x03 range and its low byte varying — 0x3903, 0xCF02, 0xB602 —
the unmistakable signature of a byte swap. Read little-endian the same bytes give 825, 719, 694:
plausible levels, stable, and exactly equal to their references at rest, which is what drift
compensation should produce.

Worth stating plainly: **a swapped u16 does not fail, it lies plausibly.** The `qt` console
command prints signal, reference, delta and the detect bit per key, in panel order, which is
what makes this checkable at all — "sticks on" and "drops out" are reports about the sensor's
*conclusion*, and the delta is its *evidence*.

### Measured: what a touch actually looks like

With peak-hold instrumentation (`qtpeak`), holding and turning two knobs gave:

| Knob | Peak abs delta | Threshold |
|---|---|---|
| B1 (pot 0, key 3) | 166 | 12 |
| B2 (pot 1, key 0) | 266 | 12 |

Two conclusions, both of which overturned a working theory:

**B1 really is a weaker channel** — about 62% of B2, consistent with its resting signal of 845
against 680–740 elsewhere. But **per-key gain is NOT the fix**: at 166 against a threshold of
12 there is a factor of thirteen in hand. A channel with that much margin does not drop out
because it is insensitive.

**A touch drives the signal ABOVE the reference**, so `reference - signal` is *negative* during
a real touch. The opposite convention had been assumed. That mattered: the stuck-key watchdog
confirmed "is this a real finger?" with a signed comparison that could therefore never pass, so
it would have judged every genuine touch marginal and recalibrated it away. Now compared by
magnitude.

### The real cause of dropout: Drift Hold Time

`DHT` (address 13) is how long reference drifting stays suspended after a detect. Its default of
25 is about **4 seconds** — after which the reference resumes drifting *toward* the touched
signal while the finger is still present. The delta shrinks, crosses back under the threshold,
and the touch is dropped mid-gesture.

That is why the dropout was worst during rotation: **rotations are simply the long gestures.**
A quick tap never lasts long enough for the reference to catch up.

Set to maximum (~41 s). This is only safe read together with `TRD` (10 s): a key that latches
spuriously is recalibrated by the max-on timer long before the drift hold would have rescued
it, so lengthening the hold cannot make sticking worse. Changing either without the other
re-breaks one of the two symptoms.

---

## A crashed application now recovers itself

**[certain, rehearsed]** A fault, an NMI storm, or a plain infinite loop all now end the same
way: breadcrumb written, `BOOT_REQ_UPDATE` set, reset into the bootloader — which holds in
flash mode. The device comes back on USB by itself, carrying its diagnosis, with **no physical
intervention**.

Proven deliberately rather than assumed, with a `hang` console command that stops the main loop
on purpose. The device returned in seconds reporting `boot_reason 0x01` (application asked) and
`app_faults 0x0029FFF0` — fault marker `F0`, source `0xFF` (the watchdog), stage `0x29` (main
loop). Same reasoning as rehearsing the SD-card recovery in M0: an untested recovery path is
worth very little.

Three mechanisms, because they catch different things:

| Failure | Caught by |
|---|---|
| Exception (hard fault, bus fault, …) | `Default_Handler` → reboot |
| NMI that re-fires | bounded absorber: 64, then reboot |
| Infinite loop — no exception at all | SysTick liveness watchdog, 2 s |

**The original design spun forever on a fault**, reasoning that a reset loop looks like a boot
failure. That was wrong in a way this session made expensive: a spinning device **disappears
from USB**, so every crash cost a person walking over to unplug the cable. There is no reset
loop to fear, because the bootloader does not relaunch a failing application — it waits.

> **A bug worth remembering.** The application defined its own `NMI_Handler` to record more
> detail, overriding the bounded one in `vectors.c` — and the override recorded, disarmed the
> monitor and *returned*, with no bound. When the NMI turned out to re-fire anyway, the device
> stormed forever and looked exactly like the spin it was meant to replace. **An override that
> silently drops a safety property is worse than no override.** Both handlers now bound.

### Solved: the stack must live LOW in SRAM, and .bss must not push it

**Symptom.** Adding ~12 KB of static data made the application die with **NMI at `STAGE_LOOP`**
— it started, reached its main loop, then took a non-maskable interrupt and vanished from USB.
Remove the data, and the same image ran perfectly.

**What it was not.** Two theories were formed, tested and disproved. Both are recorded because
each looked convincing and re-deriving them would waste the same time again:

1. **Stack misalignment.** Every working build happened to have a 16-byte-aligned stack and the
   failing one did not — a perfect correlation across three data points. Aligning the stack and
   masking the monitor's window changed nothing.
2. **The monitor window excluding the initial MSP.** `MSPMPUEA` was `__stack_top - 1`, so an
   empty stack — MSP exactly at the top — sat outside its own window. That is genuinely wrong
   and is now fixed, but with the failing layout restored the application **failed identically**
   with the fix in place. Not the cause either.

**What it is.** The failure follows the **stack's absolute address**, and nothing else:

| Stack region | Result |
|---|---|
| `0x1FFE0040`–`0x1FFE4040` | works |
| `0x1FFE0540`–`0x1FFE4540` | works |
| `0x1FFE34D0`–`0x1FFE74D0` | **NMI** |
| `0x1FFE64F0`–`0x1FFEA4F0` | **NMI** |

Established with a plain 12 KB `volatile` array as ballast, which reproduces it with no extra
code at all — so the codec's test file was never implicated, only its size.

Two further measurements narrow it. The monitor's registers store every address faithfully
(checked by poking candidate windows and reading them back), and **arming the exact failing
window while MSP sits inside it produces no NMI** — so the window is not intrinsically bad. The
breadcrumb's direction byte says MSP was **above** the region end by more than 32 bytes, i.e.
above the stack top, which is not a value a correctly initialised stack should ever hold.

**The fix: place `.stack` BEFORE `.bss` in the linker script.** The stack then sits directly
above `.data` and stays put no matter how much the image grows. Verified with the full test
suite plus ballast resident: application healthy, `nmi_count 0`, and all twelve codec tests
passing on the device.

This is worth doing on its own merits regardless of the underlying cause. With `.bss` first,
**adding an unrelated static array relocates the stack** — which makes the stack's address an
accident of unrelated code, and is exactly how a latent problem like this stays hidden until it
appears at random.

**Still unexplained:** *why* a stack high in SRAM makes the monitor report an out-of-range MSP.
The likeliest remaining candidate is a boundary between SRAM blocks that the stack-pointer
monitor treats differently, but that is a hypothesis, not a finding. The practical risk is
contained: the stack is pinned low, its address no longer moves, and a fault now recovers
itself. If it ever returns, the crumb's direction byte and `nmi_count` are already in place.

---

## M4: the protocol on the wire (2026-08-17)

**Control channel working end to end**, measured on the device rather than asserted:

| Check | Result |
|---|---|
| Unsolicited READY | 1 Hz until a host answers, then stops |
| HELLO -> READY | answered idempotently |
| PING -> PONG x5 | 5/5, ~30 ms round trip, host timestamp echoed exactly |
| ECHO 16 / 512 / 900 bytes | all byte-identical |
| HEARTBEAT | unsolicited, 996 ms apart, carrying live counters |
| Decode errors, drops | 0, 0 |

Verified three ways, deliberately: the C codec, its twelve tests (passing on **both** host and
device), and `tools/deploy/emp.py` — a host client written from `docs/protocol.md` rather than
from `frame.c`. A wire format that only round-trips through its own encoder proves very little.

### EMP shares the pipe with the text console

Frames are told apart from console text by the first byte: `0xE1` is never the start of a
console line. Keeping the console alive is deliberate — every hardware fault in this project was
diagnosed through it, and the flashing path speaks it too. A binary protocol that displaced its
own debugging tool would be a poor trade.

### Two host-side bugs, both of the exact class this protocol exists to catch

- **`WriteFile` short writes were silently accepted.** A 900-byte ECHO came back as 512. On a
  framed protocol a partial write does not produce an error, it produces a *plausible truncated
  message* — which is precisely the failure that made the old SysEx path untrustworthy, found
  here in our own tooling. `RawCom.write` now loops until everything is gone.
- **`COMMTIMEOUTS` made every latency measurement a lie.** `ReadIntervalTimeout = 0` with a
  400 ms total means `ReadFile` waits the full window whenever fewer bytes arrive than were
  asked for — and callers always ask for a big buffer. Round trips read as a flat ~403 ms. With
  a 10 ms interval timeout the real figure is **~30 ms**. A measurement that never varies is
  measuring the instrument.

### The device's time base is SysTick, NOT the DWT cycle counter

The first implementation derived milliseconds from `DWT->CYCCNT`, and it **froze**: `millis` read
identically three seconds apart. Every timeout in the firmware then silently stopped expiring —
READY and HEARTBEAT went out once at boot and never again. **A dead clock does not announce
itself**; everything downstream simply stops happening, which is horrible to diagnose from the
far end of a wire.

DWT still measures short intervals correctly (`adctime` uses it, and its numbers are linear and
consistent), but it is a debug-domain block and does not keep running unattended. SysTick is
ordinary core hardware, already running for the liveness watchdog, and proven to fire.
**Timekeeping belongs on the clock that is definitely running.**

### Surface channel working (2026-08-17)

Descriptor transaction and input reporting, both verified on the device:

| Check | Result |
|---|---|
| `DESC_BEGIN`/`FIELD` x8/`END` | `DESC_ACK` in 22 ms, 8 fields, CRC over concatenated payloads verified |
| Revision gate | a `VALUES` stamped with an unknown revision is refused with `DESC_REQUEST`, not applied |
| Heartbeat reflects state | `fields=8`, `revision=0x5EED0001` |
| `FOCUS` on touch | reports field id **and lane** |
| `EDIT` on rotate | absolute value, integrated on the device |
| `EDIT_DELTA` | sent only when the field declares nothing to integrate against |
| `EDIT` on push | toggle flips, `cause` distinguishes it from a rotation |
| `BUTTON` | emitted, and produces **no** edit |

The `EDIT` / `EDIT_DELTA` split is the decision table from the spec, working: with min and max
the device integrates locally and sends an absolute value, with only a step or a precision it
integrates at that granularity, and with none of them it sends a delta for the host to
integrate. The device is locally authoritative wherever the descriptor gives it enough to be,
which keeps the USB round trip out of the felt latency of a gesture.

`sim r|p|t|b` on the console injects input without touching the panel. That separation earned
itself immediately: when an end-to-end test came back empty it was possible to prove the
protocol half alone, rather than guessing which half was at fault.

**Known gap:** a Choice field with a `choice_count` but no min/max/step/precision currently
falls to `EDIT_DELTA`, because the decision looks only at the presence mask. It is compliant
with the table as written, but a choice with a known cardinality could perfectly well be
integrated on the device, and should be.

### Two tooling bugs found while doing this

- **The image gained a 4-byte alignment hole** between `.vectors` and `.text`, and the two tools
  disagreed about it: `check-image` warned, the flasher refused outright. The flasher now fills
  inter-section gaps with `0xFF` — exact, since the region is erased first — and the checker
  names each gap with addresses instead of a vague segment count.
- **`erase` could exceed the host's 3-second deadline.** 92 block erases legitimately take
  longer, and the tool reported "erase failed" while the device was succeeding. Now 30 s.

---

## M5 typography: host simulator, screenshots, and an unfinished blit

### The simulator (tools/sim) — build the UI on the workstation

`powershell -File tools\sim\run-sim.ps1 [ui|specimen]` compiles the REAL drawing code — text.c,
font_data.c, ui.c — against a framebuffer instead of the RA8876 and writes a PNG. No hardware,
no flash cycle, about a second per iteration.

It earned itself immediately, twice:

- **Glyphs rendered as slivers.** The font generator measures `by` from the TOP of the line box
  (PIL's default anchor draws from the top), while the renderer added `ascent` to it as if it
  were a baseline-relative bearing. Every glyph was pushed down by a whole ascent so only its
  last row or two landed. Invisible in source, obvious in a picture.
- **Four columns did not fit.** 4x190 + 3x10 + 2x12 = 814 on an 800 px panel, and the empty part
  of a value bar was drawn in a colour bright enough that a nearly-empty bar read as full.

### Screenshots (tools/deploy/screenshot.py) — read the panel back

`shot` on the device streams raw RGB565 out of the controller's memory; the host saves a PNG.
Verified: after `lcd r`, reading back gives 0xF800 — the exact red that was filled.

This is worth more than convenience. A black panel has at least two completely different causes
— nothing was drawn, or something was drawn and is not scanned out — and they are identical
from outside. Reading video memory separates them in one command.

Two things it forced:

- **Unsolicited protocol traffic must be muteable.** A READY frame landing inside a 768 KB raw
  pixel stream lands inside the IMAGE, and raw pixels have no framing to resynchronise on.
  `emp_session_mute()` exists for anything that streams unframed bytes.
- **Long commands must feed the watchdog.** A multi-second screenshot blocks the main loop, and
  the liveness watchdog quite correctly rebooted the device. That is the watchdog working;
  anything legitimately slow says so via `watchdog_kick()` rather than the timeout being
  loosened until it stops catching real hangs.

### SPI: the unit must be re-enabled before every transfer

`ra_write_bulk` clears `SPCR.SPE` at the end of a burst to drop chip select — and nothing ever
set it again. From the first text blit onwards **every SPI transfer was a silent no-op**:
register writes vanished, reads returned 0xFF, and the panel stopped changing. It presented as
"the display died", which is the second time a quiet SPI unit has been misread that way.
`spi_clear_rx()` now asserts SPE and clears stale receive errors before every transfer.

### The stack monitor is disarmed, and why

The Renesas MSP monitor produced **three** false positives, each costing hours, and each fix was
disproved by the next failure:

1. *The window excluded the initial MSP* (`EA` was `top-1`). Genuinely wrong, fixed — failure
   persisted.
2. *The stack must not be 8-byte aligned.* Perfect correlation across three builds; aligning
   changed nothing.
3. *The stack must live low in SRAM.* Held for several builds, then the same NMI appeared with
   the stack at 0x20000020.

Every occurrence reported MSP **above** the window end by more than an exception frame — a value
a correctly initialised stack should never hold — while the window itself was demonstrably good:
arming it with MSP deliberately inside produced no NMI at all. Something about how this monitor
observes MSP is not understood, and an unexplained watchdog that halts a working device is worse
than no watchdog. It is now `if (0) arm_stack_monitor();` with the reasoning kept in place.

### RESOLVED: the text blit did not reach VRAM

Superseded — kept only as a pointer, because this section stated a live fault for long enough
that it misled a later reader who was working from it.

Fills reached video memory and the simulator proved the text renderer correct, but
`ra8876_blit_*` produced nothing. Two independent causes, both found and fixed on 2026-08-17:
a wrong memory PLL that had broken the controller's SDRAM, and `ra_write_bulk()` transferring
nothing at all. Both are written up below under their own headings, with the measurements.

`src/app/ui.c` was excluded from the device build because linking it made the application fail
to start. That was the same fault: every path into `ui.c` ends in `text_draw()` ->
`ra8876_blit_*`, and a failed blit leaves the active window shrunk to a glyph-sized rectangle,
which presents as a dead display. With the pixel path fixed the file links, the application
boots normally, and the UI renders on the panel. The exclusion is lifted.

## The stock bootloader, read out over USB (2026-08-17)

We had never seen the first megabyte of flash. Every question about inherited MCU state — what
the clocks are, what interrupts are live, what the display is left in — had been answered by
inference from the *application* image, and one of those inferences (a stray pending SysTick)
had already cost days.

`dump <addr> <len>` in the application streams any memory region to the host, and
`tools/deploy/dumpmem.py` saves it. Reading flash is ordinary memory access, so it is read-only
and safe; the whole 1 MB came out in 1.1 s at 936 KiB/s, faster than the measured protocol
throughput because there is no per-fragment framing in the way.

**The stock bootloader is not a loader.** It is a complete ThreadX/SSP C++ application. Its
strings include `LCD: initialised`, `Display Initialized`, `FrameBuffer`, `TextBTE`,
`BitmapReader`, `9Graphics`, `potChange: potId=%d, relativeChange=%d`, `USB DISK MODE`,
`UPDATING FIRMWARE`, plus FileX 6.1.12 and USBX. 299 KB of the 1 MB is code.

Three things follow that matter to us:

- **It drives this panel itself**, before our first instruction. That is what made the display
  bug below diagnosable at all.
- **Option bytes** at `0x00000400`: `OFS0 = 0xFFFFFFFF`, so neither the WDT nor the IWDT
  auto-starts at reset. `OFS1 = 0xFFFFFDFF`.
- **Its initial stack pointer is `0x1FFECCE0`** — far above `0x1FFE8000`. The theory that the
  low SRAM window "behaves differently" past that boundary, which was written into `memory.ld`
  as justification, is contradicted by the vendor's own bootloader running its stack there.

## The display bug: a wrong memory PLL, and why it looked like dead hardware (2026-08-17)

Symptom: black screen, backlight apparently off, and every drawing operation silently doing
nothing — while `id` reported `ra8876_init_rc 0`, `ra_status_bits 0`, and every register read
back exactly what we had written. Geometry, colour depth, active window, `DPCR`: all correct.

**Root cause: the PLL values in `pll_program()` were solved, not measured, and they were wrong.**

|            | PPLLC1/2 | MPLLC1/2 | SPLLC1/2 |
|------------|----------|----------|----------|
| we wrote   | 08 / 34  | 02 / 17  | 02 / 17  |
| working    | 14 / 3C  | 14 / 85  | 14 / 64  |

A wrong *memory* PLL breaks the controller's SDRAM while leaving its register interface
completely healthy. So:

- register reads and writes work perfectly, and every readback confirms your configuration
- the 2D core hangs with its busy bit (status bit 3) permanently set
- fills never complete, pixel writes go nowhere, and every VRAM read returns `0xFFFF`

It presents exactly like a controller that has stopped answering, and it caused two separate
misdiagnoses in this project — the same shape of error as the SPI receive overrun before it.

**Why it was intermittent**, which is what made it hard to attribute: the PLL registers only
latch our values when the preceding soft reset actually takes effect. Usually it did not, our
writes were ignored, the chip kept running on the configuration the bootloader left, and
everything worked. That is why M2 passed and why the same binary later drew nothing.

Reading the values back off a *working* chip is what settled it. Do not re-derive these from a
datasheet formula. The rule for this controller is: **measure, then write.**

Two related conclusions, both arrived at by being wrong first:

- **Inheriting the bootloader's display setup does not work.** It was tempting — we already
  inherit `SCB->VTOR` — and it works after a warm reset. Measured from a cold power-on the
  controller reports SDRAM-not-ready, because the bootloader only brings the display up on the
  paths where it intends to draw. We must run the init ourselves, with the correct values.
- **P313 is still unproven as the reset line.** One test appeared to confirm it; that test was
  invalid, its marker write having silently failed. Pulsing P313 was separately observed to
  *set* the core-busy bit rather than clear it. `ra8876_init()` no longer touches it, and
  `hw_reset()` is kept behind `RA8876_USE_P313_RESET` for the record.

## The bulk pixel path wrote nothing at all (2026-08-17)

With the display healthy, text still rendered as pure black while the *same* `text.c` produced
correct output in the host simulator.

`ra_write_bulk()` drove `SPDCR.SPFC` to pack several frames per `SPDR` access and held chip
select across the whole burst. It transferred nothing — not corrupted pixels, nothing — and
reported success.

It was found by pushing an identical solid block through two paths and photographing the
result. `blitfill <x> <y> <w> <h> <rgb565> <mode>` is kept in the firmware for exactly this:

- `mode 1` writes one 16-bit frame per byte, the same framing the screenshot read path uses
- `mode 0` uses the bulk burst

The mode-1 block appeared; the mode-0 block did not. That is a two-minute experiment that
replaced a lot of unverifiable reasoning about RSPI frame semantics.

**The fix** rebuilds the burst out of the single 32-bit frame that every register write already
uses, carrying three payload bytes per frame behind one `0x80` data prefix. The RA8876 keeps
its memory-write pointer advancing across separate SPI transactions, so chip select does not
need to be held — which is what removes the need for the multi-frame mode entirely.

Measured after the fix:

| region | payload | time | rate |
|---|---|---|---|
| 320x100 | 64,000 B | 0.153 s | 409 KiB/s |
| 800x100 | 160,000 B | 0.244 s | 640 KiB/s |

A full-screen repaint is ~1.2 s and a typical 200x30 label ~18 ms. The wire cost is four bytes
per three payload bytes, against two-per-one for the naive byte loop. If this is ever made
faster, verify the replacement with `blitfill` before trusting it.

## Two phantoms worth recording

Both cost real time and both were self-inflicted.

- **"Progressive byte-phase corruption in the blit path."** Blue read back as `0x001F`, then
  `0x1F1F`, then `0xFFFF` across successive text draws, which looked exactly like a stream
  slipping a byte per burst. It was the dead VRAM degrading, not a framing bug. It vanished
  entirely once the PLL was right.
- **"The screenshot tool corrupts a 64x114 rectangle."** A perfect rectangle of stale colour
  survived full-screen fills in every screenshot while `pix` read the fill colour at the same
  coordinates. Neither tool was wrong: `ui_debug_init()` leaves the on-screen input monitor
  repainting from the main loop, so it redraws its cells during the ~2 s readback, while `pix`
  samples immediately after the fill and beats it. `panel off` makes screenshots quiescent.

Both were "the instrument is lying" theories that turned out to be the instrument telling the
truth about something else. Check what else is running before blaming the readback.
