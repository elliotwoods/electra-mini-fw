# Bidirectional activity highlights

Verified on 2026-10-01 with the connected Electra One Mini and the test interface at
`http://127.0.0.1:8766/`. Local activity uses solid cyan; mirrored activity uses dashed cyan.
Local activity wins. Whole-pad/vector hover highlights all mapped components. Hovering a
numeric component and touching a physical vector knob identify only that component. Reset
and Save Default actions do not receive activity markers. Values and physical knob mapping
are unchanged.

`simulator/` contains captures from the real firmware C renderer. `device/` contains physical
Mini framebuffer readbacks. `interface/` contains physical framebuffer readbacks reached
through Channel 5, the host hub, control daemon and USB. Automated touches were injected
with the firmware console; these captures do not prove physical sensor behavior or browser
pointer routing. Browser DOM routing is covered by the focused Vitest tests. A manual GUI
confirmation was requested separately. No latency or ten-minute endurance claim is made here.

## Reproduction and results

From the firmware checkout:

```sh
.venv/bin/python tools/host.py test
.venv/bin/python tools/host.py graphics
PATH=/Users/elliot/.platformio/packages/toolchain-gccarmnoneeabi/bin:$PATH cmake --build build/highlight-production --target app
.venv/bin/python tools/deploy/flash_usb.py build/highlight-production/app.srec
.venv/bin/python tools/host.py sim toggles-group
# Save build/host/sim.png as simulator/toggles-group.png before the device comparison.
.venv/bin/python tools/graphics/highlight-check.py --out docs/graphics-evidence/highlights/device --image build/highlight-production/app.srec
```

Portable tests passed; ASan/UBSan graphics tests passed 12,759 checks. The linked application
used 275,004 bytes in APP; the image gate passed with the existing section-gap warning.
The application-only updater filled eight bytes of gaps with FF, verified CRC32 and launched
the application. Bootloaders were untouched. Source/build fingerprints are in
`build-identifiers.json`; build/test/device logs are alongside this file.

Minor 3 single highlights, minor 4 grouped highlights, local precedence, immediate clear,
expiry and stale revision rejection were checked on the device. All compared captures
matched the simulator with zero different pixels. `device/highlight-check.json` records
100 highlight/clear pairs, 100 matching PONGs and zero new decode errors, sequence gaps,
EMP drops, graphics errors, input-queue drops, USB drops or NMIs. Counters before/after are
retained: pre-existing nonzero counters are not represented as zero lifetime errors. Console
framebuffer capture pauses input servicing, so its service-gap counters are not latency data.

From av-frameworks after the whole workspace build (`VULKAN_SDK` set on this macOS host):

```sh
cargo build
cargo nextest run --workspace --no-fail-fast -E 'package(av-control-surface) | package(av-control-surface-electra-mini-fw) | package(av-control-surface-remote) | package(av-frameworks-daemon) | package(av-operator-app) | package(av-gui-host)'
cd web
./node_modules/.bin/vitest run src/runtime/channel.test.ts src/runtime/controlSurfaceFocus.test.ts src/runtime/controlSurfaceSelection.test.ts
npm run build
cd ..
target/debug/av-frameworks-daemon --headless
target/debug/example-electra-one --headless --port 8766 --settings /tmp/av-highlight-settings.json
node tools/verify-mini-highlights.mjs --out /tmp/mini-interface-highlights-final
```

The focused Rust suite passed 189 tests with two skipped; focused Vitest passed 58 tests.
The live interface check verified scalar hover/leave, both pad outlines, Y-only hover,
Y touch returning lane 1, and release/reconnect clearing activity. Its JSON is under
`interface/`. Firmware malformed group packets are tested atomically, including duplicate,
unoccupied, out-of-range, stale and truncated IDs; renderer tests compare group-to-lane and
clear transitions against full repaint in generic and both operator layouts.

Verification gaps: automated browser capture was unavailable in this environment. The
PowerShell invariant script could not run because Set-Location raises arithmetic overflow
in this environment; it is not claimed passed. Native app windows launched before this
build need reopening to use the updated host and shared UI protocol.
