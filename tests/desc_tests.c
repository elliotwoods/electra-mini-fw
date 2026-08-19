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
#include "ui_state.h"
#include "sim_wire.h"
#include "diag.h"

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

static void bf32(buf_t *b, float f)
{
    uint8_t t[4];
    memcpy(t, &f, 4);
    for (int i = 0; i < 4; i++) b8(b, t[i]);
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
    b8(&b, value_tag == EMP_VAL_COLOR ? EMP_KIND_COLOR : EMP_KIND_NUMBER);
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
    xfer_number(&x, 0, 0, "Cutoff", 0.5);
    xfer_number(&x, 1, 1, "Resonance", 0.25);
    xfer_end(&x, 0, 0);
}

static void load_sparse(void)
{
    xfer_t x;
    surf_init();
    ui_state_init();
    sim_wire_reset();

    xfer_begin(&x, 42, 8);
    for (uint16_t i = 0; i < 7; i++) xfer_number(&x, i, i, "First page", i / 10.0);
    xfer_number(&x, 7, 8, "Second page", 0.8);
    xfer_end(&x, 0, 0);
}

static int good_is_intact(void)
{
    return surf_field_count() == 2
        && surf_applied_revision() == 7
        && label_is(0, "Cutoff")
        && label_is(1, "Resonance")
        && surf_field(0) && surf_field(0)->id == 0;
}

/* ------------------------------------------------------------------ tests */

static void test_round_trip(void)
{
    load_good();

    const sim_wire_msg_t *ack = sim_wire_last(EMP_OP_DESC_ACK);
    int acked = ack && ack->len >= 10 && ack->payload[0] == 7 && ack->payload[8] == 2;
    const sim_wire_msg_t *screen = sim_wire_last(EMP_OP_SCREEN);
    int visible = screen && screen->len == 12 && screen->payload[0] == 7
               && screen->payload[8] == 0 && screen->payload[10] == 2;

    check("descriptor round trip and initial screen", good_is_intact() && acked && visible);
}

static void test_sparse_slots_preserve_holes_and_pages(void)
{
    load_sparse();
    const sim_wire_msg_t *screen = sim_wire_last(EMP_OP_SCREEN);
    int ok = surf_field_count() == 8 && surf_slot_span() == 9
          && surf_field(6) && !surf_field(7) && surf_field(8)
          && label_is(8, "Second page")
          && screen && screen->len == 12
          && screen->payload[8] == 0 && screen->payload[10] == 8;

    surf_set_number(7, 0.7);
    ok = ok && sim_wire_last(EMP_OP_EDIT) == 0;
    surf_set_number(8, 0.9);
    const sim_wire_msg_t *edit = sim_wire_last(EMP_OP_EDIT);
    ok = ok && edit && edit->len >= 14 && edit->payload[12] == 8;
    check("sparse slots stay blank and never cross page boundaries", ok);
}

static void test_duplicate_and_out_of_range_slots_keep_live(void)
{
    load_good();
    xfer_t x;
    xfer_begin(&x, 50, 2);
    xfer_number(&x, 0, 0, "One", 0.1);
    xfer_number(&x, 1, 0, "Duplicate", 0.2);
    int duplicate_kept = good_is_intact();

    xfer_begin(&x, 51, 1);
    xfer_number(&x, 0, SURF_MAX_FIELDS, "Outside", 0.3);
    check("duplicate and out-of-range slots reject the transaction",
          duplicate_kept && good_is_intact());
}

static void test_live_survives_transfer(void)
{
    load_good();

    /* A second transfer starts and gets partway. Nothing has been verified yet, so the panel
     * must still be showing the descriptor it already had -- for the whole duration, not just
     * on failure. This is the case that made every host reload flicker through an empty screen. */
    xfer_t x;
    xfer_begin(&x, 9, 3);
    xfer_number(&x, 0, 0, "Attack", 0.1);
    xfer_number(&x, 1, 1, "Decay", 0.2);

    check("live descriptor survives a transfer in flight", good_is_intact());
}

static void test_crc_mismatch_keeps_live(void)
{
    load_good();
    sim_wire_reset();

    xfer_t x;
    xfer_begin(&x, 9, 1);
    xfer_number(&x, 0, 0, "Attack", 0.1);
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
    xfer_number(&x, 0, 0, "Attack", 0.1);
    xfer_number(&x, 2, 2, "Sustain", 0.3);   /* index 1 was lost */

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
    b16(&b, 0);                 /* id */
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
    xfer_number(&x, 0, 0, "Attack", 0.1);
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
    xfer_number(&x, 0, 0, "Attack", 0.1);
    xfer_end(&x, 0xDEADBEEFu, 1);

    buf_t b;
    b_init(&b);
    b64(&b, 9);                 /* revision */
    b16(&b, 1);                 /* count */
    b16(&b, 0);                 /* id */
    b32(&b, 0);                 /* ack_edit */
    b8(&b, EMP_VAL_NUMBER);
    bf64(&b, 0.75);
    sim_wire_reset();
    surf_handle(EMP_OP_VALUES, b.b, b.n);

    check("values at a revision that failed to load are refused",
          surf_applied_revision() == 7 && last_request_reason() == EMP_REQ_REVISION_UNKNOWN);
}

static void test_values_apply_across_a_hole(void)
{
    load_sparse();
    buf_t b;
    b_init(&b);
    b64(&b, 42); b16(&b, 2);
    b16(&b, 7); b32(&b, 0); b8(&b, EMP_VAL_NUMBER); bf64(&b, 0.7);
    b16(&b, 8); b32(&b, 0); b8(&b, EMP_VAL_NUMBER); bf64(&b, 0.875);
    surf_handle(EMP_OP_VALUES, b.b, b.n);
    check("values skip holes and still reach later slots",
          !surf_field(7) && surf_field(8) && surf_field(8)->number == 0.875);
}

static void test_reveal_and_clear_confirm_physical_screen(void)
{
    load_sparse();
    buf_t b;
    b_init(&b); b64(&b, 42); b16(&b, 8);
    sim_wire_reset();
    surf_handle(EMP_OP_REVEAL, b.b, b.n);
    const sim_wire_msg_t *screen = sim_wire_last(EMP_OP_SCREEN);
    int ok = surf_page() == 8 && ui_state()->focused < 0
          && screen && screen->len == 12
          && screen->payload[8] == 8 && screen->payload[10] == 1;

    b_init(&b); b64(&b, 41); b16(&b, 0);
    sim_wire_reset();
    surf_handle(EMP_OP_REVEAL, b.b, b.n);
    ok = ok && surf_page() == 8 && sim_wire_last(EMP_OP_SCREEN) == 0
       && last_request_reason() == EMP_REQ_REVISION_UNKNOWN;

    b_init(&b); b64(&b, 42); b16(&b, 7);
    sim_wire_reset();
    surf_handle(EMP_OP_REVEAL, b.b, b.n);
    ok = ok && surf_page() == 8 && sim_wire_last(EMP_OP_SCREEN) == 0;

    sim_wire_reset();
    surf_handle(EMP_OP_CLEAR, 0, 0);
    screen = sim_wire_last(EMP_OP_SCREEN);
    ok = ok && surf_field_count() == 0 && surf_slot_span() == 0 && surf_page() == 0
       && ui_state()->mode == UI_MODE_SURFACE && ui_state()->focused < 0
       && screen && screen->len == 12 && screen->payload[8] == 0
       && screen->payload[10] == 0;
    check("reveal validates revision/id and clear confirms an empty screen", ok);
}

static void test_successive_descriptors_replace(void)
{
    load_good();

    /* Three in a row. Two arenas mean the storage alternates, so a swap that goes the wrong way,
     * or a scratch arena that is not cleared between transfers, shows up by the third. */
    for (unsigned round = 0; round < 3; round++) {
        xfer_t x;
        xfer_begin(&x, 20 + round, 1);
        xfer_number(&x, 0, 0, (round & 1) ? "Odd" : "Even", 0.5);
        xfer_end(&x, 0, 0);

        int ok = surf_field_count() == 1
              && surf_applied_revision() == 20 + round
              && surf_field(0)->id == 0
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
        xfer_number(&x, 0, 0, "A rather long field label", 0.5);
        xfer_number(&x, 1, 1, "Another long field label", 0.5);
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
    xfer_field(&x, 0, 0, "Skipped", 0.0, "Hz", tag, body, len);
    xfer_number(&x, 1, 1, "Intact", 0.5);
    xfer_end(&x, 0, 0);

    const surf_field_t *f = surf_field(0);
    int ok = surf_field_count() == 2
          && label_is(0, "Skipped")
          && label_is(1, "Intact")
          && f && f->id == 0
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

static void test_color_tag_is_native(void)
{
    static const uint8_t body[] = { 0x03,
                                    0, 0, 0x80, 0x3E,
                                    0, 0, 0x80, 0x3F,
                                    0, 0, 0, 0x3F };
    xfer_t x;
    surf_init(); sim_wire_reset();
    xfer_begin(&x, 30, 1);
    xfer_field(&x, 0, 0, "Colour", 0.0, 0, EMP_VAL_COLOR,
               body, (uint16_t)sizeof(body));
    xfer_end(&x, 0, 0);
    const surf_field_t *f = surf_field(0);
    int ok = f && f->kind == EMP_KIND_COLOR && f->value_tag == EMP_VAL_COLOR
          && f->color_count == 3u && f->color[0] == 0.25f
          && f->color[1] == 1.0f && f->color[2] == 0.5f;
    check("RGB colour is stored as one native field", ok);

    float rgba[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    sim_wire_reset();
    surf_set_color(0, rgba, 4u);
    const sim_wire_msg_t *edit = sim_wire_last(EMP_OP_EDIT);
    ok = edit && edit->len == 33u && edit->payload[15] == EMP_VAL_COLOR
      && edit->payload[16] == 4u && memcmp(edit->payload + 17, rgba, sizeof(rgba)) == 0;
    check("RGBA edit encoding carries every component", ok);
}

static void test_bad_colors_keep_live_descriptor(void)
{
    static const uint8_t bad_count[] = { 2u, 0,0,0,0, 0,0,0,0 };
    static const uint8_t nonfinite[] = { 3u, 0,0,0xC0,0x7F,
                                        0,0,0,0, 0,0,0,0 };
    load_good(); sim_wire_reset();
    xfer_t x;
    xfer_begin(&x, 31, 1);
    xfer_field(&x, 0, 0, "Bad count", 0.0, 0, EMP_VAL_COLOR,
               bad_count, (uint16_t)sizeof(bad_count));
    int ok = good_is_intact() && last_request_reason() == EMP_REQ_SEQUENCE_BROKEN;

    sim_wire_reset();
    xfer_begin(&x, 32, 1);
    xfer_field(&x, 0, 0, "Nonfinite", 0.0, 0, EMP_VAL_COLOR,
               nonfinite, (uint16_t)sizeof(nonfinite));
    ok = ok && good_is_intact() && last_request_reason() == EMP_REQ_SEQUENCE_BROKEN;
    check("malformed and non-finite colours preserve the live descriptor", ok);
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
    xfer_field(&x, 0, 0, "Mystery", 0.0, 0, 0x40u, (const uint8_t *)"", 0);

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
    b16(&b, 0);                                   /* id */
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
          && f && f->id == 0 && (f->present & EMP_PRESENT_DEFAULT) == 0);
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

    b16(&b, 0); b32(&b, 0);
    b8(&b, 0x82u); b16(&b, 3); b8(&b, 1); b8(&b, 2); b8(&b, 3);

    b16(&b, 1); b32(&b, 0);
    b8(&b, EMP_VAL_NUMBER); bf64(&b, 0.875);

    surf_handle(EMP_OP_VALUES, b.b, b.n);

    const surf_field_t *f = surf_field(1);
    check("an unreadable value costs one control, not the batch",
          f && f->number > 0.874 && f->number < 0.876);
}

/* ------------------------------------------------------------------ diagnostics */

/* Diagnostics accumulate and are sent by the tick, so a test that wants to see one has to run
 * the tick -- which is itself the property worth pinning: nothing is sent from inside a decoder,
 * because that would mean framing an outbound message part-way through an inbound one. */
static int diag_seen(uint16_t code, uint32_t *context_out, uint32_t *count_out)
{
    for (unsigned i = 0; i < sim_wire_count(); i++) {
        const sim_wire_msg_t *m = sim_wire_at(i);
        if (m->opcode != EMP_OP_DIAG || m->len < 13) continue;

        uint16_t got = (uint16_t)(m->payload[1] | (m->payload[2] << 8));
        if (got != code) continue;

        if (context_out) {
            *context_out = (uint32_t)m->payload[3] | ((uint32_t)m->payload[4] << 8)
                         | ((uint32_t)m->payload[5] << 16) | ((uint32_t)m->payload[6] << 24);
        }
        if (count_out) {
            *count_out = (uint32_t)m->payload[7] | ((uint32_t)m->payload[8] << 8)
                       | ((uint32_t)m->payload[9] << 16) | ((uint32_t)m->payload[10] << 24);
        }
        return 1;
    }
    return 0;
}

static void test_nothing_is_sent_before_the_tick(void)
{
    surf_init();
    emp_diag_reset();
    sim_wire_reset();

    xfer_t x;
    xfer_begin(&x, 30, 1);
    xfer_field(&x, 0, 0, "Mystery", 0.0, 0, 0x40u, (const uint8_t *)"", 0);

    int quiet_during = !diag_seen(EMP_DIAG_VALUE_UNDECODABLE, 0, 0);
    emp_diag_tick();
    uint32_t ctx = 0;
    int said_after = diag_seen(EMP_DIAG_VALUE_UNDECODABLE, &ctx, 0) && ctx == 0x40u;

    check("a diagnostic is raised inside the decoder and sent by the tick",
          quiet_during && said_after);
}

static void test_repeats_coalesce(void)
{
    surf_init();
    emp_diag_reset();
    sim_wire_reset();

    /* Five fields, each with a tag this device steps over. One message, count five -- because a
     * diagnostic that fires per glyph per repaint would otherwise flood the link it reports on. */
    static const uint8_t body[] = { 0x01, 0x00, 0x7A };
    xfer_t x;
    xfer_begin(&x, 31, 5);
    for (uint16_t i = 0; i < 5; i++) {
        xfer_field(&x, i, i, "Ext", 0.0, 0, 0x81u, body, (uint16_t)sizeof(body));
    }
    xfer_end(&x, 0, 0);
    emp_diag_tick();

    uint32_t count = 0;
    int one_message = 0;
    unsigned diags = 0;
    for (unsigned i = 0; i < sim_wire_count(); i++) {
        if (sim_wire_at(i)->opcode == EMP_OP_DIAG) diags++;
    }
    one_message = (diags == 1);

    check("repeated diagnostics coalesce into one message with a count",
          one_message && diag_seen(EMP_DIAG_UNKNOWN_VALUE_TAG, 0, &count) && count == 5
          && emp_diag_count(EMP_SEV_INFO) == 5);
}

static void test_non_finite_is_refused_on_decode(void)
{
    surf_init();
    emp_diag_reset();
    sim_wire_reset();

    /* An IEEE-754 quiet NaN, which a host can produce from a division it did not check. Left in
     * place it does not merely display wrongly: every comparison against it is false, so the bar
     * geometry, the clamp and the digit editor all take their else branch. */
    static const uint8_t nan_bits[] = { 0, 0, 0, 0, 0, 0, 0xF8, 0x7F };

    xfer_t x;
    xfer_begin(&x, 32, 1);
    xfer_field(&x, 0, 0, "Poisoned", 0.0, 0, EMP_VAL_NUMBER, nan_bits, 8);
    xfer_end(&x, 0, 0);
    emp_diag_tick();

    const surf_field_t *f = surf_field(0);
    check("a non-finite value is refused, and the field says so",
          surf_field_count() == 1 && label_is(0, "Poisoned")
          && f && f->number == 0.0
          && (f->flags & EMP_FIELD_TRUNCATED)
          && diag_seen(EMP_DIAG_NON_FINITE, 0, 0));
}

static void test_non_finite_is_refused_on_encode(void)
{
    load_good();
    emp_diag_reset();
    sim_wire_reset();

    /* Built by bit pattern rather than by 0.0/0.0, which some compilers fold at build time. */
    static const uint8_t inf_bits[] = { 0, 0, 0, 0, 0, 0, 0xF0, 0x7F };
    double inf;
    memcpy(&inf, inf_bits, 8);

    surf_set_number(0, inf);
    emp_diag_tick();

    /* Nothing may be sent, because round-trip equality is what the host's property tests rest
     * on -- and an infinity that survived the trip would break it for every value. */
    check("a non-finite edit is refused rather than encoded",
          sim_wire_last(EMP_OP_EDIT) == 0
          && surf_field(0)->number == 0.5
          && diag_seen(EMP_DIAG_NON_FINITE, 0, 0));
}

static void test_pool_exhaustion_is_reported(void)
{
    surf_init();
    emp_diag_reset();
    sim_wire_reset();

    /* Fill the pool with labels until it cannot take another. The field must still load -- a
     * knob with no name still turns -- but the device must say which one lost its label. */
    static char big[257];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;

    uint16_t n = (uint16_t)(SURF_STRING_POOL / 256u + 2u);
    xfer_t x;
    xfer_begin(&x, 33, n);
    for (uint16_t i = 0; i < n; i++) xfer_number(&x, i, i, big, 0.25);
    xfer_end(&x, 0, 0);
    emp_diag_tick();

    const surf_field_t *last = surf_field((uint16_t)(n - 1));
    check("a label dropped for want of pool is reported, and the field still loads",
          surf_field_count() == n
          && last && last->id == n - 1 && last->label_len == 0
          && (last->flags & EMP_FIELD_TRUNCATED)
          && diag_seen(EMP_DIAG_STRING_TRUNCATED, 0, 0));
}

/* ------------------------------------------------------------------ runner */

int desc_run_selftests(desc_report_fn report)
{
    failures  = 0;
    report_fn = report;

    test_round_trip();
    test_sparse_slots_preserve_holes_and_pages();
    test_duplicate_and_out_of_range_slots_keep_live();
    test_live_survives_transfer();
    test_crc_mismatch_keeps_live();
    test_sequence_break_keeps_live();
    test_truncated_field_keeps_live();
    test_abort_keeps_live();
    test_too_many_fields_keeps_live();
    test_stale_values_do_not_apply();
    test_values_apply_across_a_hole();
    test_reveal_and_clear_confirm_physical_screen();
    test_successive_descriptors_replace();
    test_pool_does_not_accumulate();

    test_extension_tag_is_skipped();
    test_text_tag_is_skipped();
    test_color_tag_is_native();
    test_bad_colors_keep_live_descriptor();
    test_reserved_tag_abandons_the_record();
    test_extension_default_is_dropped_not_fatal();
    test_values_skip_one_control_not_the_batch();

    test_nothing_is_sent_before_the_tick();
    test_repeats_coalesce();
    test_non_finite_is_refused_on_decode();
    test_non_finite_is_refused_on_encode();
    test_pool_exhaustion_is_reported();

    report_fn = 0;
    return failures;
}
