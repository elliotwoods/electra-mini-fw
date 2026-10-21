# EMP/1 — Electra Mini Protocol v1.7

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
| `EMP_VERSION` | major 1, minor 7 |
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
feed `SurfaceCapabilities`; the firmware provider currently exposes the device's 64-field,
8-fields-per-page limits.

**`READY` prefix**, the 44 bytes §6 promises stable:

| off | size | field |
|---|---|---|
| 0 | u8 | protocol major |
| 1 | u8 | protocol minor, **negotiated** — `min(host, device)`, so the host reads back what was agreed |
| 2 | u16 | mtu the device can **receive**; not what it transmits with |
| 4 | u32 | `session_id` |
| 8 | u32 | capability flags |
| 12 | u32 | `max_message_rx` |
| 16 | u32 | `max_descriptor_bytes` |
| 20 | u16 | `max_fields` |
| 22 | u16 | `fields_per_page` |
| 24 | u64 | `applied_revision` |
| 32 | u32 | firmware version |
| 36 | u32 | firmware build |
| 40 | u32 | reserved, zero |

Then `model`, `serial`, `build_id` Strings, which are **not** covered by the promise. The
reserved word is there so one more capability can be added without spending the guarantee to
get it.

**`HELLO`**: `major` u8, `minor` u8, `host_mtu` u16, `host_epoch_ms` u64, `rx_window` u32,
`host_id` String.

The device sends `READY` unsolicited on boot and once per second until it has seen a `HELLO`,
and answers every `HELLO` idempotently. So a host attaching to a running device learns
identity without asking, and a host that starts first is not left waiting.

**A major-version mismatch still gets a `READY`.** §6 makes the mismatch terminal, but that
decision is the host's, and the host cannot report *which* firmware it refused without the
`build_id` that only `READY` carries. Refusing to answer would make an incompatible device
indistinguishable from a dead one.

**The transmit MTU is `min(host_mtu, device_mtu)`**, floored at `EMP_MTU_HID`. A host asking for
less than one HID report's worth is refused with `DIAG{E_MTU_REFUSED}` and the floor is used:
honouring it would fragment every message into uselessness.

**Dispatch is by channel first, then opcode.** The channel says which namespace the opcode is
drawn from, so reading the opcode first is reading a word before knowing its language — a
`SURFACE` message carrying opcode `0x01` is `DESC_*`-adjacent traffic, not a `HELLO`.

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
| `16` | `E_UNKNOWN_CHANNEL` |
| `17` | `E_VERSION_MISMATCH` |
| `18` | `E_MTU_REFUSED` |
| `19` | `E_GRAPHICS_TIMEOUT` — bounded accelerator completion wait expired |
| `20` | `E_GRAPHICS_FAULT` — accelerator initialization, command, memory, or bus failure |
| `21` | `E_INPUT_QUEUE_OVERFLOW` — deferred input queue exhausted during rendering |
| `22` | `E_STYLE_UNSUPPORTED` — EMP/1.6/1.7 style or tone refused and drawn as normal/neutral; context `(id << 8) \| value` |

A device MUST NOT send `DIAG` from inside a decoder. Framing an outbound message part-way
through an inbound one means building it from state the inbound parse is still walking, and on
a device that streams unframed data (see `TRANSPORT`) it can also land diagnostics in the middle
of an image. Raise, then flush from the service loop.

### 3.2 Channel 1 — SURFACE (host → device)

`0x20 DESC_BEGIN` · `0x21 DESC_FIELD` · `0x22 DESC_END` · `0x23 DESC_ABORT` ·
`0x24 VALUES` · `0x25 REVEAL` · `0x26 TRANSPORT` · `0x27 CLEAR`
· `0x28 HIGHLIGHT` (minor 3+) · `0x29 HIGHLIGHTS` (minor 4+) · `0x2A DEFAULTS` (minor 5+)
· `0x2B TONED_VALUES` (minor 6+)

**The descriptor is a transaction of per-field messages, not one giant message.** A 64-field
descriptor becomes ~64 messages of 60–250 bytes, most single-fragment even at `mtu=56`. The
device parses each directly into a staging entry — no multi-kilobyte reassembly buffer, no
allocation, interleaving points at message boundaries, and a failure names a field index
rather than "the transfer broke".

- `DESC_BEGIN`: `revision` u64, `field_count` u16, `flags` u16, `total_bytes` u32, `scope`.
- `DESC_FIELD`: transaction `index` u16 (MUST ascend by exactly 1) + field record (§3.4).
- `DESC_END`: `revision`, `field_count`, `crc32c` over the concatenated field payloads.
- `VALUES`: `revision` u64, `count` u16, then `count` × { `id` u16, `ack_edit` u32, Value }.
- `REVEAL`: `revision` u64 + absolute physical `id` u16. A valid request leaves drill-down,
  moves to `(id / fields_per_page) * fields_per_page`, and is confirmed by `SCREEN`.
- `CLEAR`: no payload. It clears the descriptor and page/focus/UI state and confirms an empty
  screen with `SCREEN { revision: 0, first_slot: 0, slot_count: 0 }` on EMP/1.1.

The field record's `id` is an absolute physical slot in `0..max_fields`; it is not the
transaction `index`. IDs may be sparse. A descriptor containing IDs `0..6, 8` therefore leaves
slot 7 blank and puts ID 8 on the second page. Duplicate or out-of-range IDs reject the entire
transaction without changing the active descriptor.

**Revision gate.** The device MUST drop `VALUES` / `REVEAL` / `TRANSPORT` whose `revision` is
neither 0 nor its own `applied_revision`, and answer `DESC_REQUEST{REVISION_UNKNOWN}`. This is
the mirror of the hub's own rule and closes the same hole in the other direction.

### 3.3 Channel 2 — INPUT (device → host)

`0x50 EDIT` · `0x51 RESET` · `0x52 PLAY_PAUSE` · `0x53 SCREEN` · `0x54 FOCUS` ·
`0x55 DESC_REQUEST` · `0x56 EDIT_DELTA` · `0x57 DESC_ACK` · `0x58 BUTTON` ·
`0x59 ACTIVATE` (minor 2+) · `0x5A SAVE_DEFAULT` (minor 5+)

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

`SCREEN` was added in EMP/1.1 and carries `revision u64`, absolute `first_slot u16`, and
`slot_count u16`. The half-open interval `[first_slot, first_slot + slot_count)` is authoritative
for the physical 2x4 knob bank; holes produce no `SurfaceInput::Screen` ID. The device emits it
after descriptor activation, every page change, a successful `REVEAL`, and `CLEAR`. A host
immediately publishes current values for occupied IDs in that interval and limits periodic `VALUES` traffic to them;
the heartbeat's page field is the recovery copy if a queued `SCREEN` is missed.
It is emitted only when HELLO negotiation selects minor version 1 or newer; EMP/1.0 peers keep
using the heartbeat page field and are never sent an opcode they did not negotiate.

Physical knob numbering is top-row-first: inputs 0–3 run left-to-right above the display,
4–7 below it. Generic pages with 1–4 occupied controls place them on bottom knobs 4–7,
left-to-right in ascending descriptor ID order. This includes short final pages and sparse
pages; unused top knobs have no field assignment. Pages with 5–8 controls use the normal
top-first slot positions. Descriptor IDs, page boundaries, SCREEN ranges and edit/focus IDs
remain unchanged. The opt-in operator layout retains its defined action/settings positions. Drilled Number
fields use the opposite knob row as a four-place digit window. Holding the rightmost button
(Zoom) turns every dial into a window control, clockwise towards the finer places, within the
value and precision bounds; the panel shows it in the strip between the rows. The drilled
buttons are, left to right, Exit, Undo, Redo, Store, Reset, Zoom (Store and Reset only while the
value differs from its default, §EMP/1.5). Choice and Color have no Zoom. Choice traverses without wrapping; Color uses R/G/B/optional-A component knobs while
focus and edits continue to identify the one aggregate field.

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

Color count is exactly 3 (RGB) or 4 (RGBA), with finite little-endian `f32` components. Other
counts and non-finite components are malformed. A Color field occupies one physical slot and
every `VALUES`, `EDIT`, default, correction, and replay carries the complete vector; component
lanes are never exposed as separate fallback Number fields.

**Non-finite `f64` or colour `f32` MUST NOT be encoded** — substitute 0.0 for a fresh Number,
or reject the complete colour without changing the live value; set `TRUNCATED` and emit
`DIAG{E_NON_FINITE}`. This keeps round-trip equality total, which is what makes property
testing possible.

**Field record**: `field_len` u16, `id` u16, `kind` u8, `flags` u8, `present` u16, `min` f64,
`max` f64, `step` f64, `precision` u8, `lane` u8, `choice_count` u16, then `value`, `label`,
`unit`, choices, optional `default`, optional `path`, then (minor 6+, `present & 0x0100`) the
EMP/1.6 `style` u8 and `tone` u8 — see "EMP/1.6" below for the complete record layout.

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

The current WinUSB implementation relies on USB bulk endpoint backpressure and does **not**
implement EMP `FLOW` messages or enforce host-side credits. The `credits_total`/`FLOW` wire
fields are reserved for a later throughput and priority-inversion pass; they are not an
advertised capability or a completed gate in this milestone.

**Host flood control:** coalesce pending `SurfaceValueChange`s per id to the latest, emit at
most one `VALUES` per 20 ms, and send only what the device reported visible via `SCREEN`. At
`fields_per_page = 8` that remains a small bounded bulk message.

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

`Idle` shows an **empty** surface, never a placeholder one. Until a `DESC_END` verifies, the
panel shows eight empty cells, so nothing the host did not send can appear on it and an operator
can always read the panel as a statement about the host. The firmware reaches `Idle` through a
short power-on sequence which draws the empty cells and the word READY; a descriptor arriving
during it cuts it short, so connecting to a device that has just been powered on is never slower
than connecting to one that has been on for an hour.

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


## EMP/1.2: actions and operator pages

Negotiation remains `min(host_minor, device_minor)`. EMP/1.0 and 1.1 peers retain
all existing descriptor, input and grid behaviour.

- Field kind 5 is a momentary **Action**, accepted only for negotiated minor >=2.
  Turning it, resetting it, holding it or releasing it cannot execute it.
- Input opcode `0x59` on the input channel is **ACTIVATE**: exactly 14 bytes,
  little endian `revision:u64, activation_sequence:u32, field_id:u16`.
  A press edge advances the session sequence. Hosts validate revision, field kind
  and monotonic wrapping sequence; duplicates and old revisions are discarded.
- The reserved u16 in DESC_BEGIN is now layout flags. Bit 0 opts into the operator
  layout for minor >=2. Zero retains the established grid. Hosts must send zero
  to older peers and omit Action fields when actions are unsupported.
- Text values are retained in a bounded 96-byte field buffer and repainted on
  VALUES updates. Long UTF-8 values truncate at a character boundary and emit
  STRING_TRUNCATED diagnostics.

The operator layout is an explicit slot contract, not automatic schema styling.
This opt-in layout retains its original descriptor slot contract: slots 0–3 correspond
to bottom knobs; 4–7 correspond to top knobs. Physical input indices remain top-first;
the layout alone maps its semantic slots to those inputs. Generic (non-operator) pages place the
page's first slot at the top left only when 5–8 of its slots are occupied; with 1–4 the top row
is empty and the lowest ID takes the bottom-left knob (§3.3, and EMP/1.6 below). Page 0 uses
0 axis, 1 jog step, 2 speed, 3 observed position text (optionally followed by a newline and operation status), 4 jog minus, 5 jog plus,
6 home, 7 stop. Press bottom-right for settings. Page 1 uses 8 route,
9 acceleration, 10 minimum speed, 11 target, 12 move, 13 connection Boolean,
14 operation text, 15 stop. Button 1 returns to page 0. Settings turn in place;
they never borrow the action row for digit editing. Other apps opt out by default.

## EMP/1.3: transient control activity

Host `0x28 HIGHLIGHT` on channel 1 is `revision u64 | have_id u8 | id u16 | lease_ms u16`
(exactly 13 bytes). The revision must equal the active descriptor. `have_id` is 0 or 1;
when 1 the ID must name an occupied slot and the lease must be 1..3000 ms. Invalid requests
are dropped. `have_id=0` clears immediately. Highlighting never changes the page, focused
editor, physical touch mask or field value. Descriptor replacement, page changes and clear
invalidate it; expiry is wrap-safe. The host renews a 1500 ms lease every 500 ms while the
interface control is active. Hosts negotiate minor 3 before sending it; older devices receive
no HIGHLIGHT operations.

Physical touch now emits the existing `FOCUS` packet as soon as the mask changes, even in
an overview, and renews every 500 ms while held. Sparse and operator layouts use the same
physical mapping as edits. A digit-editor knob reports its edited field. Releasing the last
touch reports the retained pressed editor, or no ID in overview. Touch does not open an editor.
The firmware and interface both draw local activity as solid cyan and mirrored activity as
dashed cyan; local activity wins when both name the same control.


## EMP/1.4: grouped transient control activity

Host `0x29 HIGHLIGHTS` on channel 1 is `revision u64 | count u8 | ids u16[count] | lease_ms u16`
(exactly `11 + 2*count` bytes). Count is 0..64. Every ID must name a distinct occupied
absolute slot below 64, the revision must equal the active descriptor, and a nonempty group
requires a lease of 1..3000 ms. Validation is atomic: an invalid member, duplicate, length,
revision or lease leaves the previous group unchanged. Count 0 clears the whole group.
This replaces the complete highlight set without changing page, editor, values or physical
input. The group expires and is invalidated under the same rules as EMP/1.3 HIGHLIGHT.

Hosts negotiate minor 4 before sending HIGHLIGHTS. Whole-control hover names every mapped
field (e.g. both XY components); component hover names only that component. Firmware retains
minor 3 HIGHLIGHT support for old hosts. Hosts fall back to the first field on a minor 3 device
and omit activity updates on older devices. Touch FOCUS remains one exact field/component.

## EMP/1.5: stored defaults

The device's Reset now means what the screen's reset means: the operator's saved default if
there is one, else the declared default. Store saves the current value as that default, as the
screen's save-as-default button does.

Host `0x2A DEFAULTS` on channel 1 is
`revision u64 | count u16 | { id u16 | has u8 | value (tag + payload, as 3.4) if has == 1 }[count]`.
It changes declared defaults **in place**: no descriptor transfer, so a drilled field, its digit
window and its undo history all survive. `has == 0` removes a default (Reset and Store disappear).
A stale revision is refused with `DESC_REQUEST(REVISION_UNKNOWN)` like `VALUES`; a text default is
treated as none.

Device `0x5A SAVE_DEFAULT` on channel 2 is exactly 10 bytes, `revision u64 | id u16`: store this
field's current value as its default. The device sends it after any `EDIT` for that field on the
same channel, so the host already holds the value it is asked to store; it does not carry a value
of its own. The device applies the new default locally at once and the host answers with
`DEFAULTS`, which either agrees or corrects.

Reset and Store are offered only while the value differs from its default, mirroring the screen's
column, and Store only once minor 5 is negotiated. "Differs" for a number uses the host's
tolerance, `|a - b| <= (|a| + |b|) * 1e-6 + 1e-9`, so an `f32` parameter's round trip through
`f64` is not reported as a change.

## EMP/1.6: field style and status tone

Two presentation properties, negotiated like every minor: a host sends them only when HELLO
negotiation selected minor 6 or newer. To an older peer the host encodes exactly as 1.5 (no
`0x0100` presence bit, no trailing bytes, `VALUES` instead of `TONED_VALUES`), so older-minor
traffic is byte-identical to what it was. Neither property changes what a control *does*.

| Value | `style` | `tone` |
|---|---|---|
| 0 | `NORMAL` | `NEUTRAL` — the usual ink |
| 1 | `ESTOP` — Action only | `OK` — green (`--ok` #34d399) |
| 2 | `HOLD` — Action only, minor 7+ (EMP/1.7 below) | `WARN` — amber (`--warn-soft-text` #fbbf24) |
| 3 | | `ALERT` — red (`--danger` #f87171) |

### Descriptor: trailing members of the field record

`EMP_PRESENT_STYLE = 0x0100` in `present` says the record ends with `style` u8, `tone` u8. The
complete `DESC_FIELD` payload, offsets from the start of the message:

| off | size | member |
|---|---|---|
| 0 | u16 | transaction `index` (not covered by the descriptor CRC) |
| 2 | u16 | `field_len` — bytes after this member, i.e. to the end of the record (the device does not rely on it) |
| 4 | u16 | `id` |
| 6 | u8 | `kind` (5 = Action, 3 = ReadOnly, ...) |
| 7 | u8 | `flags` (bit 1 `READONLY`) |
| 8 | u16 | `present` (bit 8 `0x0100` = STYLE) |
| 10 | f64 ×3 | `min`, `max`, `step` |
| 34 | u8 | `precision` |
| 35 | u8 | `lane` |
| 36 | u16 | `choice_count` |
| 38 | Value | current value (tag + body, §3.4) |
| … | String | `label` |
| … | String | `unit`, if `present & 0x0040` |
| … | String ×`choice_count` | choice labels |
| … | Value | `default`, if `present & 0x0010` |
| … | String | `path`, if `present & 0x0020` (hosts do not send it; the device steps over it) |
| … | u8 | **`style`**, if `present & 0x0100` |
| … | u8 | **`tone`**, if `present & 0x0100` |

`field_len` and the `DESC_END` CRC both cover the two new bytes. Example, the E-STOP Action at ID 0
with no unit, choices or default: `…| 05 | 00 | 00 01 | <24 zero bytes> | 00 00 00 00 | 01 <f64 0.0>
| 06 00 "E-STOP" | 01 00`.

Device rules:

- Negotiated minor < 6: the bit is cleared and the trailing bytes are left unread, as a 1.5 device
  does. The descriptor is otherwise accepted.
- Bit set but the two bytes missing: the record is truncated and the transaction is refused with
  `DESC_REQUEST(SEQUENCE_BROKEN)`; the live descriptor is kept (as for any truncated record).
- `style` > 1, or `style = ESTOP` on any kind but Action: the field loads, drawn `NORMAL`, with
  `TRUNCATED` set and `DIAG{E_STYLE_UNSUPPORTED}`. A non-Action is never drawn as a stop, since it
  cannot send one. (Once minor 7 is negotiated the limit is `style` > 2; see EMP/1.7.)
- `tone` > 3: the field loads with tone `NEUTRAL`, `TRUNCATED` and the same DIAG. An unknown tone
  is never mapped to the nearest known colour.
- A tone is accepted on any kind but drawn only on ReadOnly fields and on fields currently showing
  a Text value; on an editable number it would read as a range warning.

### Host `0x2B TONED_VALUES` (channel 1, minor 6+)

`revision u64 | count u16 | count × { id u16 | ack_edit u32 | tone u8 | Value }`

`VALUES` (§3.2) with a `tone` u8 between `ack_edit` and the Value of every entry, so a status and its
colour change in one message ("STOPPED" is never drawn for a frame in the green that meant
"ARMED"). To change only a colour, resend the current value with the new tone. Rules:

- Revision gate and `DESC_REQUEST(REVISION_UNKNOWN)` exactly as `VALUES`; nothing is applied.
- Unknown `id`: the entry is skipped and the batch continues. Extension (`0x80+`) or non-finite
  value: neither value nor tone changes for that entry; the batch continues. Reserved tag with no
  length, or a truncated message: the batch stops there; earlier entries stay applied.
- `tone` > 3: the value is applied, the tone becomes `NEUTRAL`, `DIAG{E_STYLE_UNSUPPORTED}`.
- Echo suppression (§3.5) withholds only the value; the tone is applied regardless, since the
  device never edits a tone.
- Plain `VALUES` never changes a tone; a descriptor replacement resets it to the declared one.
- A device whose negotiated minor is < 6 ignores the opcode, as a 1.5 device does.

### Rendering and input

- **ESTOP** Action, generic grid: the whole cell is the operator layout's stop plate (`C_STOP`,
  `#652529`, a card like the operator STOP), the field label centred in white with `PRESS` beneath.
  In the operator layout an estop Action on the action row takes the same red plate.
- It fires on the knob's press edge with an ordinary 14-byte `ACTIVATE` (EMP/1.2), exactly like
  any Action: turning, holding and releasing never fire it. One addition for the stop only: its
  knob fires it even while another field is drilled — including when the drill has made its row
  the digit editor and hidden it — and the drill is left so the stop is on screen. A plain Action
  keeps the old rule (under the digit row its press only exits). Device-local system and
  calibration views own the knobs; the stop does not fire there.
- **Tone**: the value ink (and the read-only fader) of a ReadOnly or Text-valued cell, the large
  drilled readout of such a field, and the operator layout's text readouts (slots 3 and 14) take the
  tone colour. `NEUTRAL` keeps today's ink.
- About 13 characters of status fit a generic cell in the UI face before the device shrinks and
  then truncates with `~`; keep a status word short and put detail in a second field.

### Which knob is top-left (generic pages)

Physical knob 0 is top-left. On a generic (non-operator) page the device counts the occupied slots
in the page's eight IDs `[first, first + 8)`:

- **5–8 occupied:** reading order, knob `k` shows slot `first + k`. The page's first ID (ID 0 on
  page 0) is top-left; if that ID is a hole, the top-left knob is empty.
- **1–4 occupied:** the top row is empty; occupied IDs fill bottom knobs 4–7 left to right in
  ascending ID order. Nothing is top-left.

So a stop placed at ID 0 is top-left only when page 0 carries at least five fields (IDs need not
be contiguous, but ID 0 must be occupied); with 2–4 fields it is bottom-left.


## EMP/1.7: hold-to-fire Action

One new `style` value. Everything else on the wire is EMP/1.6: the same `0x0100` trailing
`style` u8 + `tone` u8, the same 14-byte `ACTIVATE`, no new opcode, no new DIAG code.

| `style` | Name | Valid on |
|---|---|---|
| 2 | `HOLD` | Action (kind 5) only |

It exists so an operator can clear an E-stop from the panel without a stray knock doing it: the
host turns the status page's field ID 1 into a `HOLD` Action labelled "HOLD TO CLEAR" while the
fleet is stopped. What the activation does is the host's business (for the E-stop it returns the
fleet to Disarmed, never to Armed); the device only decides *when* it is sent.

### Negotiation

- A host sends `style = 2` only when HELLO negotiation selected minor 7 or newer. To a minor 6
  peer it sends a plain Action (or omits the field), never style 2.
- A device whose negotiated minor is 6 treats style 2 as EMP/1.6 says: unknown, drawn `NORMAL`,
  `TRUNCATED`, `DIAG{E_STYLE_UNSUPPORTED}`, and the Action then fires on its press edge like any
  plain Action. A host must therefore not rely on a minor 6 device to hold.
- A minor 7 device applies the EMP/1.6 rule with the limit raised to 2: `style` > 2, or `style`
  1 or 2 on any kind but Action, loads the field drawn `NORMAL` with `TRUNCATED` and
  `DIAG{E_STYLE_UNSUPPORTED}` (context `(id << 8) | style`). A non-Action is never drawn as a hold.

### Input

- The knob's press edge only **starts** a hold. Nothing is sent.
- After **1500 ms** of continuous hold the device sends the ordinary 14-byte `ACTIVATE`
  (EMP/1.2: `revision u64, activation_sequence u32, field_id u16`) exactly **once**, with the next
  activation sequence as for any press. The threshold is measured on the device's service clock
  (inputs are sampled every 20 ms service pass), so the send lands within one pass of 1500 ms.
- Releasing the knob before the threshold cancels silently: nothing is sent and the fill resets.
- Turning the held knob (any detent) before the threshold cancels it. Turning another knob does
  not.
- A hold never repeats. Holding on after it has fired sends nothing more; another activation
  needs a release and a new full hold.
- The hold is cancelled, silently, if before the threshold the knob stops addressing that same
  `HOLD` field of that same descriptor: a descriptor replacement (any new revision, even one that
  re-declares the same field), a page change, drilling into another field, or the device's own
  settings or calibration views. A host that re-sends the descriptor while the operator is holding
  makes them start again, so status changes belong in `TONED_VALUES`.
- There is **no drilled-field exception**: `HOLD` behaves as a plain Action does while another
  field is drilled (under the digit row its press only leaves the drill; nothing starts). Device-
  local system and calibration views own the knobs as usual. The EMP/1.6 `ESTOP` Action and plain
  Actions are unchanged, including the stop's drilled-field exception.
- Operator layout (DESC_BEGIN flag bit 0): a `HOLD` Action on the action row holds the same way.

### Rendering

- Generic grid: an ordinary cell (`--surface-raised`, not the stop plate), the field label at the
  top as usual, a large caption under it, and a horizontal fill across the foot of the cell.
  At rest the caption is `HOLD` in the usual ink and the fill is empty. While held, the caption and
  fill are amber (`--warn-soft-text` #fbbf24) and the fill grows left to right in proportion to
  the time held. Once `ACTIVATE` has been sent the caption reads `DONE` and the fill is full, both
  green (`--ok` #34d399), until the knob is released; release returns the cell to rest.
- Operator layout: the action-row card shows the label, `HOLD` / `DONE` beneath it in the same
  colours, and the same fill along the bottom of the card.
