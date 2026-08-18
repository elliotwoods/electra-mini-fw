/* Host tests for descriptor transfer: what the device shows while a transfer is in flight, and
 * what it shows after one fails.
 *
 * There were no tests here at all, which is how `desc_begin` came to clear the live descriptor
 * before the first field had arrived. Every test below fails against that version. The pattern
 * is always the same and is the reason this file exists: LOAD A GOOD DESCRIPTOR FIRST, then do
 * something that must not disturb it, then assert the good one is still there — because the bug
 * was never in the failure detection, which worked, but in what was left behind afterwards.
 *
 * The messages are built here by hand rather than by calling an encoder, deliberately. A test
 * that encodes with the same code it decodes with agrees with itself no matter what the wire
 * format says; these bytes are laid out from docs/protocol.md 3.4 so that a change to either
 * side has to be reconciled against the document.
 */

#include <stdio.h>
#include <string.h>

#include "emp.h"
#include "frame.h"
#include "surface.h"
#include "sim_wire.h"

typedef void (*desc_report_fn)(const char *name, int passed);

static int            failures;
static desc_report_fn report_fn;

static void check(const char *name, int passed)
{
    if (!passed) failures++;
    if (report_fn) report_fn(name, passed);
}

/* ------------------------------------------------------------------ wire building */

typedef struct { uint8_t b[512]; uint32_t n; } buf_t;

static void b_init(buf_t *b)              { b->n = 0; }
static void b8(buf_t *b, uint8_t v)       { if (b->n < sizeof(b->b)) b->b[b->n++] = v; }
static void b16(buf_t *b, uint16_t v)     { b8(b, (uint8_t)v); b8(b, (uint8_t)(v >> 8)); }
static void b32(buf_t *b, uint32_t v)     { b16(b, (uint16_t)v); b16(b, (uint16_t)(v >> 16)); }
static void b64(buf_t *b, uint64_t v)     { b32(b, (uint32_t)v); b32(b, (uint32_t)(v >> 32)); }

static void bf64(buf_t *b, double d)
{
    uint8_t t[8];
    memcpy(t, &d, 8);
    for (int i = 0; i < 8; i++) b8(b, t[i]);
}

static void bstr(buf_t *b, const char *s)
{
    uint16_t n = 0;
    while (s && s[n]) n++;
    b16(b, n);
    for (uint16_t i = 0; i < n; i++) b8(b, (uint8_t)s[i]);
}

/* A transfer under construction, so a test reads as the sequence of messages it is. */
typedef struct {
    uint64_t revision;
    uint16_t count;
    uint32_t crc;          /* running, over the field payloads, exactly as the device computes */
} xfer_t;

static void xfer_begin(xfer_t *x, uint64_t revision, uint16_t expect)
{
    buf_t b;
    b_init(&b);
    b64(&b, revision);
    b16(&b, expect);
    b16(&b, 0);                 /* flags */
    b32(&b, 0);                 /* total_bytes, advisory */

    x->revision = revision;
    x->count    = 0;
    x->crc      = 0xFFFFFFFFu;

    surf_handle(EMP_OP_DESC_BEGIN, b.b, b.n);
}

/* One field. `value_tag` and the raw `vbody` bytes are parameters so a test can put a tag this
 * decoder has never heard of on the wire, with exactly the body that tag's rule requires --
 * including, in one case, no body at all. */
static void xfer_field(xfer_t *x, uint16_t index, uint16_t id, const char *label,
                       double value, const char *unit,
                       uint8_t value_tag, const uint8_t *vbody, uint16_t vbody_len)
{
    buf_t b;
    b_init(&b);
    b16(&b, index);

    uint32_t body = b.n;        /* the CRC covers everything after the index */

    b16(&b, 0);                 /* field_len, forward compatibility */
    b16(&b, id);
    b8(&b, EMP_KIND_NUMBER);
    b8(&b, 0);                  /* flags */
    b16(&b, (uint16_t)(EMP_PRESENT_MIN | EMP_PRESENT_MAX | (unit ? EMP_PRESENT_UNIT : 0)));
    bf64(&b, 0.0);              /* min */
    bf64(&b, 1.0);              /* max */
    bf64(&b, 0.0);              /* step */
    b8(&b, 2);                  /* precision */
    b8(&b, 0);                  /* lane */
    b16(&b, 0);                 /* choice_count */

    b8(&b, value_tag);
    if (value_tag == EMP_VAL_NUMBER && !vbody) {
        bf64(&b, value);
    } else {
        for (uint16_t i = 0; i < vbody_len; i++) b8(&b, vbody[i]);
    }

    bstr(&b, label);
    if (unit) bstr(&b, unit);

    x->crc = emp_crc32c_update(x->crc, b.b + body, b.n - body);
    x->count++;

    surf_handle(EMP_OP_DESC_FIELD, b.b, b.n);
}

/* The ordinary case: a plain number field with a label. */
static void xfer_number(xfer_t *x, uint16_t index, uint16_t id, const char *label, double value)
{
    xfer_field(x, index, id, label, value, 0, EMP_VAL_NUMBER, 0, 0);
}

static void xfer_end(xfer_t *x, uint32_t crc_override, int use_override)
{
    buf_t b;
    b_init(&b);
    b64(&b, x->revision);
    b16(&b, x->count);
    b32(&b, use_override ? crc_override : ~x->crc);
    surf_handle(EMP_OP_DESC_END, b.b, b.n);
}

/* ------------------------------------------------------------------ inspection */

static int label_is(uint16_t index, const char *want)
{
    const surf_field_t *f = surf_field(index);
    if (!f) return 0;

    uint16_t n = 0;
    while (want[n]) n++;
    if (f->label_len != n) return 0;

    const char *s = surf_string(f->label_off, f->label_len);
    return s && memcmp(s, want, n) == 0;
}

static uint8_t last_request_reason(void)
{
    const sim_wire_msg_t *m = sim_wire_last(EMP_OP_DESC_REQUEST);
    return (m && m->len >= 1) ? m->payload[0] : 0xFFu;
}

/* Two fields at revision 7, verified. The starting point for every failure test below. */
static void load_good(void)
{
    xfer_t x;
    surf_init();
    sim_wire_reset();

    xfer_begin(&x, 7, 2);
    xfer_number(&x, 0, 101, "Cutoff", 0.5);
    xfer_number(&x, 1, 102, "Resonance", 0.25);
    xfer_end(&x, 0, 0);
}

static int good_is_intact(void)
{
    return surf_field_count() == 2
        && surf_applied_revision() == 7
        && label_is(0, "Cutoff")
        && label_is(1, "Resonance")
        && surf_field(0) && surf_field(0)->id == 101;
}

/* ------------------------------------------------------------------ tests */

static void test_round_trip(void)
{
    load_good();

    const sim_wire_msg_t *ack = sim_wire_last(EMP_OP_DESC_ACK);
    int acked = ack && ack->len >= 10 && ack->payload[0] == 7 && ack->payload[8] == 2;

    check("descriptor round trip", good_is_intact() && acked);
}

static void test_live_survives_transfer(void)
{
    load_good();

    /* A second transfer starts and gets partway. Nothing has been verified yet, so the panel
     * must still be showing the descriptor it already had -- for the whole duration, not just
     * on failure. This is the case that made every host reload flicker through an empty screen. */
    xfer_t x;
    xfer_begin(&x, 9, 3);
    xfer_number(&x, 0, 201, "Attack", 0.1);
    xfer_number(&x, 1, 202, "Decay", 0.2);

    check("live descriptor survives a transfer in flight", good_is_intact());
}

static void test_crc_mismatch_keeps_live(void)
{
    load_good();
    sim_wire_reset();

    xfer_t x;
    xfer_begin(&x, 9, 1);
    xfer_number(&x, 0, 201, "Attack", 0.1);
    xfer_end(&x, 0xDEADBEEFu, 1);          /* arrived complete and wrong */

    check("a CRC mismatch leaves the live descriptor alone",
          good_is_intact() && last_request_reason() == EMP_REQ_CRC_MISMATCH);
}

static void test_sequence_break_keeps_live(void)
{
    load_good();
    sim_wire_reset();

    xfer_t x;
    xfer_begin(&x, 9, 3);
    xfer_number(&x, 0, 201, "Attack", 0.1);
    xfer_number(&x, 2, 203, "Sustain", 0.3);   /* index 1 was lost */

    check("a broken sequence leaves the live descriptor alone",
          good_is_intact() && last_request_reason() == EMP_REQ_SEQUENCE_BROKEN);
}

static void test_truncated_field_keeps_live(void)
{
    load_good();
    sim_wire_reset();

    xfer_t x;
    xfer_begin(&x, 9, 1);

    /* A field record that stops in the middle of its own bounds. */
    buf_t b;
    b_init(&b);
    b16(&b, 0);                 /* index */
    b16(&b, 0);                 /* field_len */
    b16(&b, 201);               /* id */
    b8(&b, EMP_KIND_NUMBER);
    b8(&b, 0);
    b16(&b, EMP_PRESENT_MIN | EMP_PRESENT_MAX);
    bf64(&b, 0.0);              /* min, and then nothing */
    surf_handle(EMP_OP_DESC_FIELD, b.b, b.n);
    (void)x;

    check("a truncated field leaves the live descriptor alone",
          good_is_intact() && last_request_reason() == EMP_REQ_SEQUENCE_BROKEN);
}

static void test_abort_keeps_live(void)
{
    load_good();

    xfer_t x;
    xfer_begin(&x, 9, 2);
    xfer_number(&x, 0, 201, "Attack", 0.1);
    surf_handle(EMP_OP_DESC_ABORT, 0, 0);

    check("an aborted transfer leaves the live descriptor alone", good_is_intact());
}

static void test_too_many_fields_keeps_live(void)
{
    load_good();
    sim_wire_reset();

    xfer_t x;
    xfer_begin(&x, 9, SURF_MAX_FIELDS + 1);

    check("a descriptor too large to hold is refused without disturbing the live one",
          good_is_intact() && last_request_reason() == EMP_REQ_TOO_MANY_FIELDS);
}

static void test_stale_values_do_not_apply(void)
{
    load_good();
    sim_wire_reset();

    /* The revision gate and the arena swap have to agree about what "loaded" means, and they
     * are now two separate mechanisms. If a future change published the revision before the
     * fields -- an obvious-looking simplification -- values stamped with the new revision would
     * pass the gate and match nothing, silently. The device must still be at revision 7 and must
     * ask for the descriptor it does not have. */
    xfer_t x;
    xfer_begin(&x, 9, 1);
    xfer_number(&x, 0, 201, "Attack", 0.1);
    xfer_end(&x, 0xDEADBEEFu, 1);

    buf_t b;
    b_init(&b);
    b64(&b, 9);                 /* revision */
    b16(&b, 1);                 /* count */
    b16(&b, 201);               /* id */
    b32(&b, 0);                 /* ack_edit */
    b8(&b, EMP_VAL_NUMBER);
    bf64(&b, 0.75);
    sim_wire_reset();
    surf_handle(EMP_OP_VALUES, b.b, b.n);

    check("values at a revision that failed to load are refused",
          surf_applied_revision() == 7 && last_request_reason() == EMP_REQ_REVISION_UNKNOWN);
}

static void test_successive_descriptors_replace(void)
{
    load_good();

    /* Three in a row. Two arenas mean the storage alternates, so a swap that goes the wrong way,
     * or a scratch arena that is not cleared between transfers, shows up by the third. */
    for (unsigned round = 0; round < 3; round++) {
        xfer_t x;
        xfer_begin(&x, 20 + round, 1);
        xfer_number(&x, 0, (uint16_t)(300 + round), (round & 1) ? "Odd" : "Even", 0.5);
        xfer_end(&x, 0, 0);

        int ok = surf_field_count() == 1
              && surf_applied_revision() == 20 + round
              && surf_field(0)->id == 300 + round
              && label_is(0, (round & 1) ? "Odd" : "Even");
        if (!ok) { check("successive descriptors replace cleanly", 0); return; }
    }
    check("successive descriptors replace cleanly", 1);
}

static void test_pool_does_not_accumulate(void)
{
    /* The string pool is per arena and reset at DESC_BEGIN. If it were not, repeated transfers
     * would exhaust it and labels would start coming back empty -- a device that works when you
     * connect and loses its labels an hour later. */
    surf_init();
    sim_wire_reset();

    for (unsigned round = 0; round < 200; round++) {
        xfer_t x;
        xfer_begin(&x, 1 + round, 2);
        xfer_number(&x, 0, 400, "A rather long field label", 0.5);
        xfer_number(&x, 1, 401, "Another long field label", 0.5);
        xfer_end(&x, 0, 0);
    }
    check("the string pool does not accumulate across transfers",
          surf_field_count() == 2 && label_is(0, "A rather long field label"));
}

/* ------------------------------------------------------------------ value tags */

/* Every one of these puts a tag this device does not render in front of a label it must still
 * read. The label is the assertion: if the value's body was not stepped over by exactly the
 * right number of bytes, the label length is read from the middle of the value and everything
 * after it is nonsense -- while the CRC still checks out, because the bytes did arrive intact.
 * That is what made this failure so quiet: a descriptor that verified and was wrong. */
static void check_skippable(const char *name, uint8_t tag, const uint8_t *body, uint16_t len)
{
    xfer_t x;
    surf_init();
    sim_wire_reset();

    xfer_begin(&x, 11, 2);
    xfer_field(&x, 0, 501, "Skipped", 0.0, "Hz", tag, body, len);
    xfer_number(&x, 1, 502, "Intact", 0.5);
    xfer_end(&x, 0, 0);

    const surf_field_t *f = surf_field(0);
    int ok = surf_field_count() == 2
          && label_is(0, "Skipped")
          && label_is(1, "Intact")
          && f && f->id == 501
          && f->unit_len == 2                       /* the unit followed the label, and landed */
          /* Normalised to something renderable, so nothing downstream has to know the tag. */
          && f->value_tag == EMP_VAL_NUMBER;

    check(name, ok);
}

static void test_extension_tag_is_skipped(void)
{
    /* 0x80-0xFF: len u16 + bytes, which exists so exactly this can be survived. */
    static const uint8_t body[] = { 0x05, 0x00, 'h', 'e', 'l', 'l', 'o' };
    check_skippable("an extension value tag is stepped over, not misread", 0x81u,
                    body, (uint16_t)sizeof(body));
}

static void test_text_tag_is_skipped(void)
{
    static const uint8_t body[] = { 0x03, 0x00, 'a', 'b', 'c' };
    check_skippable("a Text value is stepped over, not misread", EMP_VAL_TEXT,
                    body, (uint16_t)sizeof(body));
}

static void test_color_tag_is_skipped(void)
{
    /* count u8 + count x f32. Colour is deferred, which is not the same as unknown. */
    static const uint8_t body[] = { 0x03,
                                    0, 0, 0, 0,
                                    0, 0, 0x80, 0x3F,
                                    0, 0, 0, 0 };
    check_skippable("a Color value is stepped over, not misread", EMP_VAL_COLOR,
                    body, (uint16_t)sizeof(body));
}

static void test_reserved_tag_abandons_the_record(void)
{
    /* 0x05-0x7F is reserved and carries NO length. There is no honest way to find the end of
     * the value, so guessing is the one thing that must not happen -- the field would load and
     * be wrong, which is worse than not loading. */
    load_good();
    sim_wire_reset();

    xfer_t x;
    xfer_begin(&x, 9, 1);
    xfer_field(&x, 0, 601, "Mystery", 0.0, 0, 0x40u, (const uint8_t *)"", 0);

    check("a reserved value tag abandons the record rather than guessing",
          good_is_intact() && last_request_reason() == EMP_REQ_SEQUENCE_BROKEN);
}

static void test_extension_default_is_dropped_not_fatal(void)
{
    /* The default sits between the choices and the optional path. Nothing reads `path` yet, so
     * failing to consume the default is currently harmless -- which is exactly why this test is
     * here: it is the one that starts failing the day `path` is parsed, rather than `path`
     * quietly decoding from the middle of a default nobody stepped over. Losing Reset on one
     * control is acceptable; losing the field, or silently misreading what follows, is not. */
    xfer_t x;
    surf_init();
    sim_wire_reset();

    buf_t b;
    b_init(&b);
    b16(&b, 0);                                   /* index */
    uint32_t body = b.n;
    b16(&b, 0);                                   /* field_len */
    b16(&b, 701);                                 /* id */
    b8(&b, EMP_KIND_NUMBER);
    b8(&b, 0);
    b16(&b, EMP_PRESENT_DEFAULT);
    bf64(&b, 0.0); bf64(&b, 1.0); bf64(&b, 0.0);  /* min, max, step */
    b8(&b, 2); b8(&b, 0);                         /* precision, lane */
    b16(&b, 0);                                   /* choice_count */
    b8(&b, EMP_VAL_NUMBER); bf64(&b, 0.5);        /* value */
    bstr(&b, "Defaulted");                        /* label */
    b8(&b, 0x90u);                                /* default: an extension tag */
    b16(&b, 2); b8(&b, 0xAA); b8(&b, 0xBB);

    xfer_begin(&x, 12, 1);
    x.crc = emp_crc32c_update(x.crc, b.b + body, b.n - body);
    x.count++;
    surf_handle(EMP_OP_DESC_FIELD, b.b, b.n);
    xfer_end(&x, 0, 0);

    const surf_field_t *f = surf_field(0);
    check("an extension default is dropped without losing the field",
          surf_field_count() == 1 && label_is(0, "Defaulted")
          && f && f->id == 701 && (f->present & EMP_PRESENT_DEFAULT) == 0);
}

static void test_values_skip_one_control_not_the_batch(void)
{
    load_good();
    sim_wire_reset();

    /* Two values, the first carrying a tag from a newer host. The second must still land. */
    buf_t b;
    b_init(&b);
    b64(&b, 7);
    b16(&b, 2);

    b16(&b, 101); b32(&b, 0);
    b8(&b, 0x82u); b16(&b, 3); b8(&b, 1); b8(&b, 2); b8(&b, 3);

    b16(&b, 102); b32(&b, 0);
    b8(&b, EMP_VAL_NUMBER); bf64(&b, 0.875);

    surf_handle(EMP_OP_VALUES, b.b, b.n);

    const surf_field_t *f = surf_field(1);
    check("an unreadable value costs one control, not the batch",
          f && f->number > 0.874 && f->number < 0.876);
}

/* ------------------------------------------------------------------ runner */

int desc_run_selftests(desc_report_fn report)
{
    failures  = 0;
    report_fn = report;

    test_round_trip();
    test_live_survives_transfer();
    test_crc_mismatch_keeps_live();
    test_sequence_break_keeps_live();
    test_truncated_field_keeps_live();
    test_abort_keeps_live();
    test_too_many_fields_keeps_live();
    test_stale_values_do_not_apply();
    test_successive_descriptors_replace();
    test_pool_does_not_accumulate();

    test_extension_tag_is_skipped();
    test_text_tag_is_skipped();
    test_color_tag_is_skipped();
    test_reserved_tag_abandons_the_record();
    test_extension_default_is_dropped_not_fatal();
    test_values_skip_one_control_not_the_batch();

    report_fn = 0;
    return failures;
}
