# Mini graphics refresh

Work is in the standalone `elliotwoods/electra-mini-fw` checkout on `main`. Commit `a065a80` preserves the previous checkout's uncommitted source, tests, documentation and macOS tools; the old checkout remains intact. Build outputs and virtual environments were excluded. Application updates use the existing updater at `0x00120000`; neither bootloader is installed or modified on the device.

## Appearance and reproducibility

The palette comes from av-frameworks revision `bb279c5d689b3be85044b75e7558d7863b6045d3`, with the source CSS retained at `tools/graphics/av-tokens.css`. Run `python3 tools/graphics/make-palette.py` to regenerate RGB565 constants and `contrast.json`. The generator enforces 7:1 primary numeric ink, 4.5:1 informative labels and 3:1 essential indicators on the quantized surfaces. Cyan selection, teal live state, semantic warning colours and a derived dark danger plate accompany distinct selection bars, touch caps, pressed inset borders and explicit READ ONLY labels.

Inter labels and JetBrains Mono values are retained under OFL, with sources and hashes in `tools/font/fonts.lock.json`. Icons retain DejaVu. Run `python3 tools/font/make-font.py` using Pillow 12.3.0 and FreeType 2.14.3; the generator enforces both versions and verifies all three retained font sources. Font sizes are Inter 26/19, Mono 38/26 and focused Mono 112 pixels. Values remain monospaced when reduced to fit. Titles use two lines or preserve their identifying suffix with a visible truncation mark. Bootloader font/text copies retain the baseline implementation.

All application screen families use graphite surfaces, rounded gradient cards, inset meter tracks, gradient fills, selection plates, RGB channel ramps and alpha checkerboards. Physical control mapping, sparse slot IDs, settings/calibration storage and EMP contracts remain under the existing portable interaction/protocol tests.

## Renderer

`src/app/graphics.h` defines the shared clipped RGB565 solid/linear paint, rounded rectangle, border, coverage text and region-copy operations. Text samples the actual card gradient. Cache keys include font, foreground, text and background coordinates. Page changes clear surface metadata; cache resets and tile reuse invalidate matching entries. Rendering stages in a persistent work canvas before publishing coalesced dirty regions in strips. Reset and replacement screens publish together; the reset test confirms that the previous visible frame survives until replacement rendering finishes. The simulator models separate canvas/display addresses, clipping and the complete 16 MiB VRAM range.

RA8876 uses hardware geometry and BTE copies. Page 0 is visible, 1 is the work canvas, 2–9 are gradient tiles and 10–13 are cached text shelves. Total reserved VRAM is 10,752,000 bytes, below 16 MiB. MCU graphics data uses fixed arrays; there is no runtime allocation. Waits are bounded by 20 ms and a poll count, with USB polling and physical input sampling between operations.

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

`docs/graphics-evidence/simulator/` contains 36 captures from the real UI code. Device captures are framebuffer readbacks, explicitly separated from simulator evidence; they are not photographs of panel scanout. RA8876 and corrected D/AVE overview/focused captures matched the simulator pixel for pixel. Simulator settings show fixture uptime/build text; physical settings show device data.

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

All three use GCC 12.3.1, Cortex-M4 hard-float flags, `-Og -g3`, 240 MHz MCU clock, the same board clocks and SPI setup, benchmark workloads and input-service instrumentation. Each workload has three runs: sample 0 is a separate cold-cache update (paging records sample 1, the first actual page transition), samples 1–30 are warmups, and samples 31–330 are 300 measured updates. Workloads cover one value, precision editing, eight updates, RGBA colour, page changes, settings/calibration transitions and brightness transitions. The benchmark verifies DWT clock cycles over 100 ms before recording timings.

The preserved renderer uses the original UI, typography and flat surfaces, with current benchmark/input/timing instrumentation overlaid by `build-baseline.py`. It provides historical interaction context; it cannot have identical visual quality to the refresh. RA8876 and D/AVE share the same refreshed drawing code and verified pixel output, so backend selection compares identical visual quality. No historical timing is presented as an identical-quality GPU comparison.

The timing columns measure CPU composition/submission, CPU time spent waiting for accelerator completion, pixel-stream SPI time and total completion. Submission excludes measured wait/pixel-stream phases but includes register SPI and input sampling. GPU wait is observed busy-wait duration, not an independent GPU profiler. Total is the authoritative interactive completion metric. Input gap is the cumulative maximum within a workload; input drops and accelerator errors are counters. Cold cache results are kept separate. Raw output, parsed samples, image SHA-256, identities and protocol counters accompany each run.

## Qualification status

The complete benchmark and mixed-run results are pending. Selection requires correctness and reliability first, then interactive p95, worst stalls, transitions and memory. Differences below 10% are ties favouring RA8876. The production configuration currently enables RA8876; D/AVE requires explicit `GRAPHICS_BENCHMARK=ON` and is rejected by CMake otherwise. Final selection and installed production image will be recorded after measurements.

Early D/AVE attempts exposed missing render-buffer submission and a cold full-canvas completion timeout. Both were corrected; failed probe output is retained separately and excluded from valid performance results. The corrected bounded candidate passed exact overview/focused framebuffer parity and reported zero graphics errors. Its paced startup also exposed the updater's premature identity check; the check now retries rather than confusing enumeration with responsiveness.

Physical hand testing of rapid turns, presses, touches and paging remains pending user observation. A framebuffer capture proves stored pixels, not panel scanout flicker. Calibration fixtures do not write saved calibration. No performance, input-loss or reset-free endurance claim is made until the matching evidence is recorded.
