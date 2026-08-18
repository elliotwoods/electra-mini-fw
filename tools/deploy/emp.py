"""
EMP/1 client — speaks the protocol to the device over the same COM port as the console.

    python emp.py                 # listen for READY / HEARTBEAT, then handshake and ping
    python emp.py --listen 10     # just watch unsolicited traffic for 10 seconds

This is the third independent implementation of the wire format, after the C codec and its
tests, and that is the point: a format that only round-trips through its own encoder proves
very little. Everything here is written from docs/protocol.md, not from frame.c.
"""

import argparse
import struct
import sys
import time

MAGIC = 0xE1
VERSION_MAJOR = 1
HEADER = 8

FLAG_FIRST = 0x01
FLAG_LAST = 0x02

CH_CONTROL = 0

OP = {
    0x01: "HELLO", 0x02: "READY", 0x03: "PING", 0x04: "PONG",
    0x05: "HEARTBEAT", 0x06: "DIAG", 0x07: "FLOW", 0x08: "BYE",
    0x0E: "ECHO", 0x0F: "ECHO_REPLY",
    0x20: "DESC_BEGIN", 0x21: "DESC_FIELD", 0x22: "DESC_END", 0x24: "VALUES",
    0x50: "EDIT", 0x54: "FOCUS", 0x55: "DESC_REQUEST", 0x56: "EDIT_DELTA",
    0x57: "DESC_ACK", 0x58: "BUTTON",
}
OP_HELLO, OP_READY, OP_PING, OP_PONG = 0x01, 0x02, 0x03, 0x04
OP_HEARTBEAT, OP_ECHO, OP_ECHO_REPLY = 0x05, 0x0E, 0x0F
OP_DESC_BEGIN, OP_DESC_FIELD, OP_DESC_END = 0x20, 0x21, 0x22
OP_VALUES = 0x24
OP_EDIT, OP_FOCUS, OP_DESC_REQUEST = 0x50, 0x54, 0x55
OP_EDIT_DELTA, OP_DESC_ACK, OP_BUTTON = 0x56, 0x57, 0x58
CH_SURFACE, CH_INPUT = 1, 2

# Pinned by docs/protocol.md; the C header asserts the same values, and the codec tests check
# them on both sides. A wire constant that lives in only one implementation is a latent bug.
PRESENT_MIN, PRESENT_MAX, PRESENT_STEP = 0x0001, 0x0002, 0x0004
PRESENT_PRECISION, PRESENT_UNIT = 0x0008, 0x0040
KIND_TOGGLE, KIND_NUMBER, KIND_CHOICE = 0, 1, 2
VAL_BOOL, VAL_NUMBER, VAL_CHOICE = 0x00, 0x01, 0x02


def crc32c(data: bytes) -> int:
    """Castagnoli, reflected. Pinned by the spec: crc32c(b'123456789') == 0xE3069283."""
    poly = 0x82F63B78
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (poly & -(crc & 1))
    return ~crc & 0xFFFFFFFF


def encode(channel: int, opcode: int, payload: bytes, seq: int, mtu: int = 1016):
    """Returns (bytes, next_seq). Single-fragment only — everything the host sends on the
    control channel must fit one fragment anyway (rule F1)."""
    if len(payload) > mtu:
        raise ValueError("control messages may not be fragmented (F1)")
    hdr = struct.pack("<BBBBHH", MAGIC, (VERSION_MAJOR << 4) | channel, opcode,
                      FLAG_FIRST | FLAG_LAST, seq & 0xFFFF, len(payload))
    return hdr + payload, (seq + 1) & 0xFFFF


class Decoder:
    """Byte-at-a-time, because the console shares this pipe: anything that is not a fragment
    is text and is handed back so it can still be read."""

    def __init__(self):
        self.buf = bytearray()
        self.need = 0
        self.text = bytearray()

    def feed(self, data: bytes):
        out = []
        for b in data:
            if not self.buf:
                if b != MAGIC:
                    self.text.append(b)
                    continue
                self.buf.append(b)
                self.need = HEADER
                continue

            self.buf.append(b)
            if len(self.buf) == HEADER:
                self.need = HEADER + struct.unpack_from("<H", self.buf, 6)[0]
            if len(self.buf) >= self.need:
                out.append(self._finish())
        return [m for m in out if m]

    def _finish(self):
        magic, ver_ch, opcode, flags, seq, plen = struct.unpack_from("<BBBBHH", self.buf, 0)
        payload = bytes(self.buf[HEADER:HEADER + plen])
        self.buf = bytearray()
        if (ver_ch >> 4) != VERSION_MAJOR:
            return {"error": "version %d" % (ver_ch >> 4)}
        return {"channel": ver_ch & 0x0F, "opcode": opcode, "flags": flags,
                "seq": seq, "payload": payload}

    def take_text(self) -> bytes:
        t = bytes(self.text)
        self.text = bytearray()
        return t


class Reader:
    def __init__(self, off=0):
        self.off = off

    def u8(self, b):  v = b[self.off]; self.off += 1; return v
    def u16(self, b): v = struct.unpack_from("<H", b, self.off)[0]; self.off += 2; return v
    def u32(self, b): v = struct.unpack_from("<I", b, self.off)[0]; self.off += 4; return v
    def u64(self, b): v = struct.unpack_from("<Q", b, self.off)[0]; self.off += 8; return v

    def string(self, b):
        n = self.u16(b)
        s = b[self.off:self.off + n].decode("utf-8", "replace")
        self.off += n
        return s


def parse_ready(p: bytes) -> dict:
    r = Reader()
    d = {}
    d["version"] = "%d.%d" % (r.u8(p), r.u8(p))
    d["mtu"] = r.u16(p)
    d["session_id"] = r.u32(p)
    d["capabilities"] = r.u32(p)
    d["max_message_rx"] = r.u32(p)
    d["max_descriptor_bytes"] = r.u32(p)
    d["max_fields"] = r.u16(p)
    d["fields_per_page"] = r.u16(p)
    d["applied_revision"] = r.u64(p)
    fw = r.u32(p)
    d["firmware"] = "%d.%d.%d" % ((fw >> 16) & 0xFF, (fw >> 8) & 0xFF, fw & 0xFF)
    d["build_number"] = r.u32(p)
    r.u32(p)                       # reserved

    # The fixed prefix ends here, and section 6 promises exactly this much stays stable across
    # major versions. Asserting it is the only way the promise means anything: a device that
    # quietly grew or shrank this would otherwise be discovered by a host misreading the strings.
    if r.off != 44:
        raise SystemExit("READY prefix is %d bytes, not the 44 the spec promises" % r.off)

    d["model"] = r.string(p)
    d["serial"] = r.string(p)
    d["build"] = r.string(p)
    return d


def parse_heartbeat(p: bytes) -> dict:
    r = Reader()
    d = {}
    d["session_id"] = r.u32(p)
    d["applied_revision"] = r.u64(p)
    d["uptime_ms"] = r.u32(p)
    d["page"] = r.u16(p)
    d["field_count"] = r.u16(p)
    d["state"] = r.u8(p)
    for k in ("rx_fragments", "tx_fragments", "rx_decode_errors",
              "rx_seq_gaps", "tx_dropped", "render_us_max"):
        d[k] = r.u32(p)
    return d


def field_record(fid, kind, label, value, lo=None, hi=None, step=None,
                 precision=None, unit=None, lane=0):
    """One DESC_FIELD body: everything after the u16 index.

    min/max/step always ride on the wire with a presence mask saying which are meaningful — 24
    bytes spent to buy a fixed-size prefix, so a bare-metal parser does one bounds check rather
    than branchy offset arithmetic."""
    present = 0
    if lo is not None: present |= PRESENT_MIN
    if hi is not None: present |= PRESENT_MAX
    if step is not None: present |= PRESENT_STEP
    if precision is not None: present |= PRESENT_PRECISION
    if unit is not None: present |= PRESENT_UNIT

    body = struct.pack("<HBBH", fid, kind, 0, present)
    body += struct.pack("<ddd", lo or 0.0, hi or 0.0, step or 0.0)
    body += struct.pack("<BBH", precision or 0, lane, 0)

    if kind == KIND_TOGGLE:
        body += struct.pack("<BB", VAL_BOOL, 1 if value else 0)
    elif kind == KIND_CHOICE:
        body += struct.pack("<BI", VAL_CHOICE, int(value))
    else:
        body += struct.pack("<Bd", VAL_NUMBER, float(value))

    lb = label.encode("utf-8")
    body += struct.pack("<H", len(lb)) + lb
    if unit is not None:
        ub = unit.encode("utf-8")
        body += struct.pack("<H", len(ub)) + ub

    return struct.pack("<H", len(body) + 2) + body


def parse_edit(p: bytes) -> dict:
    r = Reader()
    d = {"revision": r.u64(p), "edit_seq": r.u32(p), "id": r.u16(p), "cause": r.u8(p)}
    tag = r.u8(p)
    if tag == VAL_BOOL:     d["value"] = bool(r.u8(p))
    elif tag == VAL_CHOICE: d["value"] = r.u32(p)
    else:                   d["value"] = struct.unpack_from("<d", p, r.off)[0]
    return d


def parse_pong(p: bytes) -> dict:
    r = Reader()
    return {"ping_id": r.u32(p), "host_time_us": r.u64(p),
            "device_time_us": r.u64(p), "credits_total": r.u32(p)}


class Link:
    def __init__(self, port=None):
        sys.path.insert(0, __file__.rsplit("\\", 1)[0])
        from flash_usb import find_port
        from rawcom import RawCom
        self.port = find_port(port)
        self.c = RawCom(self.port)
        self.dec = Decoder()
        self.seq = 0
        time.sleep(0.2)
        self.c.read(65536)

    def send(self, opcode, payload=b"", channel=CH_CONTROL):
        frame, self.seq = encode(channel, opcode, payload, self.seq)
        self.c.write(frame)

    def pump(self, seconds):
        msgs = []
        end = time.time() + seconds
        while time.time() < end:
            chunk = self.c.read(65536)
            if chunk:
                msgs += self.dec.feed(chunk)
            else:
                time.sleep(0.005)
        return msgs

    def wait_for(self, opcode, timeout=2.0):
        """Return (message, seconds_waited), stopping the moment it arrives.

        The first version of this timed a fixed pump window and reported that as the round
        trip, so a 1.5 s window looked like 1.5 s of device latency. Measuring the wait rather
        than the timeout is the whole difference between a number and a misleading number.
        """
        t0 = time.time()
        while time.time() - t0 < timeout:
            chunk = self.c.read(65536)
            if chunk:
                for m in self.dec.feed(chunk):
                    if m.get("opcode") == opcode:
                        return m, time.time() - t0
            else:
                time.sleep(0.002)
        return None, time.time() - t0

    def close(self):
        self.c.close()


def describe(m):
    op = OP.get(m.get("opcode"), "op 0x%02X" % m.get("opcode", 0))
    p = m.get("payload", b"")
    if m.get("opcode") == OP_READY:
        return "READY " + ", ".join("%s=%s" % kv for kv in parse_ready(p).items())
    if m.get("opcode") == OP_HEARTBEAT:
        return "HEARTBEAT " + ", ".join("%s=%s" % kv for kv in parse_heartbeat(p).items())
    if m.get("opcode") == OP_PONG:
        return "PONG " + ", ".join("%s=%s" % kv for kv in parse_pong(p).items())
    return "%s (%d bytes)" % (op, len(p))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--listen", type=float, default=0)
    args = ap.parse_args()

    assert crc32c(b"123456789") == 0xE3069283, "CRC-32C does not match the pinned vector"

    link = Link(args.port)
    print("port: %s" % link.port)

    if args.listen:
        print("listening %.0fs for unsolicited traffic..." % args.listen)
        for m in link.pump(args.listen):
            print("  " + describe(m))
        link.close()
        return

    print("\n1. unsolicited traffic (the device should speak without being asked)")
    seen = link.pump(2.5)
    ready = [m for m in seen if m.get("opcode") == OP_READY]
    beats = [m for m in seen if m.get("opcode") == OP_HEARTBEAT]
    if ready:
        print("   " + describe(ready[0]))
    elif beats:
        # Not a failure. READY repeats only until a host has been seen; after that the device
        # switches to heartbeats. A device that already met a host this session is SUPPOSED to
        # look like this, and calling it "none seen" made correct behaviour look broken.
        print("   no READY, but %d heartbeat(s): the device has already seen a host this "
              "session and has stopped advertising" % len(beats))
    else:
        print("   nothing at all — the device is not talking unprompted")

    print("\n2. HELLO -> READY")
    link.send(OP_HELLO, struct.pack("<BBHQI", VERSION_MAJOR, 0, 1016, 0, 16))
    got = [m for m in link.pump(1.5) if m.get("opcode") == OP_READY]
    print("   %s" % (describe(got[0]) if got else "NO ANSWER"))

    print("\n3. PING -> PONG, round trip")
    stamps = []
    for i in range(5):
        t0 = time.time()
        link.send(OP_PING, struct.pack("<IQ", 0xC0FFEE + i, int(t0 * 1e6)))
        m, dt = link.wait_for(OP_PONG)
        if not m:
            print("   NO ANSWER on ping %d" % i)
            break
        d = parse_pong(m["payload"])
        if d["ping_id"] != 0xC0FFEE + i or d["host_time_us"] != int(t0 * 1e6):
            print("   echo WRONG on ping %d" % i)
            break
        stamps.append(dt * 1000)
    if stamps:
        print("   %d/5 answered: min %.1f ms, median %.1f ms, max %.1f ms"
              % (len(stamps), min(stamps), sorted(stamps)[len(stamps) // 2], max(stamps)))
        print("   host timestamp echoed back exactly, so liveness needs no clock agreement")

    print("\n4. ECHO round trip, several payload sizes")
    for n in (16, 512, 900):
        blob = bytes((i * 7 + n) & 0xFF for i in range(n))
        link.send(OP_ECHO, blob)
        m, dt = link.wait_for(OP_ECHO_REPLY, 2.0)
        if not m:
            print("   %4d bytes: NO ANSWER" % n)
        else:
            print("   %4d bytes: %d back in %.1f ms, %s"
                  % (n, len(m["payload"]), dt * 1000,
                     "identical" if m["payload"] == blob else "MISMATCH"))

    print("\n5. HEARTBEAT (unsolicited, ~1 Hz)")
    first, _ = link.wait_for(OP_HEARTBEAT, 3.0)
    second, gap = link.wait_for(OP_HEARTBEAT, 3.0)
    if first and second:
        hb = parse_heartbeat(second["payload"])
        print("   two seen, %.0f ms apart" % (gap * 1000))
        print("   uptime=%d ms  rx_frag=%d tx_frag=%d errors=%d gaps=%d dropped=%d"
              % (hb["uptime_ms"], hb["rx_fragments"], hb["tx_fragments"],
                 hb["rx_decode_errors"], hb["rx_seq_gaps"], hb["tx_dropped"]))
        if hb["rx_seq_gaps"]:
            # Expected across runs: this tool restarts its sequence at 0 while the device keeps
            # counting, so every fresh connection registers as one gap. That is the detector
            # doing its job, and it is reported rather than quietly swallowed.
            print("   (gaps are this tool reconnecting with a fresh sequence, not lost data)")
    else:
        print("   %s" % ("only one seen" if first else "none seen"))

    print("\n6. descriptor transaction")
    revision = 0x5EED0001
    specs = [
        (1, KIND_NUMBER, "Cutoff",  0.5,  dict(lo=0.0, hi=1.0, step=0.01, unit="Hz")),
        (2, KIND_NUMBER, "Reso",    0.2,  dict(lo=0.0, hi=1.0, step=0.01)),
        (3, KIND_TOGGLE, "Bypass",  0,    dict()),
        (4, KIND_CHOICE, "Shape",   1,    dict()),
        (5, KIND_NUMBER, "Drive",   0.0,  dict(lo=-24.0, hi=24.0, precision=1, unit="dB")),
        (6, KIND_NUMBER, "Mix",     1.0,  dict(lo=0.0, hi=1.0, step=0.05)),
        (7, KIND_NUMBER, "Attack",  0.01, dict(lo=0.0, hi=2.0, precision=3, unit="s")),
        (8, KIND_NUMBER, "Release", 0.30, dict(lo=0.0, hi=5.0, precision=2, unit="s")),
    ]
    bodies = [field_record(f, k, l, v, **kw) for (f, k, l, v, kw) in specs]

    total = sum(len(b) for b in bodies)
    link.send(OP_DESC_BEGIN, struct.pack("<QHHI", revision, len(bodies), 0, total),
              channel=CH_SURFACE)
    for i, body in enumerate(bodies):
        link.send(OP_DESC_FIELD, struct.pack("<H", i) + body, channel=CH_SURFACE)
        time.sleep(0.01)
    link.send(OP_DESC_END,
              struct.pack("<QHI", revision, len(bodies), crc32c(b"".join(bodies))),
              channel=CH_SURFACE)

    m, dt = link.wait_for(OP_DESC_ACK, 2.0)
    if m:
        rev, cnt = struct.unpack_from("<QH", m["payload"], 0)
        print("   DESC_ACK in %.1f ms: revision=0x%X, %d fields accepted"
              % (dt * 1000, rev, cnt))
    else:
        req, _ = link.wait_for(OP_DESC_REQUEST, 0.5)
        print("   NO ACK%s" % (" - device asked for a resend, reason %d" % req["payload"][0]
                               if req else ""))

    print("\n7. revision gate: a VALUES with an unknown revision must be refused")
    link.send(OP_VALUES, struct.pack("<QH", 0xDEADBEEF, 0), channel=CH_SURFACE)
    m, _ = link.wait_for(OP_DESC_REQUEST, 2.0)
    print("   %s" % ("DESC_REQUEST reason %d - refused, as it should be" % m["payload"][0]
                     if m else "NOT REFUSED - the gate is open"))

    print("\n8. turn a knob, push one, or press a button in the next 12 s")
    end = time.time() + 12
    seen = 0
    while time.time() < end and seen < 8:
        for m in link.pump(0.2):
            op = m.get("opcode")
            if op == OP_EDIT:
                d = parse_edit(m["payload"])
                v = d["value"]
                print("   EDIT   id=%d seq=%d cause=%d value=%s"
                      % (d["id"], d["edit_seq"], d["cause"],
                         ("%.4f" % v) if isinstance(v, float) else v))
                seen += 1
            elif op == OP_EDIT_DELTA:
                _, sq, fid, delta = struct.unpack_from("<QIHi", m["payload"], 0)
                print("   DELTA  id=%d seq=%d delta=%d" % (fid, sq, delta))
                seen += 1
            elif op == OP_FOCUS:
                _, have, fid, lane, mask = struct.unpack_from("<QBHBH", m["payload"], 0)
                print("   FOCUS  id=%d lane=%d touch_mask=0x%02X" % (fid, lane, mask))
            elif op == OP_BUTTON:
                print("   BUTTON %d %s" % (m["payload"][0],
                                           "down" if m["payload"][1] else "up"))
    if not seen:
        print("   (nothing touched)")

    # Bytes outside any fragment. Some is expected and benign: opening the port drains
    # mid-stream, so the tail of whatever was in flight arrives without a header and is treated
    # as text until the next magic byte resynchronises the decoder. That is the spec's resync
    # rule working, not a framing failure.
    text = link.dec.take_text()
    printable = bytes(c for c in text if 32 <= c < 127)
    if printable.strip():
        print("\nnon-fragment bytes (console output, or a partial frame from before we "
              "attached): %r" % printable[:100])
    link.close()


if __name__ == "__main__":
    main()
