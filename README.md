# electra-mini-fw

Custom application firmware for the **Electra One Mini** (Renesas Synergy S7G2, Cortex-M4F).

Replaces the stock application. Does **not** replace the bootloader — see [RECOVERY.md](RECOVERY.md),
which you should read before flashing anything.

## Why

The existing `av-frameworks` integration drives this device through a Lua bundle uploaded over
USB-MIDI SysEx, and has hit ceilings no better bundle can move: a 64 KiB WinMM SysEx frame
limit, a 43,535-byte script that takes 1.61 s to read back, and a shared outbound path where a
1 Hz heartbeat truncates a 43 KB transfer. Owning the firmware removes the transport ceiling,
the GUI and menu model, the ASCII-only string truncation, and Lua.

## Layout

| Path | Contents |
|---|---|
| `RECOVERY.md` | How to un-brick the device. Read first. |
| `src/startup/` | Linker script (`.text` at `0x00100000`), vector table, reset path |
| `src/bsp/` | SSP BSP **with pin init removed** — our one deliberate divergence |
| `src/hal/` | Our drivers: display, I²C, inputs, USB |
| `tools/mkimage/` | Build the S-record, check its load address, stage it to `/boot` |
| `docs/hardware-notes.md` | Hardware findings, each cited to an address in the stock image |

## Key constraint: we bring up the board ourselves

The application does **not** inherit a configured board from the bootloader. It performs its
own full bring-up, and so must ours:

- **100 pins** are muxed from a table at `0x0031E0AC` (`ioport_cfg_t` at `0x0031E3CC`).
  Decoded in [`docs/pinmap.txt`](docs/pinmap.txt) — regenerate with
  `node tools/pinmap/decode-pinmap.js <fw.bin>`.
- **Clocks** are configured from scratch: main osc → PLL ×20 → ICLK 240 MHz, with flash and
  SRAM wait states raised *before* the clock goes up.
- **SDRAM** at `0x90000000` is initialised by the app (controller at `0x40003C00`).
- **QSPI** serial flash is initialised by the app; registers at `0x64000000`, XIP window at
  `0x60000000`. The live code path expects a Micron MT25QL256 (32 MB).
- **ICU event-link** entries must be populated per interrupt — on this part the NVIC alone
  does nothing.

The one thing genuinely inherited is **`SCB->VTOR`**: the application never writes it, so the
bootloader must already point it at `0x00100000`. Keep our vector table there, or set VTOR
ourselves rather than relying on it.

> An earlier reading of this image concluded the opposite — that pin muxing was inherited. That
> was wrong: the scan that produced it used too tight a bound on the `pin_cfg` field and missed
> the table. Corrected here; see `docs/hardware-notes.md` for the evidence.

## Toolchain

Renesas e² studio + **SSP 2.7.0** (2025-04-08). Supports S7G2 and no longer requires licence
keys. Bring-up is in C; Rust is deferred until the hardware is proven, because there is no
Renesas Synergy SVD and therefore no PAC to generate.

## Approach

The stock firmware is used only as a reference for **how to reach the hardware** and **what
quirks exist** -- which registers a peripheral needs, in what order, and where the datasheet is
silent or wrong. None of its application code is reproduced here, and none of the material used
to establish those facts is part of this repository.

Everything in `docs/hardware-notes.md` that matters has since been confirmed on the device, and
several entries record where the original reading turned out to be wrong.

## Third-party material

`tools/font/vendor/` holds DejaVu Sans and DejaVu Sans Mono, used to generate the glyph atlas
in `src/app/font_data.c`. They are vendored rather than read from the system so the build is
reproducible on any machine, and because that atlas is a derivative of the font: shipping it
requires a licence that permits derived works. DejaVu is under the Bitstream Vera licence,
reproduced in `tools/font/vendor/LICENSE_DEJAVU`.

Nothing else in this repository is derived from third-party material.

## Status

Pre-M0. Nothing has been flashed. See the project plan for milestones and gates.
