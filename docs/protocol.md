# EMP/1 — Electra Mini Protocol v1.0

Normative wire spec for `electra-mini-fw` ↔ `av-frameworks`. MUST / MUST NOT / SHOULD / MAY
per RFC 2119. Both codecs (C in `src/proto/`, Rust in the host crate) are written against
this document; where they disagree, this document is right and both are wrong until fixed.

## 0. Two premises, corrected

**The device port is full speed.** `bcdUSB = 0x0110`, 64-byte endpoints — see
`hardware-notes.md`. The MTU is therefore **negotiated, never assumed**:

| Transport | MTU | Practical ceiling | 8 KB descriptor |
|---|---|---|---|
| HID interrupt, `bInterval=1` | 56 | **64 KB/s** (64 B per 1 ms frame, per direction) | ~140 ms |
| Vendor bulk | 1016 | ~0.7–1.0 MB/s *(must be measured)* | ~10–15 ms |

A larger HID report buys nothing: an interrupt endpoint is polled once per interval whatever
its size. A bulk *transfer* may span many 64-byte packets, and that is where its win comes
from even at full speed.

**`path` and `lane` are not sent to the device.** `SurfaceField.path` and `.lane` exist so the
*host* can build `SurfaceUiState`; the device only ever echoes `id`. Paths are the longest
strings in a descriptor, so omitting them roughly halves it. `SurfaceChoice.value` is likewise
host-side, per its own doc comment.

## 1. Layering

```
application   SurfaceUpdate / SurfaceInput      (av-control-surface contract types)
message       opcode + payload                  (§3)
fragment      8-byte header + <= mtu payload    (§2)
transport     HID report | vendor bulk          — the only layer that knows which
```

All integers and floats are **little-endian**; both ends are LE, so nothing swaps.
**No field is aligned and no implementation may cast a buffer pointer to a struct.** All
access goes through `memcpy` / `from_le_bytes`. Padding to keep `f64` 8-aligned would cost
wire bytes on the transport where bytes are scarcest.

## 2. Fragment framing

### 2.1 Header — 8 bytes on every fragment

| Off | Size | Field | Contents |
|---|---|---|---|
| 0 | 1 | `magic` | `0xE1` |
| 1 | 1 | `ver_ch` | high nibble = major version (1); low nibble = channel |
| 2 | 1 | `opcode` | repeated identically on every fragment of a message |
| 3 | 1 | `flags` | b0 `FIRST`, b1 `LAST`, rest reserved (send 0, ignore on receive) |
| 4 | 2 | `seq` | u16, per-direction fragment counter, wraps |
| 6 | 2 | `payload_len` | u16, payload bytes in *this* fragment, `<= mtu` |

Channels: 0 `CONTROL`, 1 `SURFACE`, 2 `INPUT`.

Why each field earns its byte:

- **`magic`** doubles as the padding sentinel. A receiver parses fragments sequentially from
  offset 0 and stops when fewer than 8 bytes remain or the next byte is not `0xE1`; if the
  remainder is all zero that is padding, otherwise `E_BAD_MAGIC`. This single rule lets HID
  (padded, one fragment per report) and bulk (packed, several per transfer) share one parser.
- **version nibble** makes a mismatch detectable on the *first* fragment, before any payload
  is trusted — earlier and cheaper than discovering it in `HELLO`.
- **`opcode` repeated per fragment** lets the reassembler reject a continuation whose opcode
  disagrees with the message in progress (`E_FRAGMENT_UNEXPECTED`). That check is precisely
  the detector for the bug class that truncated the old 43 KB SysEx transfer.
- **`seq` is u16, not u8**: a 20 KB descriptor at `mtu=56` is ~370 fragments. USB is lossless
  while the pipe is up, so `seq` is not for wire errors — it detects **ring overrun and pipe
  reset**, which is where data actually disappears on Windows.
- **`payload_len` is authoritative.** Short packets and ZLPs are never delimiters, which
  removes a classic silent-hang bug.
- **No per-fragment CRC.** USB already CRC-16s every packet in hardware. What USB cannot catch
  is our own software corrupting a reassembly, so integrity is spent there: one CRC-32C over
  the whole message. The header is validated structurally instead.

### 2.2 Multi-fragment messages

| flags | meaning |
|---|---|
| `FIRST\|LAST` | whole message in one fragment; no prefix, no CRC |
| `FIRST` | payload begins with the 8-byte message prefix below |
| neither | middle fragment, raw bytes |
| `LAST` | final fragment, raw bytes |

Message prefix (only when `FIRST && !LAST`): `total_len` u32, `crc32c` u32.

The receiver learns the total size before copying a byte, so it bounds the copy against a
static buffer with no allocation and rejects oversize immediately. Single-fragment messages
pay nothing for this — and on this device most messages are single-fragment.

CRC-32C (Castagnoli), not zlib CRC-32: the host gets it from SSE 4.2, and Cortex-M4 with a
1 KB table runs ~8 cycles/byte. **Pinned vector: `crc32c("123456789") == 0xE3069283`.**

### 2.3 Interleaving — the three rules that fix the old failure

- **F1.** A channel-0 message MUST fit in one fragment.
- **F2.** At most one multi-fragment message in flight per direction.
- **F3.** A single-fragment message MAY be sent at any time, *including between fragments of
  a multi-fragment message*.
- **F4.** The TX path MUST serialize at fragment granularity from two queues (priority =
  channel 0, and bulk) and MUST NOT compose messages into a shared partially-filled output
  buffer. The old device-side bug was exactly a shared outbound buffer; F4 forbids the shape,
  not just the symptom.

Consequences: the heartbeat is channel 0, so by F1 it can never be split and by F3 it can be
emitted mid-transfer. By F2 the receiver needs exactly one reassembly buffer per direction and
no message-id demultiplexing — which is what makes a no-allocation device implementation
straightforward. And truncation of the old kind becomes structurally impossible; if an
implementation manages it anyway, the CRC fails and the seq gap is reported, so the failure is
*visible* instead of arriving as a plausible short payload.

### 2.4 Constants

| Name | Value |
|---|---|
| `EMP_MAGIC` | `0xE1` |
| `EMP_VERSION` | major 1, minor 0 |
| `EMP_HEADER_BYTES` | 8 |
| `EMP_PREFIX_BYTES` | 8 |
| `EMP_MTU_HID` / `EMP_MTU_BULK` | 56 / 1016 |
| `EMP_MAX_MESSAGE` | 65536 |
| device `max_message_rx` | 8192 (advertised) |
| device `max_string_bytes` | 512 (advertised) |

## 3. Message set

`0x00–0x1F` control · `0x20–0x4F` host→device · `0x50–0x7F` device→host. Each opcode is
defined for exactly one direction, so no message is ambiguous in a capture.

### 3.1 Channel 0 — CONTROL (single-fragment only)

| Op | Dir | Name |
|---|---|---|
| `0x01` | H→D | `HELLO` — version, host mtu, host_epoch_ms, rx_window, host_id |
| `0x02` | D→H | `READY` — see below |
| `0x03` | H→D | `PING` — `ping_id` u32, `host_time_us` u64 |
| `0x04` | D→H | `PONG` — echoes both, plus `device_time_us`, `credits_total` |
| `0x05` | D→H | `HEARTBEAT` — 1 Hz, unsolicited |
| `0x06` | both | `DIAG` — severity, code, context, detail |
| `0x07` | D→H | `FLOW` — `credits_total`, `next_expected_seq`, `free_fragments` |
| `0x08` | H→D | `BYE` |
| `0x0E`/`0x0F` | H→D / D→H | `ECHO` / `ECHO_REPLY` |

**`READY`** is how the firmware identifies itself rather than the host inferring it: protocol
version, mtu, `session_id` (**changes on every device boot**), capability flags,
`max_message_rx`, `max_descriptor_bytes`, `max_fields`, `fields_per_page`, `applied_revision`,
firmware version and build, then `model` / `serial` / `build_id` strings — which map directly
onto `SurfacePluginSnapshot::{model, serial, firmware}`. `max_fields` and `fields_per_page`
feed `SurfaceCapabilities`, which the current provider hardcodes to 64/4.

The device sends `READY` unsolicited on boot and once per second until it has seen a `HELLO`,
and answers every `HELLO` idempotently. So a host attaching to a running device learns
identity without asking, and a host that starts first is not left waiting.

**`PONG` echoes the host's own timestamp**, so round-trip needs no clock agreement.

**`HEARTBEAT`** carries `session_id`, `applied_revision`, uptime, page, field count, state,
and counters: `rx_fragments`, `tx_fragments`, `rx_decode_errors`, `rx_seq_gaps`, `tx_dropped`,
`render_us_max`. `session_id` on every heartbeat is the cheap resync detector — a device that
reset mid-descriptor is identified within a second with no probe. `render_us_max` exists so
the 0.8 s full-repaint figure stays continuously checkable from the host rather than quoted
from a document.

**No `DIAG` of any severity may close the pipe or return an error to a caller.** The provider
turns it into `snapshot.log`, `error_count` and `SurfaceLink::detail`. Only a transport
failure changes phase.

**`DIAG` payload**: `severity` u8 (`0` info, `1` warn, `2` error), `code` u16, `context` u32,
`count` u32, `detail` String.

`count` is what makes the obligations elsewhere in this document affordable. Several of them —
an uncovered codepoint, a dropped label — are raised per glyph per repaint, and one message per
occurrence would turn a diagnostic into a denial of service against the link it is reporting on.
A sender therefore holds one slot per distinct `code` and emits the accumulated `count` when it
flushes; `context` and `detail` come from the first occurrence in that batch. A receiver must
treat `count` as "this happened *n* times", not as a repeat of one event.

`context` is code-specific and always a number, never text, so the device never formats a
string: the field index for a truncated label, the tag for an unknown value tag,
`(channel << 8) | opcode` for an unknown opcode, the codepoint for a missing glyph.

| code | meaning |
|---|---|
| `1` | `E_STRING_TRUNCATED` — string pool exhausted; the field keeps its knob and loses its name |
| `2` | `E_NON_FINITE` — NaN or infinity refused, on decode or encode |
| `3` | `E_UNKNOWN_VALUE_TAG` — stepped over by its declared length |
| `4` | `E_VALUE_UNDECODABLE` — reserved tag, no length; the record was abandoned |
| `5` | `E_CHOICES_EXHAUSTED` — option labels dropped; the knob shows indices |
| `6` | `E_DESC_TOO_MANY_FIELDS` |
| `7` | `E_DESC_SEQUENCE` |
| `8` | `E_DESC_CRC` |
| `9` | `E_REVISION_UNKNOWN` |
| `10` | `E_FRAGMENT_UNEXPECTED` |
| `11` | `E_REASSEMBLY_TIMEOUT` |
| `12` | `E_UNKNOWN_OPCODE` |
| `13` | `E_TX_DROPPED` |
| `14` | `E_GLYPH_MISSING` |
| `15` | `E_DIAG_OVERFLOW` — diagnostics lost because every slot was in use. Reported rather than dropped silently, since dropping silently is the failure this whole message exists to end. |

A device MUST NOT send `DIAG` from inside a decoder. Framing an outbound message part-way
through an inbound one means building it from state the inbound parse is still walking, and on
a device that streams unframed data (see `TRANSPORT`) it can also land diagnostics in the middle
of an image. Raise, then flush from the service loop.

### 3.2 Channel 1 — SURFACE (host → device)

`0x20 DESC_BEGIN` · `0x21 DESC_FIELD` · `0x22 DESC_END` · `0x23 DESC_ABORT` ·
`0x24 VALUES` · `0x25 REVEAL` · `0x26 TRANSPORT` · `0x27 CLEAR`

**The descriptor is a transaction of per-field messages, not one giant message.** A 64-field
descriptor becomes ~64 messages of 60–250 bytes, most single-fragment even at `mtu=56`. The
device parses each directly into a staging entry — no multi-kilobyte reassembly buffer, no
allocation, interleaving points at message boundaries, and a failure names a field index
rather than "the transfer broke".

- `DESC_BEGIN`: `revision` u64, `field_count` u16, `flags` u16, `total_bytes` u32, `scope`.
- `DESC_FIELD`: `index` u16 (MUST ascend by exactly 1) + field record (§3.4).
- `DESC_END`: `revision`, `field_count`, `crc32c` over the concatenated field payloads.
- `VALUES`: `revision` u64, `count` u16, then `count` × { `id` u16, `ack_edit` u32, Value }.

**Revision gate.** The device MUST drop `VALUES` / `REVEAL` / `TRANSPORT` whose `revision` is
neither 0 nor its own `applied_revision`, and answer `DESC_REQUEST{REVISION_UNKNOWN}`. This is
the mirror of the hub's own rule and closes the same hole in the other direction.

### 3.3 Channel 2 — INPUT (device → host)

`0x50 EDIT` · `0x51 RESET` · `0x52 PLAY_PAUSE` · `0x53 SCREEN` · `0x54 FOCUS` ·
`0x55 DESC_REQUEST` · `0x56 EDIT_DELTA` · `0x57 DESC_ACK` · `0x58 BUTTON`

`EDIT` carries `revision` u64, `edit_seq` u32, `id` u16, `cause` u8, Value.

**`EDIT_DELTA` exists because the pots are endless** two-track potentiometers producing signed
deltas, while `SurfaceInput::Edit` is absolute. Who converts:

| Field declares | Device |
|---|---|
| `min` **and** `max` | integrate locally, send absolute `EDIT` |
| `step` only | integrate at `step`/tick, send absolute |
| `precision` only | integrate at `10^-precision`/tick, send absolute |
| none | send `EDIT_DELTA`; host integrates and answers with `VALUES` |

The device is locally authoritative during a gesture wherever it can be, which keeps the USB
round-trip out of felt latency. The last row is the acknowledged compromise.

`BUTTON` is **diagnostics only** and produces no `SurfaceInput`, so device-local navigation
never looks like a user edit.

### 3.4 Encoding

**String** = `len` u16 + `len` bytes **UTF-8**, no terminator. The old protocol's 22-char
labels and ASCII-only sanitiser are gone. A device that must truncate MUST cut on a codepoint
boundary, set the field's `TRUNCATED` flag, and emit `DIAG{E_STRING_TRUNCATED}` — silent
truncation is forbidden, which was the actual sin, not the limit itself.

Carrying UTF-8 is not the same as rendering it: glyphs come from an atlas in RA8876 VRAM, so
coverage is an asset decision. Uncovered codepoints get a fallback glyph and a `DIAG` count.

**Value** = tag + body: `0x00` Bool u8 · `0x01` Number f64 · `0x02` Choice u32 (the contiguous
device-facing index) · `0x03` Text String · `0x04` Color (`count` u8 + `count` × f32) ·
`0x80–0xFF` extension (`len` u16 + bytes, skippable by a v1 decoder).

`f64` is sent raw, not pre-formatted as the old protocol did: formatting host-side would be
lossy for a value the device echoes back, and puts display policy on the wrong side. The
device formats using `precision`, which needs only fixed-point integer formatting.

**Non-finite `f64` MUST NOT be encoded** — substitute 0.0, set `TRUNCATED`, emit
`DIAG{E_NON_FINITE}`. This keeps round-trip equality total, which is what makes property
testing possible.

**Field record**: `field_len` u16, `id` u16, `kind` u8, `flags` u8, `present` u16, `min` f64,
`max` f64, `step` f64, `precision` u8, `lane` u8, `choice_count` u16, then `value`, `label`,
`unit`, choices, optional `default`, optional `path`.

`min`/`max`/`step` are always on the wire with a presence bitmask, spending 24 bytes
unconditionally to buy a fixed 36-byte prefix: one bounds check, one copy, no branchy offset
arithmetic in a bare-metal parser. `field_len` is the forward-compatibility mechanism — a v1.0
decoder reads what it knows and skips to it.

### 3.5 Echo suppression, made causal

Every `EDIT` carries a monotonic `edit_seq`; the provider stamps each `VALUES` entry with the
highest `edit_seq` it has accounted for on that id. The device applies an inbound value iff
the field is not under active touch, or `ack_edit >= last_edit_seq_sent[id]`.

This deletes the old 350 ms `ECHO_SUPPRESSION` timer and its dependence on poll cadence, and
replaces a timing heuristic with a causal rule a unit test can pin. A value arriving with
`ack_edit` equal to the last sent edit but a *different* value is a correction (a clamp, a
quantization) and is applied; anything older is a stale echo and is ignored.

## 4. Flow control

Bulk and interrupt OUT endpoints NAK when the device buffer is full and the host controller
retries in hardware, so **data is never lost to a slow device at the USB layer**. Loss happens
in software rings — the device's RX ring and the Windows HID input ring. Hence:
`HidD_SetNumInputBuffers(h, 512)` is **not optional** (the default 32 reports ≈ 32 ms of slack
at 64 KB/s). This is the direct successor to "only 4 WinMM input buffers".

**Credits exist to bound the host's own OS-queued depth**, not to prevent loss. If the host
dumps 370 fragments into the write queue, a `PING` behind them waits for the whole transfer —
~370 ms of priority inversion on HID, exactly when liveness measurement should be sharpest.
With `rx_window = 16` the worst case is 16 ms, and that is a test assertion.

Credits are **absolute, not incremental**: `credits_total` is cumulative since session start,
so a lost or reordered `FLOW` self-repairs on the next one. It is piggybacked on `PONG` and
`HEARTBEAT` too, so no `FLOW` is load-bearing alone. Channels 0 and 2 are exempt — the device
reserves ring slots so control can never be starved by a stalled bulk transfer.

**Host flood control:** coalesce pending `SurfaceValueChange`s per id to the latest, emit at
most one `VALUES` per 20 ms, and send only what the device reported visible via `SCREEN`. At
`fields_per_page = 4` that is ~5 KB/s — inside even the HID ceiling.

**Device backpressure:** a coalescing queue (`EDIT`, `EDIT_DELTA`, `FOCUS`, `SCREEN` — a new
entry for a queued id replaces it, so it cannot overflow; resolution degrades, events do not
vanish) and a discrete FIFO (`RESET`, `PLAY_PAUSE`, `BUTTON`, depth 32, never coalesced).
**Every drop is reported** via `tx_dropped` and `rx_seq_gaps` in the heartbeat. A silently
degraded link is the failure this rewrite exists to eliminate.

**No retransmission.** A `seq` gap means something structural — ring overrun, pipe reset,
device fault — not a wire error, so recovery is: abort, `DIAG`, `DESC_REQUEST`, resync. At
10–140 ms per descriptor, redoing it is cheaper than the machinery to avoid redoing it.

## 5. State machines

**Host:** `Down` → `Opening` → `Handshaking` (`HELLO`, ≤3 at 500 ms) → `Syncing` (descriptor
transaction) → `Steady`. Mapping to `SurfacePhase`: `Unavailable`/`Detecting`,
`CheckingFirmware`, `Provisioning`, `Ready`.

`SurfaceLinkState` is computed **independently of phase**: `Live` if anything decodable
arrived within 2500 ms; `Silent` if the pipe is open and nothing has for longer; `Untested`
before the first `READY`; `Down` with no pipe. 2500 ms is derived — two missed 1 Hz heartbeats
plus slack. Three unanswered `PING`s force `Silent` regardless.

**Device:** `Boot` → `Idle` → `Staging` (parse into the staging arena, display unchanged) →
`Live` (arenas swapped at `DESC_END`). Two fixed arenas ping-ponged, in SDRAM. **No allocation
on any path**, and a failed transaction leaves the displayed descriptor untouched — a
half-arrived descriptor is never visible.

**Resync:** a changed `session_id` in any `READY`/`HEARTBEAT` means the device reset; the host
abandons the transaction **unconditionally** and restarts. A transaction MUST NOT continue
across a session change. On a transport drop the device **keeps displaying** its descriptor
and marks the link stale on screen rather than blanking — a cable hiccup should not cost the
operator their page.

## 6. Versioning

Major version is in every fragment; mismatch is terminal and reported as
`SurfacePhase::Incompatible` with the device's `build_id` (`READY`'s first 44 bytes are
promised stable across major versions, which is the one back-compat guarantee here). Minor is
negotiated as `min(host, device)` and may only add opcodes, flags, trailing members inside
`field_len`, or `0x80+` value tags.

**No disagreement is fatal to the link.** Unknown opcode, channel, flag bit, or extension
value tag → ignore and `DIAG`. The worst outcome is a degraded surface plus a diagnostic
naming exactly which construct was refused.

## 7. Testing

Five families, all runnable without hardware:

1. **Pinned wire vectors** — `vectors/emp1.txt`, one record per line: name, hex bytes,
   canonical text. Both repos hold a byte-identical copy and each asserts its SHA-256 against
   a constant. Cross-language parity is then *checked*, not assumed from two implementations
   of the same prose. Covers every opcode and value tag, empty strings, 4-byte UTF-8, a
   64-field descriptor, and `f64` edge values.
2. **Round-trip properties** — `proptest` over arbitrary contract types; total, because
   non-finite floats are rejected at encode.
3. **Fragmentation matrix** — the same message at `mtu ∈ {56, 64, 200, 512, 1016, 4096}`, plus
   `len == mtu`, `len == mtu-8`, `len == 0`.
4. **The interleave regression** — segment 40 KB at `mtu=56`, inject a complete `PING` after
   every 7th fragment, assert the blob is byte-identical and every `PING` arrives in order.
   Then inject a fragment with a mismatched opcode and assert `E_FRAGMENT_UNEXPECTED` rather
   than silent corruption. **This is the test that would have caught the original bug.**
5. **Truncation and fuzz** — every prefix of every vector must return `Incomplete` or a typed
   error, never panic; single-byte mutations likewise. The same corpus drives libFuzzer +
   ASan against the C codec off-target.

For that last point to be possible, **`src/proto/` MUST depend on nothing but `<stdint.h>` and
`<string.h>`** — no MCU headers. That single constraint is what lets the device codec run
under a sanitiser on a workstation.

Plus `ECHO`/`ECHO_REPLY` on real hardware: the device decodes and re-encodes a blob through
its own codec, exercising real USB timing and real interleaving — the one thing off-target
harnesses cannot do.

## 8. Open — needs measurement, not argument

- **FS bulk throughput on Windows.** ~0.7–1.0 MB/s is derived from USB scheduling rules, not
  measured. This is the number that decides bulk vs HID.
- `rx_window = 16`, arena 64 KB, `max_message_rx` 8192, FIFO depth 32 — plausible, unmeasured.
- 20 ms `VALUES` coalescing — chosen by reasoning, not from RA8876 composite timing.
  `HEARTBEAT.render_us_max` exists to correct it with data.
- Font atlas coverage for non-Latin UTF-8 — the protocol is clean; rendering is an asset call.
- Whether the USB-C socket is physically FS-only. Very strong inference from the pin table and
  the descriptor; confirm on the PCB before assuming High Speed is unreachable.
