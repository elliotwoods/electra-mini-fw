# Mini graphics refresh

Work is in the standalone `elliotwoods/electra-mini-fw` checkout on `main`. Commit `a065a80` preserves the previous checkout's uncommitted source, tests, documentation and macOS tools; the old checkout remains intact. Build outputs and virtual environments were excluded. Application updates use the existing updater at `0x00120000`; neither bootloader is installed or modified on the device.

## Appearance and reproducibility

The palette comes from av-frameworks revision `bb279c5d689b3be85044b75e7558d7863b6045d3`, with the source CSS retained at `tools/graphics/av-tokens.css`. Run `python3 tools/graphics/make-palette.py` to regenerate RGB565 constants and `contrast.json`. The generator enforces 7:1 primary numeric ink, 4.5:1 informative labels and 3:1 essential indicators on the quantized surfaces. Cyan selection, teal live state, semantic warning colours and a derived dark danger plate accompany distinct selection bars, touch caps, pressed inset borders and explicit READ ONLY labels.

Inter labels and JetBrains Mono values are retained under OFL, with sources and hashes in `tools/font/fonts.lock.json`. Icons retain DejaVu. Run `python3 tools/font/make-font.py` using Pillow 12.3.0 and FreeType 2.14.3; the generator enforces both versions and verifies all three retained font sources. Font sizes are Inter 26/19, Mono 38/26 and focused Mono 112 pixels. Values remain monospaced when reduced to fit. Titles use two lines or preserve their identifying suffix with a visible truncation mark. Bootloader font/text copies retain the baseline implementation.

All application screen families use graphite surfaces, rounded gradient cards, inset meter tracks, gradient fills, selection plates, RGB channel ramps and alpha checkerboards. Physical control mapping, sparse slot IDs, settings/calibration storage and EMP contracts remain under the existing portable interaction/protocol tests.

## Renderer

`src/app/graphics.h` defines the shared clipped RGB565 solid/linear paint, rounded rectangle, border, coverage text and region-copy operations. Text samples the actual card gradient. Cache keys include font, foreground, text and background coordinates. Page changes clear surface metadata; cache resets and tile reuse invalidate matching entries. Rendering stages in a persistent work canvas before publishing coalesced dirty regions in strips. Reset and replacement screens publish together; the reset test confirms that the previous visible frame survives until replacement rendering finishes. The simulator models separate canvas/display addresses, clipping and the complete 16 MiB VRAM range.

RA8876 uses hardware geometry and BTE copies. Straight border sides and repeated RGB565 gradient rows are consolidated into rectangles of at most 32 rows; all 36 simulator captures remained pixel-identical after this optimization. Page 0 is visible, 1 is the work canvas, 2–9 are gradient tiles and 10–13 are cached text shelves. Total reserved VRAM is 10,752,000 bytes, below 16 MiB. MCU graphics data uses fixed arrays; there is no runtime allocation. Waits are bounded by 20 ms and a poll count, with USB polling and physical input sampling between operations.

D/AVE uses the pinned Renesas FSP v6.6.0 TES driver and a bare-metal S7G2 D1 port. It draws matching RGB565 pages in MCU SDRAM, then sends completed dirty regions to the RA8876. Its bounded lifetime arena is 128 KiB. DRW hardware revision, timeout and bus-fault details are exposed in `id`; failures use the existing diagnostic channel. See `third_party/renesas-dave2d/README.md` and the retained licence.

Physical input sampling continues at the 20 ms service period during rendering. A fixed 128-sample queue defers applying interactions until drawing completes, avoiding renderer reentry. Queue exhaustion is diagnosed. Benchmark scenes deliberately freeze UI actions while continuing to sample the hardware; their input-service gaps measure sampling, not physical gesture acceptance. The separate live mixed-update runner exercises real EMP messages and permits physical interactions.

## Build and verification

```sh
export PATH=/path/to/toolchain-gccarmnoneeabi/bin:$PATH
python3 tools/host.py test
python3 tools/host.py graphics
python3 tools/host.py sim ui
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build
cmake -B build/ra-bench -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DGRAPHICS_BENCHMARK=ON
cmake --build build/ra-bench
cmake -B build/dave -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DGRAPHICS_BENCHMARK=ON -DGRAPHICS_BACKEND=DAVE2D
cmake --build build/dave
python3 tools/graphics/build-baseline.py /private/tmp/electra-mini-legacy
```

The build gates each application image before installation: base, flash bounds, stack, reset vector and entry record. The linker retains inherited RWX-segment warnings; image validation reports small alignment gaps, which the updater deliberately fills with erased `0xFF` bytes. Installation validates a padded image CRC and actively checks application identity after re-enumeration. Identity checks retry within a bounded 30-second startup window.

The latest ASan/UBSan run passed 5,258 graphics checks. Graphics tests cover clipping, RGB565 gradient endpoints, rounded edges, canvas staging, VRAM copies, text blended against gradients, background restoration, cache invalidation, monospaced widths, sparse pages, lower-row 1–4-control placement, long suffixes, negative/read-only values and incremental touch/focus release versus a clean repaint. ASan and UBSan are enabled for portable tests, graphics tests and simulator captures.

`docs/graphics-evidence/simulator/` contains 36 captures from the real UI code. Device captures are framebuffer readbacks, explicitly separated from simulator evidence; they are not photographs of panel scanout. D/AVE overview/focused captures matched the simulator pixel for pixel. Strict RA comparison found two native diagonal-line tie pixels in the editor; the RA path now draws the shared integer line as horizontal/vertical hardware rectangles. Final captures verify this correction. Final overview, focused editor, RGBA editor and both operator families match exactly across GPUs; see `device/backend-parity.json`. Simulator settings show fixture uptime/build text; physical settings show device data.

## Measurement method

```sh
.venv/bin/python tools/deploy/flash_usb.py build/dave/app.srec
.venv/bin/python tools/graphics/qualify.py --out build/qualification/dave-final --image build/dave/app.srec
.venv/bin/python tools/deploy/flash_usb.py /private/tmp/electra-mini-legacy/build/app.srec
.venv/bin/python tools/graphics/qualify.py --out build/qualification/baseline --image /private/tmp/electra-mini-legacy/build/app.srec
.venv/bin/python tools/deploy/flash_usb.py build/ra-bench/app.srec
.venv/bin/python tools/graphics/qualify.py --out build/qualification/ra --image build/ra-bench/app.srec
.venv/bin/python tools/graphics/mixed.py --out build/qualification/mixed --seconds 600
```

All three use GCC 12.3.1, Cortex-M4 hard-float flags, `-Og -g3`, 240 MHz MCU clock, the same board clocks and SPI setup, benchmark workloads and input-service instrumentation. Each workload has three runs: sample 0 is a separate cold-cache update, samples 1–30 are warmups, and samples 31–330 are 300 measured updates. Workloads cover one value, precision editing, eight updates, RGBA colour, page changes, settings/calibration transitions and brightness transitions. The benchmark checks DWT cycles over a nominal 100 ms interval before recording timings and records the actual elapsed milliseconds. Input servicing can extend this interval; cycles are normalized against that measured span.

The preserved renderer uses the original UI, typography and flat surfaces, with current benchmark/input/timing instrumentation overlaid by `build-baseline.py`. It provides historical interaction context; it cannot have identical visual quality to the refresh. RA8876 and D/AVE share the same refreshed drawing code and verified pixel output, so backend selection compares identical visual quality. No historical timing is presented as an identical-quality GPU comparison.

The timing columns measure CPU composition/submission, CPU time spent waiting for accelerator completion, pixel-stream SPI time and total completion. Submission excludes measured wait/pixel-stream phases but includes register SPI and input sampling. GPU wait is observed busy-wait duration, not an independent GPU profiler. Total is the authoritative interactive completion metric. Input gap is the cumulative maximum within a workload; input drops and accelerator errors are counters. Cold cache results are kept separate. Paging uses three additional one-sample probes with `gbench 4 1 1`: the fixture starts on page 8, clears caches and immediately changes to page 0. The standard paging loop starts on page 0; its sample 1 has already warmed some shared assets and is therefore excluded from the cold comparison. A cold value update includes the first complete widget redraw after fixture setup. Raw output, parsed samples, image SHA-256, identities and protocol counters accompany each run.

## Device results and selection

RA8876 is the production default. Both refreshed backends passed the complete measurement matrices with no new protocol errors, input queue drops, graphics errors or resets. RA8876 has lower p95 completion time in every workload, much shorter worst stalls and page transitions, and avoids D/AVE's additional 10,883,072-byte MCU SDRAM reservation. No workload falls within the 10% tie rule. D/AVE remains available only in explicit benchmark builds; CMake rejects it otherwise.

Each entry below is physical-device p95 milliseconds over 900 measured updates (three runs of 300 after 30 warmups). Full median/p95/max, CPU submission, accelerator wait, pixel SPI, input gaps and separate cold samples are in [comparison.md](graphics-evidence/device/comparison.md) and [comparison.json](graphics-evidence/device/comparison.json).

| Workload | Original renderer | RA8876 refresh | D/AVE refresh |
|---|---:|---:|---:|
| One value | 8.03 | 11.95 | 50.92 |
| Precision editor | 25.62 | 30.30 | 91.15 |
| Eight updates | 38.26 | 47.17 | 367.96 |
| RGBA colour | 0.75 | 10.96 | 54.75 |
| Page switch | 80.42 | 89.02 | 793.00 |
| Settings/calibration | 98.76 | 53.94 | 796.20 |
| Brightness | 72.33 | 41.59 | 772.13 |

The richer visuals cost up to 10.21 ms additional warm p95 versus the original flat renderer; settings and brightness transitions improve. Warm RA page completion peaks at 90.32 ms. Truly cold RA page changes take 177.40–182.66 ms versus 67.91–80.15 ms original and 725.73–842.31 ms D/AVE. This cold-cache cost remains a limitation; the refresh is not uniformly faster than the original. RA's measured warm input-sampling gaps are 21–27 ms, with zero queue drops; the original's maximum reaches 57 ms. The user reported “No issues noticed” when asked to check rapid turns, presses, touch feedback, paging, flicker and corruption. That is observational acceptance, not a timestamped physical-event latency measurement.

Warm benchmark internal RAM totals (data, ramfunc, bss and stack) are 106,640 bytes original, 121,296 RA and 121,356 D/AVE. All retain the existing 16,777,216-byte heap reservation. D/AVE additionally reserves 10,883,072 bytes in `.graphics_sdram`, for 27,660,288 total SDRAM bytes; RA uses 10,752,000 bytes of controller VRAM for its visible/work/cache pages. Exact ELF sections and hashes are in [memory.json](graphics-evidence/device/memory.json).

The live EMP endurance test ran 600.02 seconds: 8,362 one/eight-value batches, page changes every 12 seconds, 579 ping requests and 579 exact echo replies, and four transport reconnects. Every reply arrived and uptime/session continuity held. No new protocol errors, console USB drops, graphics errors, input queue drops or NMI were recorded. Start/end framebuffer captures confirm the control surface was active. No physical EMP input messages were captured by the harness; the separate user's observation above supplies the hand-test result.

Some faster-starting images have a pre-existing console USB drop count of 64 before host ownership; before and after counters are retained and unchanged in every accepted run. EMP transmit-drop counters remain zero. The first endurance gate incorrectly required the absolute console count to be zero; its original record is retained under `device/probes/mixed-original-gate/`, and the same complete run was revalidated against counter deltas. Seven rejection/acceptance cases verify that the corrected gate still rejects new drops, missing replies, session/uptime resets, diagnostics and input loss. The inherited `shot` console command blocks physical sampling while it reads the framebuffer; both captures are outside the timed endurance window. Consequently its cumulative `id` input-gap maximum is not presented as a render latency or endurance service-gap measurement.

The initial RA implementation had costly row-by-row borders; geometry consolidation preserved all 36 simulator captures and the final matrix above measures that fix. A superseded clock check assumed exactly 100 ms despite input callbacks; the final check normalizes actual elapsed time. Early D/AVE probes exposed missing render-buffer submission and a cold full-canvas timeout, both corrected. Failed probes and an interrupted obsolete matrix are retained separately and excluded from selection. An initial mixed attempt inherited the brightness fixture; it was discarded and fixture setup now cancels settings before starting real updates.

The installed production application is built from `e6c0c97` with `GRAPHICS_BENCHMARK=OFF`, RA8876 enabled, and image SHA-256 `e738fb9e2f5a1ac280d2e6fce466fc2777b498cd1f58a4274cb80591da8eb132`. Warm RA uses `87f5ee3`, warm D/AVE and the original-renderer instrumentation use `454ffd8`; all cold probes use `e6c0c97` (the original UI remains `a065a80`). Shared refreshed UI/fonts/compositor are identical; RA additionally fixes native diagonal-line tie rasterization. Source-file hashes, compiler settings, ELF/SREC identifiers and source revisions are in [source-manifest.json](graphics-evidence/device/source-manifest.json).

The application-only updater validated the padded image CRC, launched it and verified identity. A subsequent read-only check confirmed the persistent boot record matches this image and is proven, five exact PONG responses, three exact ECHO payloads (16/512/900 bytes), reconnect with unchanged session, and no new diagnostic counters. [Production evidence](graphics-evidence/device/production/production-check.json) includes its framebuffer. Saved preference bytes match before benchmarks, after endurance and after production installation. This Mini has no custom saved calibration record; all three snapshots confirm that absence. The updater writes normal image-trust entries, but neither bootloader code is flashed. The router is released to resume its normal connection.

To reproduce cold probes and publish the completed evidence after normal warm/endurance and production checks:

```sh
# Build the same source revision with explicit benchmark flags; use separate cold build directories.
.venv/bin/python tools/graphics/qualify.py --out build/qualification/cold/ra --image build/ra-cold/app.srec --count 1 --workloads 4 --cold-paging
.venv/bin/python tools/deploy/flash_usb.py build/app.srec
.venv/bin/python tools/graphics/storage.py --out build/qualification/storage-after-production
.venv/bin/python tools/graphics/production-check.py --out build/qualification/production
.venv/bin/python tools/graphics/provenance.py
.venv/bin/python tools/graphics/report.py
```

The report refuses incomplete matrices, missing replies, changed image hashes, new error counters and altered saved records. Raw results, build/install logs, captures and a SHA-256 artifact manifest live under `docs/graphics-evidence/device/`. Simulator and physical framebuffer evidence remain explicitly distinguished. Panel scanout has only the user's observational check; framebuffer readbacks cannot prove absence of scanout tearing.
