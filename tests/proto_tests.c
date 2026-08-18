/* EMP/1 codec tests — one body, two homes.
 *
 * These run on the host (fast, in CI, no hardware) AND on the device via the `selftest`
 * console command. That is deliberate: the host run catches logic errors in seconds, and the
 * device run catches the things a host cannot — that the real compiler, the real alignment
 * behaviour and the real integer widths agree with the spec. A protocol tested only on the
 * host is a protocol whose first target-specific bug ships.
 *
 * No I/O, no allocation, no statics: the whole codec is pure functions over caller buffers, so
 * a test is just a buffer and a comparison.
 *
 * Every wire constant asserted here is pinned in docs/protocol.md. If the document changes,
 * these fail — which is the point. The document is normative; this file is its enforcement.
 */

#include <string.h>
#include "frame.h"

typedef void (*emp_report_fn)(const char *name, int passed);

#define CHECK(cond) do { if (!(cond)) { ok = 0; } } while (0)

static void run(const char *name, int ok, emp_report_fn report, int *failures)
{
    report(name, ok);
    if (!ok) (*failures)++;
}

/* ------------------------------------------------------------------ CRC */

static int test_crc(void)
{
    int ok = 1;
    /* The vector pinned in the spec. If this fails nothing else is meaningful. */
    CHECK(emp_crc32c((const uint8_t *)"123456789", 9) == 0xE3069283u);
    CHECK(emp_crc32c((const uint8_t *)"", 0) == 0x00000000u);

    /* Incremental must equal one-shot, or fragmenting a message would change its CRC. */
    uint32_t c = 0xFFFFFFFFu;
    c = emp_crc32c_update(c, (const uint8_t *)"12345", 5);
    c = emp_crc32c_update(c, (const uint8_t *)"6789", 4);
    CHECK((~c) == 0xE3069283u);
    return ok;
}

/* ------------------------------------------------------------------ constants */

static int test_constants(void)
{
    int ok = 1;
    CHECK(EMP_MAGIC == 0xE1u);
    CHECK(EMP_HEADER_BYTES == 8u);
    CHECK(EMP_PREFIX_BYTES == 8u);
    CHECK(EMP_MTU_HID == 56u);
    CHECK(EMP_MTU_BULK == 1016u);
    CHECK(EMP_VERSION_MAJOR == 1u);
    CHECK(EMP_FLAG_FIRST == 0x01u && EMP_FLAG_LAST == 0x02u);
    CHECK(EMP_CH_CONTROL == 0u && EMP_CH_SURFACE == 1u && EMP_CH_INPUT == 2u);
    return ok;
}

/* ------------------------------------------------------------------ header */

static int test_header(void)
{
    int ok = 1;
    uint8_t buf[EMP_HEADER_BYTES];

    emp_header_t h;
    h.version_major = EMP_VERSION_MAJOR;
    h.channel = EMP_CH_INPUT;
    h.opcode = EMP_OP_EDIT;
    h.flags = EMP_FLAG_FIRST | EMP_FLAG_LAST;
    h.seq = 0xBEEF;
    h.payload_len = 0;

    emp_header_write(buf, &h);

    /* Byte-for-byte, because this is the wire format and a "round-trips fine" test would
     * happily pass on a codec that is self-consistently wrong. */
    CHECK(buf[0] == 0xE1);
    CHECK(buf[1] == 0x12);            /* version 1, channel 2 */
    CHECK(buf[2] == EMP_OP_EDIT);
    CHECK(buf[3] == 0x03);
    CHECK(buf[4] == 0xEF && buf[5] == 0xBE);    /* little-endian */
    CHECK(buf[6] == 0x00 && buf[7] == 0x00);

    emp_header_t out;
    CHECK(emp_header_read(buf, sizeof(buf), &out) == EMP_OK);
    CHECK(out.channel == EMP_CH_INPUT && out.opcode == EMP_OP_EDIT);
    CHECK(out.seq == 0xBEEF && out.flags == 0x03 && out.payload_len == 0);

    /* Rejections. */
    CHECK(emp_header_read(buf, 4, &out) == EMP_ERR_SHORT);

    uint8_t bad[EMP_HEADER_BYTES];
    memcpy(bad, buf, sizeof(bad));
    bad[0] = 0x00;
    CHECK(emp_header_read(bad, sizeof(bad), &out) == EMP_ERR_BAD_MAGIC);

    memcpy(bad, buf, sizeof(bad));
    bad[1] = 0x22;                    /* version 2 */
    CHECK(emp_header_read(bad, sizeof(bad), &out) == EMP_ERR_BAD_VERSION);

    /* payload_len longer than the buffer must be short, not a buffer overrun. */
    memcpy(bad, buf, sizeof(bad));
    bad[6] = 0xFF; bad[7] = 0x00;
    CHECK(emp_header_read(bad, sizeof(bad), &out) == EMP_ERR_SHORT);
    return ok;
}

/* ------------------------------------------------------------------ round trip */

static void fill(uint8_t *p, uint32_t n, uint8_t seed)
{
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7u + (i >> 5));
}

/* Walk a packed transfer through the reassembler, exactly as the receive path does. */
static int feed_all(emp_reasm_t *r, const uint8_t *buf, uint32_t len,
                    const uint8_t **msg, uint32_t *msg_len, int *complete)
{
    uint32_t off = 0;
    *complete = 0;
    while (off < len) {
        uint32_t consumed = 0;
        emp_header_t h;
        int rc = emp_reasm_fragment(r, buf + off, len - off, &consumed, &h, msg, msg_len);
        if (rc < 0) return rc;
        if (rc == 1) *complete += 1;
        if (consumed == 0) return EMP_ERR_SHORT;
        off += consumed;
    }
    return EMP_OK;
}

static int test_single_fragment(void)
{
    int ok = 1;
    uint8_t payload[32], out[128];
    fill(payload, sizeof(payload), 0x11);

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), EMP_MTU_HID);
    CHECK(emp_write_message(&w, EMP_CH_CONTROL, EMP_OP_PING, payload, sizeof(payload)) == EMP_OK);
    CHECK(w.used == EMP_HEADER_BYTES + sizeof(payload));

    emp_reasm_t r;
    emp_reasm_init(&r);
    const uint8_t *msg = 0; uint32_t mlen = 0; int complete = 0;
    CHECK(feed_all(&r, out, w.used, &msg, &mlen, &complete) == EMP_OK);
    CHECK(complete == 1);
    CHECK(mlen == sizeof(payload));
    CHECK(msg && memcmp(msg, payload, sizeof(payload)) == 0);
    CHECK(r.rx_decode_errors == 0);
    return ok;
}

static int test_multi_fragment(uint16_t mtu)
{
    int ok = 1;
    static uint8_t payload[2000];
    static uint8_t out[4096];
    fill(payload, sizeof(payload), 0x5A);

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), mtu);
    CHECK(emp_write_message(&w, EMP_CH_SURFACE, EMP_OP_DESC_FIELD,
                            payload, sizeof(payload)) == EMP_OK);

    emp_reasm_t r;
    emp_reasm_init(&r);
    const uint8_t *msg = 0; uint32_t mlen = 0; int complete = 0;
    CHECK(feed_all(&r, out, w.used, &msg, &mlen, &complete) == EMP_OK);
    CHECK(complete == 1);
    CHECK(mlen == sizeof(payload));
    CHECK(msg && memcmp(msg, payload, sizeof(payload)) == 0);
    CHECK(r.rx_seq_gaps == 0);
    CHECK(r.rx_decode_errors == 0);
    return ok;
}

/* ------------------------------------------------------------------ failures */

static int test_control_never_fragments(void)
{
    int ok = 1;
    static uint8_t payload[200], out[512];
    fill(payload, sizeof(payload), 1);

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), EMP_MTU_HID);

    /* Rule F1. A control message that will not fit must be refused, not split — a split
     * control message can queue behind bulk, which is the priority inversion the whole design
     * exists to avoid. */
    CHECK(emp_write_message(&w, EMP_CH_CONTROL, EMP_OP_HEARTBEAT,
                            payload, sizeof(payload)) == EMP_ERR_BAD_CHANNEL);
    CHECK(w.used == 0);
    return ok;
}

static int test_crc_detects_corruption(void)
{
    int ok = 1;
    static uint8_t payload[300], out[1024];
    fill(payload, sizeof(payload), 0x33);

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), EMP_MTU_HID);
    CHECK(emp_write_message(&w, EMP_CH_SURFACE, EMP_OP_VALUES, payload, sizeof(payload)) == EMP_OK);

    /* Flip one bit in the middle of the payload. USB would never deliver this, but our own
     * reassembly could produce it, and that is precisely what the CRC is spent on. */
    out[w.used / 2] ^= 0x01;

    emp_reasm_t r;
    emp_reasm_init(&r);
    const uint8_t *msg = 0; uint32_t mlen = 0; int complete = 0;
    CHECK(feed_all(&r, out, w.used, &msg, &mlen, &complete) == EMP_ERR_CRC);
    CHECK(r.rx_decode_errors == 1);
    return ok;
}

static int test_interleaved_opcode_rejected(void)
{
    int ok = 1;
    static uint8_t payload[300], out[1024];
    fill(payload, sizeof(payload), 0x77);

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), EMP_MTU_HID);
    CHECK(emp_write_message(&w, EMP_CH_SURFACE, EMP_OP_VALUES, payload, sizeof(payload)) == EMP_OK);

    /* Corrupt the opcode of the SECOND fragment, simulating a continuation belonging to a
     * different message. This is the detector for the bug class that used to truncate a 43 KB
     * transfer into a plausible short one — the failure must be loud, not plausible. */
    uint32_t second = EMP_HEADER_BYTES + EMP_MTU_HID;
    out[second + 2] = EMP_OP_REVEAL;

    emp_reasm_t r;
    emp_reasm_init(&r);
    const uint8_t *msg = 0; uint32_t mlen = 0; int complete = 0;
    CHECK(feed_all(&r, out, w.used, &msg, &mlen, &complete) == EMP_ERR_FRAGMENT_UNEXPECTED);
    return ok;
}

static int test_truncation_is_an_error_not_a_crash(void)
{
    int ok = 1;
    static uint8_t payload[400], out[1024];
    fill(payload, sizeof(payload), 0x22);

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), EMP_MTU_HID);
    CHECK(emp_write_message(&w, EMP_CH_SURFACE, EMP_OP_VALUES, payload, sizeof(payload)) == EMP_OK);

    /* Feed every prefix of the transfer. None may complete a message, and none may run off the
     * end of the buffer. A decoder that only behaves on well-formed input is not a decoder. */
    for (uint32_t cut = 1; cut < w.used; cut++) {
        emp_reasm_t r;
        emp_reasm_init(&r);
        const uint8_t *msg = 0; uint32_t mlen = 0; int complete = 0;
        int rc = feed_all(&r, out, cut, &msg, &mlen, &complete);
        CHECK(rc <= 0);
        CHECK(complete == 0);
    }
    return ok;
}

static int test_padding(void)
{
    int ok = 1;
    uint8_t out[128];
    memset(out, 0, sizeof(out));

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), EMP_MTU_HID);
    uint8_t payload[4] = { 1, 2, 3, 4 };
    CHECK(emp_write_message(&w, EMP_CH_CONTROL, EMP_OP_PONG, payload, sizeof(payload)) == EMP_OK);

    /* A fixed-size report is zero-padded after the last fragment. The remainder must be
     * recognised as padding rather than counted as a decode error, which is what lets one
     * parser serve both a padded transport and a packed one. */
    CHECK(emp_is_padding(out + w.used, sizeof(out) - w.used) == 1);

    emp_header_t h;
    CHECK(emp_header_read(out + w.used, sizeof(out) - w.used, &h) == EMP_ERR_BAD_MAGIC);
    return ok;
}

static int test_seq_gap_counted(void)
{
    int ok = 1;
    static uint8_t payload[300], out[1024];
    fill(payload, sizeof(payload), 0x44);

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), EMP_MTU_HID);
    CHECK(emp_write_message(&w, EMP_CH_SURFACE, EMP_OP_VALUES, payload, sizeof(payload)) == EMP_OK);

    /* Bump the second fragment's sequence number: a dropped fragment, as a ring overrun would
     * produce. It must be COUNTED — a silently degraded link is the failure this protocol
     * exists to eliminate — even though the message then fails its length or CRC check. */
    uint32_t second = EMP_HEADER_BYTES + EMP_MTU_HID;
    out[second + 4] = (uint8_t)(out[second + 4] + 3u);

    emp_reasm_t r;
    emp_reasm_init(&r);
    const uint8_t *msg = 0; uint32_t mlen = 0; int complete = 0;
    (void)feed_all(&r, out, w.used, &msg, &mlen, &complete);
    CHECK(r.rx_seq_gaps >= 1);
    return ok;
}

/* THE INTERLEAVE REGRESSION — docs/protocol.md 7.4 calls this "the test that would have caught
 * the original bug", and it did not exist.
 *
 * Rule F3: a single-fragment message MAY arrive at any time, INCLUDING between fragments of a
 * multi-fragment one. That is not a permission the protocol grants grudgingly, it is the reason
 * the heartbeat lives on a channel that cannot be split -- a device transferring a descriptor
 * must still be able to say it is alive, and a host must still be able to ping it.
 *
 * The receiver rejected exactly that pattern as FRAGMENT_UNEXPECTED. So the one interleaving the
 * specification requires was the one interleaving refused, and because nothing here exercised
 * it, both the code and its comment claimed compliance.
 *
 * Scaled from the 40 KB in the document to fit this device's advertised max_message_rx; the
 * shape -- inject after every 7th fragment, assert the blob is byte-identical and every
 * injection arrives in order -- is the document's. */
static int test_interleaved_single_fragments(void)
{
    int ok = 1;
    static uint8_t payload[4000];
    static uint8_t out[8192];
    static emp_reasm_t r;                  /* static: an emp_reasm_t is 8 KB of stack otherwise */
    fill(payload, sizeof(payload), 0x31);

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), EMP_MTU_HID);
    CHECK(emp_write_message(&w, EMP_CH_SURFACE, EMP_OP_DESC_FIELD,
                            payload, sizeof(payload)) == EMP_OK);

    emp_reasm_init(&r);

    unsigned injected = 0, injections_seen = 0, blobs_seen = 0;
    unsigned frag_index = 0;
    uint32_t off = 0;

    while (off < w.used) {
        const uint8_t *msg = 0;
        uint32_t mlen = 0, consumed = 0;
        emp_header_t h;

        int rc = emp_reasm_fragment(&r, out + off, w.used - off, &consumed, &h, &msg, &mlen);
        CHECK(rc >= 0);
        if (rc == 1) {
            blobs_seen++;
            CHECK(mlen == sizeof(payload));
            CHECK(msg && memcmp(msg, payload, sizeof(payload)) == 0);
        }
        off += consumed;
        frag_index++;

        if ((frag_index % 7u) == 0 && off < w.used) {
            /* A complete PING, mid-transfer. Its payload carries the injection number, so
             * "arrived in order" is checked rather than merely "arrived". */
            uint8_t ping[12];
            uint8_t pbuf[64];
            fill(ping, sizeof(ping), (uint8_t)injected);

            emp_writer_t pw;
            emp_writer_init(&pw, pbuf, sizeof(pbuf), EMP_MTU_HID);
            CHECK(emp_write_message(&pw, EMP_CH_CONTROL, EMP_OP_PING,
                                    ping, sizeof(ping)) == EMP_OK);

            const uint8_t *pmsg = 0;
            uint32_t plen = 0, pconsumed = 0;
            emp_header_t ph;
            int prc = emp_reasm_fragment(&r, pbuf, pw.used, &pconsumed, &ph, &pmsg, &plen);

            CHECK(prc == 1);                       /* delivered, not refused */
            CHECK(ph.opcode == EMP_OP_PING);
            CHECK(plen == sizeof(ping));
            CHECK(pmsg && memcmp(pmsg, ping, sizeof(ping)) == 0);
            injections_seen++;
            injected++;
        }
    }

    CHECK(blobs_seen == 1);                        /* the blob survived, intact, once */
    CHECK(injections_seen == injected);
    CHECK(injections_seen > 3);                    /* the test actually interleaved something */

    /* No assertion on rx_seq_gaps: the injections come from a second writer with its own
     * counter, whereas a real sender interleaves both streams through one per-direction
     * sequence. Asserting here would be testing the fixture, not the receiver. */
    return ok;
}

/* A lost LAST fragment must not wedge the receiver.
 *
 * Before emp_reasm_abort() existed there was no way out of this state at all: in_progress
 * stayed set forever, so every later message was either an F2 violation or a continuation of a
 * transfer whose sender had long since given up. One lost fragment cost the link permanently,
 * and the only recovery was a device reset. */
static int test_reassembly_can_be_abandoned(void)
{
    int ok = 1;
    static uint8_t payload[300], out[1024];
    static emp_reasm_t r;
    fill(payload, sizeof(payload), 0x62);

    emp_writer_t w;
    emp_writer_init(&w, out, sizeof(out), EMP_MTU_HID);
    CHECK(emp_write_message(&w, EMP_CH_SURFACE, EMP_OP_VALUES, payload, sizeof(payload)) == EMP_OK);

    emp_reasm_init(&r);

    /* Everything except the final fragment, which is where the transfer stops. */
    uint32_t off = 0, last_start = 0;
    while (off < w.used) {
        emp_header_t h;
        CHECK(emp_header_read(out + off, w.used - off, &h) == EMP_OK);
        if (h.flags & EMP_FLAG_LAST) { last_start = off; break; }

        const uint8_t *msg = 0; uint32_t mlen = 0, consumed = 0;
        CHECK(emp_reasm_fragment(&r, out + off, w.used - off, &consumed, &h, &msg, &mlen) == 0);
        off += consumed;
    }
    CHECK(last_start != 0);
    CHECK(r.in_progress == 1);

    uint32_t errors_before = r.rx_decode_errors;
    emp_reasm_abort(&r);
    CHECK(r.in_progress == 0);
    CHECK(r.rx_decode_errors == errors_before + 1);   /* counted, never silent */

    /* And the receiver is usable again: a complete message straight afterwards must arrive. */
    emp_writer_t w2;
    emp_writer_init(&w2, out, sizeof(out), EMP_MTU_HID);
    CHECK(emp_write_message(&w2, EMP_CH_SURFACE, EMP_OP_VALUES, payload, sizeof(payload)) == EMP_OK);

    const uint8_t *msg = 0; uint32_t mlen = 0; int complete = 0;
    CHECK(feed_all(&r, out, w2.used, &msg, &mlen, &complete) == EMP_OK);
    CHECK(complete == 1);
    CHECK(mlen == sizeof(payload));
    CHECK(msg && memcmp(msg, payload, sizeof(payload)) == 0);
    return ok;
}

/* ------------------------------------------------------------------ entry */

int emp_run_selftests(emp_report_fn report)
{
    int failures = 0;
    run("crc32c",              test_crc(),                              report, &failures);
    run("wire constants",      test_constants(),                        report, &failures);
    run("header codec",        test_header(),                           report, &failures);
    run("single fragment",     test_single_fragment(),                  report, &failures);
    run("multi frag hid",      test_multi_fragment(EMP_MTU_HID),        report, &failures);
    run("multi frag bulk",     test_multi_fragment(EMP_MTU_BULK),       report, &failures);
    run("control unfragmented",test_control_never_fragments(),          report, &failures);
    run("crc catches flip",    test_crc_detects_corruption(),           report, &failures);
    run("opcode mismatch",     test_interleaved_opcode_rejected(),      report, &failures);
    run("truncation safe",     test_truncation_is_an_error_not_a_crash(), report, &failures);
    run("padding",             test_padding(),                          report, &failures);
    run("seq gap counted",     test_seq_gap_counted(),                  report, &failures);
    run("interleaved singles", test_interleaved_single_fragments(),       report, &failures);
    run("reasm abandonable",   test_reassembly_can_be_abandoned(),      report, &failures);
    return failures;
}
