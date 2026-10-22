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
#include <math.h>

#include "emp.h"
#include "frame.h"
#include "surface.h"
#include "ui_state.h"
#include "sim_wire.h"
#include "diag.h"
#include "theme.h"

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

static void test_small_pages_use_bottom_row(void)
{
    int ok = 1;
    for (uint16_t count = 1; count <= 8; count++) {
        xfer_t x;
        surf_init(); ui_state_init(); sim_wire_reset();
        xfer_begin(&x, 100 + count, count);
        for (uint16_t i = 0; i < count; i++) xfer_number(&x, i, i, "Ordered", 0.25);
        xfer_end(&x, 0, 0);
        unsigned first = count <= 4 ? 4 : 0;
        for (unsigned knob = 0; knob < 8; knob++) {
            int32_t expected = knob >= first && knob < first + count ? (int32_t)(knob - first) : -1;
            ok = ok && ui_state_field_for(knob) == expected;
        }
        if (count <= 4) {
            ok = ok && !ui_state_rotate(0, 4, 10) && surf_field(0)->number == 0.25;
            ui_state_push(4, 1, 11); ui_state_push(4, 0, 12);
            ok = ok && ui_state()->focused == 0 && ui_state()->digit_top
                    && ui_digit_row_first() == 0 && ui_cell_row_first() == 4;
            const sim_wire_msg_t *focus = sim_wire_last(EMP_OP_FOCUS);
            ok = ok && focus && focus->payload[9] == 0; /* ID remains zero, not physical knob 4. */
            ui_state_push(0, 1, 13); ui_state_push(0, 0, 14);
            ok = ok && ui_state()->focused < 0;
            (void)ui_state_rotate(4, 4, 15);
            const sim_wire_msg_t *edit = sim_wire_last(EMP_OP_EDIT);
            ok = ok && surf_field(0)->number != 0.25 && edit && edit->payload[12] == 0;
        }
    }
    check("1-4 controls use lower row; 5-8 use reading order; input IDs stay stable", ok);
}

static void test_sparse_small_pages_keep_order_and_ids(void)
{
    xfer_t x;
    surf_init(); ui_state_init(); sim_wire_reset();
    xfer_begin(&x, 110, 4);
    const uint16_t ids[] = { 1, 3, 7, 9 };
    for (uint16_t i = 0; i < 4; i++) xfer_number(&x, i, ids[i], "Sparse", 0.25);
    xfer_end(&x, 0, 0);
    int ok = ui_state_field_for(4) == 1 && ui_state_field_for(5) == 3
          && ui_state_field_for(6) == 7 && ui_state_field_for(7) < 0;
    ok = ok && ui_state_reveal(9) && ui_state_field_for(4) == 9 && ui_state_field_for(5) < 0;
    const sim_wire_msg_t *screen = sim_wire_last(EMP_OP_SCREEN);
    ok = ok && screen && screen->payload[8] == 8 && screen->payload[10] == 2;
    check("sparse short pages pack left-to-right without changing IDs or page reports", ok);
}

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

static void test_highlight_validation(void)
{
    surf_init(); ui_state_init(); xfer_t x; xfer_begin(&x,42,1);
    xfer_number(&x,0,0,"Gain",0.5); xfer_end(&x,0,0); ui_state_tick(100);
    buf_t b; b_init(&b); b64(&b,42); b8(&b,1); b16(&b,0); b16(&b,1500);
    surf_handle(EMP_OP_HIGHLIGHT,b.b,b.n);
    int ok=ui_state()->remote_highlights==1 && ui_state()->focused==-1 && ui_state()->touch_mask==0;
    ui_state_highlight(-1,0); b.b[0]=41; surf_handle(EMP_OP_HIGHLIGHT,b.b,b.n);
    ok=ok && ui_state()->remote_highlights==0;
    b.b[0]=42; surf_handle(EMP_OP_HIGHLIGHT,b.b,b.n-1);
    ok=ok && ui_state()->remote_highlights==0;
    b.b[8]=2; surf_handle(EMP_OP_HIGHLIGHT,b.b,b.n);
    ok=ok && ui_state()->remote_highlights==0;
    b.b[8]=1; b.b[9]=7; surf_handle(EMP_OP_HIGHLIGHT,b.b,b.n);
    ok=ok && ui_state()->remote_highlights==0;
    b.b[9]=0; surf_handle(EMP_OP_HIGHLIGHT,b.b,b.n); b.b[8]=0;
    surf_handle(EMP_OP_HIGHLIGHT,b.b,b.n); ok=ok && ui_state()->remote_highlights==0;
    check("highlight validates revision, length, flag and occupied slot; clear is immediate",ok);
    load_sparse();
    b_init(&b); b64(&b,42); b8(&b,2); b16(&b,0); b16(&b,8); b16(&b,1500);
    surf_handle(EMP_OP_HIGHLIGHTS,b.b,b.n);
    ok=ui_state()->remote_highlights==(UINT64_C(1)|UINT64_C(1)<<8);
    b.b[0]=41;surf_handle(EMP_OP_HIGHLIGHTS,b.b,b.n);
    ok=ok && ui_state()->remote_highlights==257;
    b.b[0]=42;surf_handle(EMP_OP_HIGHLIGHTS,b.b,b.n-1);
    ok=ok && ui_state()->remote_highlights==257;
    b.b[11]=0;surf_handle(EMP_OP_HIGHLIGHTS,b.b,b.n); /* duplicate id */
    ok=ok && ui_state()->remote_highlights==257;
    b.b[11]=7;surf_handle(EMP_OP_HIGHLIGHTS,b.b,b.n); /* unoccupied id */
    ok=ok && ui_state()->remote_highlights==257;
    b.b[11]=64;surf_handle(EMP_OP_HIGHLIGHTS,b.b,b.n);
    ok=ok && ui_state()->remote_highlights==257;
    b.b[8]=65;surf_handle(EMP_OP_HIGHLIGHTS,b.b,b.n);
    ok=ok && ui_state()->remote_highlights==257;
    b_init(&b);b64(&b,42);b8(&b,0);b16(&b,0);surf_handle(EMP_OP_HIGHLIGHTS,b.b,b.n);
    ok=ok && ui_state()->remote_highlights==0;
    check("group highlight is atomic; stale, malformed, duplicate, empty and out-of-range ids cannot partially apply",ok);
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
          && f->value_tag == (tag == EMP_VAL_TEXT ? EMP_VAL_TEXT : EMP_VAL_NUMBER)
          && (tag != EMP_VAL_TEXT || strcmp(f->text, "abc") == 0);

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
    check_skippable("a Text value is retained without shifting the next field", EMP_VAL_TEXT,
                    body, (uint16_t)sizeof(body));
}

static void test_action_press_only(void)
{
    surf_init(); surf_demo_descriptor();
    surf_field_t *field = (surf_field_t *)surf_field(0);
    field->kind = EMP_KIND_ACTION;
    field->present = 0;
    ui_state_surface_changed();
    sim_wire_reset();
    ui_state_rotate(0, 10, 1);
    check("action ignores rotation", sim_wire_last(EMP_OP_ACTIVATE) == 0);
    ui_state_push(0, 1, 2);
    const sim_wire_msg_t *m = sim_wire_last(EMP_OP_ACTIVATE);
    check("action press emits revision/sequence/id", m && m->len == 14u);
    unsigned before = sim_wire_count();
    ui_state_push(0, 1, 3);
    ui_state_push(0, 0, 4);
    check("action held/released never repeats", sim_wire_count() == before);
    ui_state_push(0, 1, 5);
    check("next physical press executes again", sim_wire_count() == before + 1u);
    ui_state_push(0, 0, 6);
}

static void test_operator_keeps_action_row(void)
{
    surf_init();
    surf_demo_operator(); ui_state_init(); ui_state_surface_changed();
    check("operator layout opted in", surf_operator_layout());
    ui_state_push(4, 1, 10); ui_state_push(4, 0, 11);
    check("operator setting never drills", ui_state()->focused < 0);
    sim_wire_reset();
    ui_state_push(3, 1, 12); ui_state_push(3, 0, 13); /* physical top-right */
    const sim_wire_msg_t *m = sim_wire_last(EMP_OP_ACTIVATE);
    check("operator stop retains its physical slot", m && m->len == 14 && m->payload[12] == 7);
    ui_state_push(7, 1, 14); ui_state_push(7, 0, 15); /* bottom-right More */
    check("operator More retains bottom-right knob", surf_page() == 8);
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


static int near(double a, double b) { return fabs(a - b) < 1e-9; }

/* Store and Reset mirror the screen's column: present only while a value differs from its
 * default. Store is optimistic and keeps the drill; DEFAULTS updates in place. */
static void defaults_msg(uint64_t revision, uint16_t id, int has, double value)
{
    buf_t b; b_init(&b);
    b64(&b, revision); b16(&b, 1); b16(&b, id); b8(&b, (uint8_t)(has != 0));
    if (has) { b8(&b, EMP_VAL_NUMBER); bf64(&b, value); }
    surf_handle(EMP_OP_DEFAULTS, b.b, b.n);
}

static void test_defaults_store_and_reset(void)
{
    load_sparse();                                  /* revision 42, field 1 holds 0.1 */
    int ok = !surf_field_modified(surf_field(1));   /* no default declared */

    defaults_msg(42, 1, 1, 0.5);
    ok = ok && surf_field_modified(surf_field(1));
    defaults_msg(41, 1, 1, 0.1);                    /* stale revision: refused */
    ok = ok && surf_field_modified(surf_field(1));
    defaults_msg(42, 1, 1, 0.1 * (1.0 + 1e-8));     /* an f32 round trip is not a change */
    ok = ok && !surf_field_modified(surf_field(1));
    defaults_msg(42, 1, 1, 0.5);

    unsigned knob = (unsigned)ui_state_knob_for(1);
    ui_state_push(knob, 1, 1000); ui_state_push(knob, 0, 1005);
    ok = ok && ui_state()->focused == 1
            && ui_state_button_enabled(UI_BTN_STORE)
            && ui_state_button_enabled(UI_BTN_RESET);

    (void)ui_state_rotate(ui_digit_row_first() + 3u, UI_DIGIT_STEP, 1100);
    double edited = surf_field(1)->number;
    sim_wire_reset();
    ui_state_button(UI_BTN_STORE, 1, 1200);
    ui_state_button(UI_BTN_STORE, 0, 1210);
    const sim_wire_msg_t *m = sim_wire_last(EMP_OP_SAVE_DEFAULT);
    ok = ok && m && m->len == 10 && m->payload[0] == 42 && m->payload[8] == 1 && m->payload[9] == 0;
    ok = ok && ui_state()->focused == 1                         /* still drilled */
            && !surf_field_modified(surf_field(1))
            && near(surf_field(1)->default_number, edited)
            && !ui_state_button_enabled(UI_BTN_STORE)
            && !ui_state_button_enabled(UI_BTN_RESET)
            && ui_state_button_enabled(UI_BTN_UNDO);             /* history kept */

    /* The host's confirmation is a DEFAULTS message, not a descriptor: still drilled. */
    defaults_msg(42, 1, 1, edited);
    ok = ok && ui_state()->focused == 1 && ui_state_button_enabled(UI_BTN_UNDO);

    /* Reset returns to the stored default. */
    ui_state_button(UI_BTN_UNDO, 1, 1300); ui_state_button(UI_BTN_UNDO, 0, 1310);
    ok = ok && surf_field_modified(surf_field(1)) && ui_state_button_enabled(UI_BTN_RESET);
    ui_state_button(UI_BTN_RESET, 1, 1400); ui_state_button(UI_BTN_RESET, 0, 1410);
    ok = ok && near(surf_field(1)->number, edited) && !surf_field_modified(surf_field(1));

    /* A cleared default removes both. */
    defaults_msg(42, 1, 0, 0.0);
    ok = ok && !(surf_field(1)->present & EMP_PRESENT_DEFAULT) && !ui_state_button_enabled(UI_BTN_RESET);
    check("defaults update in place; store and reset follow the modified state", ok);
}

/* ------------------------------------------------------------------ EMP/1.6 style and tone
 *
 * Laid out by hand from docs/protocol.md "EMP/1.6", like everything above: the trailing
 * `style` u8 + `tone` u8 sit after the default (none here) and after `path` when present, and
 * `field_len` counts every byte after itself, as the host encoder computes it. */

typedef struct {
    uint16_t    id;
    uint8_t     kind;
    const char *label;
    const char *text;         /* Text value when set, else Number `number` */
    double      number;
    int         styled;       /* set EMP_PRESENT_STYLE and append style + tone */
    uint8_t     style, tone;
    const char *path;         /* EMP_PRESENT_PATH, stepped over by the device */
    int         cut;          /* declare the style but leave its bytes off: a truncated record */
    uint8_t     flags;        /* EMP/1.8 JOINED / DISABLED, beside READONLY */
} sfield_t;

static void styled_record(buf_t *b, uint16_t index, const sfield_t *s)
{
    b_init(b);
    b16(b, index);
    uint32_t len_at = b->n;
    b16(b, 0);                                    /* field_len, patched below */
    b16(b, s->id);
    b8(b, s->kind);
    b8(b, (uint8_t)((s->kind == EMP_KIND_READONLY ? EMP_FIELD_READONLY : 0u) | s->flags));
    uint16_t present = 0;
    if (s->styled) present |= EMP_PRESENT_STYLE;
    if (s->path)   present |= EMP_PRESENT_PATH;
    b16(b, present);
    bf64(b, 0.0); bf64(b, 0.0); bf64(b, 0.0);    /* min, max, step */
    b8(b, 0); b8(b, 0); b16(b, 0);                /* precision, lane, choice_count */
    if (s->text) { b8(b, EMP_VAL_TEXT); bstr(b, s->text); }
    else         { b8(b, EMP_VAL_NUMBER); bf64(b, s->number); }
    bstr(b, s->label);
    if (s->path) bstr(b, s->path);
    if (s->styled && !s->cut) { b8(b, s->style); b8(b, s->tone); }
    uint16_t field_len = (uint16_t)(b->n - len_at - 2u);
    b->b[len_at] = (uint8_t)field_len; b->b[len_at + 1] = (uint8_t)(field_len >> 8);
}

static void xfer_styled(xfer_t *x, uint16_t index, const sfield_t *s)
{
    buf_t b;
    styled_record(&b, index, s);
    x->crc = emp_crc32c_update(x->crc, b.b + 2, b.n - 2);
    x->count++;
    surf_handle(EMP_OP_DESC_FIELD, b.b, b.n);
}

static void load_styled(uint64_t revision, const sfield_t *fields, uint16_t n)
{
    xfer_t x;
    surf_init(); ui_state_init(); emp_diag_reset(); sim_wire_reset();
    xfer_begin(&x, revision, n);
    for (uint16_t i = 0; i < n; i++) xfer_styled(&x, i, &fields[i]);
    xfer_end(&x, 0, 0);
}

/* The status page the av-fleet daemon is expected to send: a stop top-left, a status readout. */
static const sfield_t status_page[] = {
    { 0, EMP_KIND_ACTION,   "E-STOP",       0,       0.0, 1, EMP_STYLE_ESTOP,  EMP_TONE_NEUTRAL, 0, 0 },
    { 1, EMP_KIND_READONLY, "Orchestrator", "ARMED", 0.0, 1, EMP_STYLE_NORMAL, EMP_TONE_OK,      0, 0 },
    { 2, EMP_KIND_READONLY, "Nodes",        0,       2.0, 1, EMP_STYLE_NORMAL, EMP_TONE_WARN,    0, 0 },
    { 3, EMP_KIND_NUMBER,   "Brightness",   0,       0.5, 0, 0, 0,                               0, 0 },
    { 4, EMP_KIND_ACTION,   "Identify",     0,       0.0, 1, EMP_STYLE_NORMAL, EMP_TONE_NEUTRAL, 0, 0 },
    { 5, EMP_KIND_READONLY, "Last stop",    "none",  0.0, 1, EMP_STYLE_NORMAL, EMP_TONE_ALERT,
         "/fleet/last_stop", 0 },
};
#define STATUS_FIELDS ((uint16_t)(sizeof(status_page) / sizeof(status_page[0])))

static void test_style_and_tone_decode(void)
{
    load_styled(160, status_page, STATUS_FIELDS);
    const surf_field_t *stop = surf_field(0), *armed = surf_field(1), *nodes = surf_field(2);
    const surf_field_t *plain = surf_field(3), *action = surf_field(4), *last = surf_field(5);
    int ok = surf_applied_revision() == 160 && surf_field_count() == STATUS_FIELDS;
    ok = ok && stop && stop->style == EMP_STYLE_ESTOP && surf_field_is_estop(stop)
            && !(stop->flags & EMP_FIELD_TRUNCATED) && label_is(0, "E-STOP");
    ok = ok && armed && armed->tone == EMP_TONE_OK && strcmp(armed->text, "ARMED") == 0;
    ok = ok && nodes && nodes->tone == EMP_TONE_WARN && nodes->number == 2.0;
    ok = ok && plain && plain->style == 0 && plain->tone == 0 && !surf_field_is_estop(plain);
    ok = ok && action && !surf_field_is_estop(action);
    /* `path` is stepped over and the style after it still lands; the label is not the path. */
    ok = ok && last && last->tone == EMP_TONE_ALERT && label_is(5, "Last stop")
            && !(last->flags & EMP_FIELD_TRUNCATED);
    check("1.6 style and tone decode, after path when present", ok);
}

static void test_style_validation(void)
{
    static const sfield_t bad[] = {
        { 0, EMP_KIND_ACTION,   "Odd style",   0, 0.0, 1, 3u,              0,  0, 0 },
        { 1, EMP_KIND_NUMBER,   "Not an action",0, 0.5, 1, EMP_STYLE_ESTOP, 0,  0, 0 },
        { 2, EMP_KIND_READONLY, "Odd tone",    "x", 0.0, 1, 0,              9u, 0, 0 },
    };
    load_styled(161, bad, 3);
    emp_diag_tick();
    uint32_t ctx = 0, count = 0;
    int ok = surf_applied_revision() == 161;
    ok = ok && surf_field(0)->style == EMP_STYLE_NORMAL && (surf_field(0)->flags & EMP_FIELD_TRUNCATED);
    ok = ok && surf_field(1)->style == EMP_STYLE_NORMAL && !surf_field_is_estop(surf_field(1))
            && (surf_field(1)->flags & EMP_FIELD_TRUNCATED);
    ok = ok && surf_field(2)->tone == EMP_TONE_NEUTRAL && (surf_field(2)->flags & EMP_FIELD_TRUNCATED);
    ok = ok && diag_seen(EMP_DIAG_STYLE_UNSUPPORTED, &ctx, &count) && ctx == ((0u << 8) | 3u)
            && count == 3;
    check("unknown style/tone and estop on a non-Action degrade with a diagnostic", ok);

    /* A declared style with its bytes missing is a truncated record: refused, live kept. */
    load_styled(162, status_page, STATUS_FIELDS);
    sfield_t cut = status_page[0];
    cut.cut = 1;
    xfer_t x;
    sim_wire_reset();
    xfer_begin(&x, 163, 1);
    xfer_styled(&x, 0, &cut);
    xfer_end(&x, 0, 0);
    check("a style member cut short refuses the descriptor and keeps the live one",
          surf_applied_revision() == 162 && last_request_reason() == EMP_REQ_SEQUENCE_BROKEN
          && surf_field_is_estop(surf_field(0)));
}

static void toned_text(buf_t *b, uint16_t id, uint32_t ack, uint8_t tone, const char *text)
{
    b16(b, id); b32(b, ack); b8(b, tone); b8(b, EMP_VAL_TEXT); bstr(b, text);
}

static void test_toned_values(void)
{
    load_styled(170, status_page, STATUS_FIELDS);
    buf_t b;

    /* Text and tone change together, in one entry. */
    b_init(&b); b64(&b, 170); b16(&b, 2);
    toned_text(&b, 1, 0, EMP_TONE_ALERT, "STOPPED");
    b16(&b, 2); b32(&b, 0); b8(&b, EMP_TONE_OK); b8(&b, EMP_VAL_NUMBER); bf64(&b, 3.0);
    surf_handle(EMP_OP_TONED_VALUES, b.b, b.n);
    int ok = strcmp(surf_field(1)->text, "STOPPED") == 0 && surf_field(1)->tone == EMP_TONE_ALERT
          && surf_field(2)->number == 3.0 && surf_field(2)->tone == EMP_TONE_OK;
    check("TONED_VALUES sets value and tone together", ok);

    /* Plain VALUES never touches a tone. */
    b_init(&b); b64(&b, 170); b16(&b, 1);
    b16(&b, 1); b32(&b, 0); b8(&b, EMP_VAL_TEXT); bstr(&b, "ARMED");
    surf_handle(EMP_OP_VALUES, b.b, b.n);
    check("VALUES leaves the tone where it was",
          strcmp(surf_field(1)->text, "ARMED") == 0 && surf_field(1)->tone == EMP_TONE_ALERT);

    /* Revision gate, as VALUES. */
    sim_wire_reset();
    b_init(&b); b64(&b, 999); b16(&b, 1);
    toned_text(&b, 1, 0, EMP_TONE_OK, "LIVE");
    surf_handle(EMP_OP_TONED_VALUES, b.b, b.n);
    check("TONED_VALUES at an unknown revision is refused with DESC_REQUEST",
          strcmp(surf_field(1)->text, "ARMED") == 0 && surf_field(1)->tone == EMP_TONE_ALERT
          && last_request_reason() == EMP_REQ_REVISION_UNKNOWN);

    /* An unknown tone: value applied, tone neutral, reported. An extension value: neither
     * changes, and the batch goes on. An unknown id: skipped. */
    emp_diag_reset(); sim_wire_reset();
    static const uint8_t ext[] = { 0x02, 0x00, 0xAA, 0xBB };
    b_init(&b); b64(&b, 170); b16(&b, 4);
    toned_text(&b, 1, 0, 7u, "ODD");
    b16(&b, 2); b32(&b, 0); b8(&b, EMP_TONE_ALERT); b8(&b, 0x81u);
    for (unsigned i = 0; i < sizeof(ext); i++) b8(&b, ext[i]);
    toned_text(&b, 40, 0, EMP_TONE_OK, "nobody");
    toned_text(&b, 5, 0, EMP_TONE_WARN, "control surface");
    surf_handle(EMP_OP_TONED_VALUES, b.b, b.n);
    emp_diag_tick();
    uint32_t ctx = 0;
    ok = strcmp(surf_field(1)->text, "ODD") == 0 && surf_field(1)->tone == EMP_TONE_NEUTRAL
      && surf_field(2)->number == 3.0 && surf_field(2)->tone == EMP_TONE_OK
      && strcmp(surf_field(5)->text, "control surface") == 0 && surf_field(5)->tone == EMP_TONE_WARN
      && diag_seen(EMP_DIAG_STYLE_UNSUPPORTED, &ctx, 0) && ctx == ((1u << 8) | 7u);
    check("TONED_VALUES: unknown tone neutral, skipped value keeps tone, batch continues", ok);
}

/* Byte identity with 1.5: a peer that negotiated less than 6 never set the bit and never sends
 * the opcode, but if it did this device must behave exactly as a 1.5 device would. */
static void test_older_minor_is_unchanged(void)
{
    sim_wire_set_peer_minor(5);
    load_styled(180, status_page, STATUS_FIELDS);
    const sim_wire_msg_t *ack = sim_wire_last(EMP_OP_DESC_ACK);
    int ok = ack && surf_applied_revision() == 180;
    for (uint16_t i = 0; i < STATUS_FIELDS; i++) {
        const surf_field_t *f = surf_field(i);
        ok = ok && f && f->style == 0 && f->tone == 0 && !(f->present & EMP_PRESENT_STYLE)
                && !(f->flags & EMP_FIELD_TRUNCATED);
    }
    ok = ok && !surf_field_is_estop(surf_field(0)) && label_is(1, "Orchestrator")
            && strcmp(surf_field(1)->text, "ARMED") == 0;

    buf_t b; b_init(&b); b64(&b, 180); b16(&b, 1);
    toned_text(&b, 1, 0, EMP_TONE_ALERT, "STOPPED");
    sim_wire_reset();
    surf_handle(EMP_OP_TONED_VALUES, b.b, b.n);
    ok = ok && strcmp(surf_field(1)->text, "ARMED") == 0 && sim_wire_count() == 0;
    check("minor 5 peer: style bit and TONED_VALUES ignored as by a 1.5 device", ok);

    /* The same records without the 1.6 members decode to the same fields. */
    surf_field_t with_style[STATUS_FIELDS];
    for (uint16_t i = 0; i < STATUS_FIELDS; i++) with_style[i] = *surf_field(i);
    sfield_t plain[STATUS_FIELDS];
    for (uint16_t i = 0; i < STATUS_FIELDS; i++) { plain[i] = status_page[i]; plain[i].styled = 0; }
    load_styled(180, plain, STATUS_FIELDS);
    ok = 1;
    for (uint16_t i = 0; i < STATUS_FIELDS; i++) {
        const surf_field_t *f = surf_field(i);
        ok = ok && f->kind == with_style[i].kind && f->flags == with_style[i].flags
                && f->present == with_style[i].present && f->number == with_style[i].number
                && f->label_len == with_style[i].label_len && strcmp(f->text, with_style[i].text) == 0;
    }
    check("minor 5 peer: a record with the 1.6 tail decodes as the one without", ok);

    /* ACTIVATE is the same 14 bytes whatever the style. */
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(181, status_page, STATUS_FIELDS);
    sim_wire_reset();
    ui_state_push(4, 1, 10); ui_state_push(4, 0, 11);      /* "Identify", a plain Action */
    const sim_wire_msg_t *a = sim_wire_last(EMP_OP_ACTIVATE);
    uint8_t plain_bytes[14] = {0};
    int have_plain = a && a->len == 14u;
    if (have_plain) memcpy(plain_bytes, a->payload, 14);
    ui_state_push(0, 1, 12); ui_state_push(0, 0, 13);      /* the stop */
    const sim_wire_msg_t *s = sim_wire_last(EMP_OP_ACTIVATE);
    uint32_t seq_plain = (uint32_t)plain_bytes[8] | ((uint32_t)plain_bytes[9] << 8)
                       | ((uint32_t)plain_bytes[10] << 16) | ((uint32_t)plain_bytes[11] << 24);
    uint32_t seq_stop = s ? ((uint32_t)s->payload[8] | ((uint32_t)s->payload[9] << 8)
                       | ((uint32_t)s->payload[10] << 16) | ((uint32_t)s->payload[11] << 24)) : 0;
    ok = have_plain && s && s->channel == EMP_CH_INPUT && s->len == 14u
      && memcmp(s->payload, plain_bytes, 8) == 0                   /* revision */
      && seq_stop == seq_plain + 1u
      && s->payload[12] == 0 && s->payload[13] == 0 && plain_bytes[12] == 4;
    check("an estop press is an ordinary 14-byte ACTIVATE", ok);
}

static void test_estop_press(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(190, status_page, STATUS_FIELDS);
    int ok = ui_state_field_for(0) == 0;                     /* top-left on a 6-field page */
    sim_wire_reset();
    ui_state_rotate(0, 20, 1);
    ok = ok && !sim_wire_last(EMP_OP_ACTIVATE) && !sim_wire_last(EMP_OP_EDIT);
    ui_state_push(0, 1, 2);
    ok = ok && sim_wire_last(EMP_OP_ACTIVATE) && ui_state()->focused < 0;
    unsigned before = sim_wire_count();
    ui_state_push(0, 1, 3); ui_state_push(0, 0, 4);          /* held, released: nothing more */
    ok = ok && sim_wire_count() == before;
    check("estop fires on press only; turning and holding do nothing", ok);

    /* Drill a bottom-row field: the top row becomes the digit editor and hides the stop. Its
     * knob still fires it, and leaves the drill so the stop is on screen. */
    ui_state_push(5, 1, 22); ui_state_push(5, 0, 23);        /* id 5, "Last stop", bottom row */
    ok = ui_state()->focused == 5 && ui_state()->digit_top;
    sim_wire_reset();
    ui_state_push(0, 1, 24);
    const sim_wire_msg_t *m = sim_wire_last(EMP_OP_ACTIVATE);
    ok = ok && m && m->payload[12] == 0 && ui_state()->focused < 0;
    ui_state_push(0, 0, 25);
    /* A plain Action keeps today's rule: under the digit row its press only exits the drill. */
    ui_state_push(1, 1, 26); ui_state_push(1, 0, 27);        /* id 1, top row: bottom row edits */
    ok = ok && ui_state()->focused == 1 && !ui_state()->digit_top;
    sim_wire_reset();
    ui_state_push(4, 1, 28); ui_state_push(4, 0, 29);        /* "Identify", plain Action */
    ok = ok && !sim_wire_last(EMP_OP_ACTIVATE) && ui_state()->focused < 0;
    check("estop fires from a hidden digit row and leaves the drill", ok);
}

static void test_top_left_slot(void)
{
    /* The answer the av-fleet daemon relies on: generic pages with 5..8 occupied slots put the
     * page's first ID on the top-left knob; 1..4 put nothing on the top row at all. */
    int ok = 1;
    for (uint16_t count = 2; count <= 8; count++) {
        sfield_t page[8];
        for (uint16_t i = 0; i < count; i++) {
            page[i] = (sfield_t){ i, EMP_KIND_READONLY, "Status", "ok", 0.0, 1, 0, EMP_TONE_OK, 0, 0 };
        }
        page[0] = status_page[0];
        load_styled(200 + count, page, count);
        ok = ok && ui_state_field_for(0) == (count >= 5 ? 0 : -1)
                && ui_state_field_for(4) == (count >= 5 ? 4 : 0)
                && ui_state_knob_for(0) == (count >= 5 ? 0 : 4);
    }
    check("estop at ID 0 is top-left with 5..8 fields, bottom-left with 2..4", ok);
}

static void test_presentation_colours(void)
{
    load_styled(210, status_page, STATUS_FIELDS);
    int ok = ui_tone_color(EMP_TONE_NEUTRAL) == 0 && ui_tone_color(EMP_TONE_OK) == C_OK
          && ui_tone_color(EMP_TONE_WARN) == C_WARN && ui_tone_color(EMP_TONE_ALERT) == C_DANGER
          && ui_tone_color(9u) == 0;
    ok = ok && ui_cell_background(surf_field(0)) == C_STOP
            && ui_cell_background(surf_field(4)) == C_CELL
            && ui_cell_background(surf_field(3)) == C_CELL;
    ok = ok && ui_value_color(surf_field(1), 0) == C_OK && ui_meter_color(surf_field(1)) == C_OK
            && ui_value_color(surf_field(2), 0) == C_WARN
            && ui_value_color(surf_field(5), 0) == C_DANGER;
    /* A tone on an editable number is not drawn: it would read as a range warning. */
    ((surf_field_t *)surf_field(3))->tone = EMP_TONE_ALERT;
    ok = ok && ui_value_color(surf_field(3), 0) == C_VALUE && ui_value_color(surf_field(3), 1) == C_ACCENT
            && ui_meter_color(surf_field(3)) == C_ACCENT;
    ((surf_field_t *)surf_field(1))->tone = EMP_TONE_NEUTRAL;
    ok = ok && ui_value_color(surf_field(1), 1) == C_VALUE && ui_meter_color(surf_field(1)) == C_TITLE;
    check("estop plate is C_STOP; tones pick ok/warn/alert ink on status fields only", ok);
}

/* ------------------------------------------------------------------ EMP/1.7 hold-to-fire
 *
 * The status page as the host sends it while the fleet is STOPPED: the stop stays at ID 0 and
 * ID 1 becomes "HOLD TO CLEAR", an Action with style 2. Six fields, so knob k shows ID k. */

static const sfield_t stopped_page[] = {
    { 0, EMP_KIND_ACTION,   "E-STOP",        0,         0.0, 1, EMP_STYLE_ESTOP,  0, 0, 0 },
    { 1, EMP_KIND_ACTION,   "HOLD TO CLEAR", 0,         0.0, 1, EMP_STYLE_HOLD,   0, 0, 0 },
    { 2, EMP_KIND_READONLY, "Orchestrator",  "STOPPED", 0.0, 1, EMP_STYLE_NORMAL, EMP_TONE_ALERT, 0, 0 },
    { 3, EMP_KIND_NUMBER,   "Brightness",    0,         0.5, 0, 0, 0, 0, 0 },
    { 4, EMP_KIND_ACTION,   "Identify",      0,         0.0, 1, EMP_STYLE_NORMAL, 0, 0, 0 },
    { 5, EMP_KIND_READONLY, "Last stop",     "surface", 0.0, 1, EMP_STYLE_NORMAL, 0, 0, 0 },
};
#define STOPPED_FIELDS ((uint16_t)(sizeof(stopped_page) / sizeof(stopped_page[0])))

static unsigned activations(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < sim_wire_count(); i++)
        if (sim_wire_at(i)->opcode == EMP_OP_ACTIVATE) n++;
    return n;
}

static uint32_t activation_seq(const sim_wire_msg_t *m)
{
    return (uint32_t)m->payload[8] | ((uint32_t)m->payload[9] << 8)
         | ((uint32_t)m->payload[10] << 16) | ((uint32_t)m->payload[11] << 24);
}

static void test_hold_decodes(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(300, stopped_page, STOPPED_FIELDS);
    emp_diag_tick();
    const surf_field_t *hold = surf_field(1);
    int ok = surf_applied_revision() == 300 && hold && hold->style == EMP_STYLE_HOLD
          && surf_field_is_hold(hold) && !surf_field_is_estop(hold)
          && !(hold->flags & EMP_FIELD_TRUNCATED) && label_is(1, "HOLD TO CLEAR")
          && surf_field_is_estop(surf_field(0)) && !surf_field_is_hold(surf_field(0))
          && !surf_field_is_hold(surf_field(4)) && !diag_seen(EMP_DIAG_STYLE_UNSUPPORTED, 0, 0)
          && ui_cell_background(hold) == C_CELL && ui_state_field_for(1) == 1;
    check("1.7 hold style decodes on an Action at minor 7", ok);

    /* HOLD on anything but an Action: drawn normal, flagged, reported, like ESTOP. */
    static const sfield_t bad[] = {
        { 0, EMP_KIND_NUMBER,   "Not an action", 0,   0.5, 1, EMP_STYLE_HOLD, 0, 0, 0 },
        { 1, EMP_KIND_READONLY, "Read only",     "x", 0.0, 1, EMP_STYLE_HOLD, 0, 0, 0 },
        { 2, EMP_KIND_ACTION,   "Unknown",       0,   0.0, 1, 3u,             0, 0, 0 },
    };
    load_styled(301, bad, 3);
    emp_diag_tick();
    uint32_t ctx = 0, count = 0;
    ok = surf_applied_revision() == 301;
    for (uint16_t i = 0; i < 3; i++)
        ok = ok && surf_field(i)->style == EMP_STYLE_NORMAL && !surf_field_is_hold(surf_field(i))
                && (surf_field(i)->flags & EMP_FIELD_TRUNCATED);
    ok = ok && diag_seen(EMP_DIAG_STYLE_UNSUPPORTED, &ctx, &count) && count == 3
            && ctx == ((0u << 8) | EMP_STYLE_HOLD);
    check("hold on a non-Action, or style 3, is normal with E_STYLE_UNSUPPORTED", ok);

    /* A minor 6 peer never sends style 2; if one did, this device answers as a 1.6 device: an
     * unknown style, so a plain Action that fires on the press edge. */
    sim_wire_set_peer_minor(6);
    load_styled(302, stopped_page, STOPPED_FIELDS);
    emp_diag_tick();
    ok = surf_field(1)->style == EMP_STYLE_NORMAL && !surf_field_is_hold(surf_field(1))
      && (surf_field(1)->flags & EMP_FIELD_TRUNCATED)
      && diag_seen(EMP_DIAG_STYLE_UNSUPPORTED, &ctx, 0) && ctx == ((1u << 8) | EMP_STYLE_HOLD)
      && surf_field_is_estop(surf_field(0));
    sim_wire_reset();
    ui_state_push(1, 1, 10);
    ok = ok && activations() == 1;
    ui_state_push(1, 0, 11);
    check("minor 6 peer: style 2 is unknown, drawn and fired as a plain Action", ok);
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
}

static void test_hold_fires_once(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(310, stopped_page, STOPPED_FIELDS);
    sim_wire_reset();
    const uint32_t t0 = 1000;
    ui_state_tick(t0);
    ui_state_push(1, 1, t0);
    int ok = activations() == 0 && ui_state()->hold_slot == 1 && !ui_state()->hold_done;
    ui_state_tick(t0 + 750);
    ok = ok && activations() == 0 && ui_state_hold_permille(1) == 500u
            && ui_state_hold_permille(0) == 0u;
    ui_state_tick(t0 + EMP_HOLD_MS - 1u);
    ok = ok && activations() == 0 && ui_state_hold_permille(1) < 1000u;
    ui_state_tick(t0 + EMP_HOLD_MS);
    const sim_wire_msg_t *m = sim_wire_last(EMP_OP_ACTIVATE);
    ok = ok && activations() == 1 && m && m->channel == EMP_CH_INPUT && m->len == 14u
            && m->payload[0] == 54 && m->payload[1] == 1 && m->payload[7] == 0   /* revision 310 */
            && m->payload[12] == 1 && m->payload[13] == 0                         /* field 1 */
            && ui_state()->hold_done && ui_state_hold_permille(1) == 1000u;
    uint32_t first_seq = m ? activation_seq(m) : 0;
    check("hold fires one 14-byte ACTIVATE at 1500 ms, not before", ok);

    /* Still held: nothing more, however long. Released: still nothing, and the face clears. */
    ui_state_tick(t0 + 3000); ui_state_tick(t0 + 60000);
    ok = activations() == 1 && ui_state()->hold_done;
    ui_state_push(1, 0, t0 + 60010);
    ui_state_tick(t0 + 60020);
    ok = ok && activations() == 1 && ui_state()->hold_slot < 0 && ui_state_hold_permille(1) == 0u;
    check("a fired hold never repeats while held, and release sends nothing", ok);

    /* A second activation needs a new press and a whole new hold. */
    const uint32_t t1 = t0 + 70000;
    ui_state_push(1, 1, t1);
    ui_state_tick(t1 + 1000);
    ok = activations() == 1;
    ui_state_tick(t1 + EMP_HOLD_MS);
    m = sim_wire_last(EMP_OP_ACTIVATE);
    ok = ok && activations() == 2 && m && activation_seq(m) == first_seq + 1u;
    ui_state_push(1, 0, t1 + EMP_HOLD_MS + 5);
    check("another activation takes release and a new full hold", ok);
}

static void test_hold_cancels(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(320, stopped_page, STOPPED_FIELDS);
    sim_wire_reset();

    /* Early release: silent, and the fill resets. */
    ui_state_push(1, 1, 100); ui_state_tick(100);
    ui_state_tick(100 + EMP_HOLD_MS - 50u);
    ui_state_push(1, 0, 100 + EMP_HOLD_MS - 40u);
    int ok = ui_state()->hold_slot < 0 && ui_state_hold_permille(1) == 0u;
    ui_state_tick(100 + EMP_HOLD_MS); ui_state_tick(5000);
    ok = ok && activations() == 0;
    check("releasing before 1500 ms never fires", ok);

    /* Turning the held knob cancels; holding on afterwards does not resume it. */
    ui_state_push(1, 1, 6000); ui_state_tick(6000);
    ui_state_tick(6500);
    ui_state_rotate(1, 1, 6600);
    ok = ui_state()->hold_slot < 0;
    ui_state_tick(6000 + EMP_HOLD_MS); ui_state_tick(20000);
    ok = ok && activations() == 0 && !sim_wire_last(EMP_OP_EDIT);
    ui_state_push(1, 0, 20010);
    /* Turning a different knob does not. */
    ui_state_push(1, 1, 21000); ui_state_tick(21000);
    ui_state_rotate(3, 2, 21100);
    ui_state_tick(21000 + EMP_HOLD_MS);
    ok = ok && activations() == 1;
    ui_state_push(1, 0, 23000);
    check("turning the held knob cancels the hold; another knob does not", ok);

    /* A descriptor replaced mid-hold cancels it, even one that still has the same Action. */
    load_styled(321, stopped_page, STOPPED_FIELDS);
    sim_wire_reset();
    ui_state_push(1, 1, 30000); ui_state_tick(30000);
    ui_state_tick(30500);
    xfer_t x;
    xfer_begin(&x, 322, STOPPED_FIELDS);
    for (uint16_t i = 0; i < STOPPED_FIELDS; i++) xfer_styled(&x, i, &stopped_page[i]);
    xfer_end(&x, 0, 0);
    ok = surf_applied_revision() == 322 && ui_state()->hold_slot < 0;
    ui_state_tick(30000 + EMP_HOLD_MS); ui_state_tick(40000);
    ok = ok && activations() == 0;
    ui_state_push(1, 0, 40010);
    check("a descriptor replaced mid-hold cancels it", ok);

    /* Leaving for the device's own settings mid-hold cancels it too. */
    sim_wire_reset();
    ui_state_push(1, 1, 50000); ui_state_tick(50000);
    ui_state_button(UI_BTN_SYSTEM, 1, 50100); ui_state_button(UI_BTN_SYSTEM, 0, 50110);
    ok = ui_state()->mode == UI_MODE_SYSTEM;
    ui_state_tick(50200);
    ui_state_tick(50000 + EMP_HOLD_MS);
    ok = ok && activations() == 0 && ui_state()->hold_slot < 0;
    ui_state_push(1, 0, 52000);
    ui_state_button(UI_BTN_BACK, 1, 52100); ui_state_button(UI_BTN_BACK, 0, 52110);
    check("opening device settings mid-hold cancels it", ok);
}

static void test_hold_leaves_others_alone(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(330, stopped_page, STOPPED_FIELDS);
    sim_wire_reset();

    /* The stop still fires on its press edge, beside a HOLD. */
    ui_state_push(0, 1, 10);
    const sim_wire_msg_t *m = sim_wire_last(EMP_OP_ACTIVATE);
    int ok = activations() == 1 && m && m->payload[12] == 0;
    ui_state_push(0, 0, 11);
    /* A plain Action too. */
    ui_state_push(4, 1, 12);
    m = sim_wire_last(EMP_OP_ACTIVATE);
    ok = ok && activations() == 2 && m && m->payload[12] == 4;
    ui_state_push(4, 0, 13);
    ui_state_tick(5000);
    ok = ok && activations() == 2 && ui_state()->hold_slot < 0;
    check("estop and plain Action still fire on the press edge beside a HOLD", ok);

    /* The stop's drilled exception is unchanged, and HOLD has none: drill a bottom-row field
     * so the top row is the digit editor; the stop knob fires, the HOLD knob only exits. */
    ui_state_push(5, 1, 100); ui_state_push(5, 0, 101);
    ok = ui_state()->focused == 5 && ui_state()->digit_top;
    sim_wire_reset();
    ui_state_push(1, 1, 102);
    ok = ok && activations() == 0 && ui_state()->focused < 0 && ui_state()->hold_slot < 0;
    ui_state_tick(102 + EMP_HOLD_MS); ui_state_tick(9000);
    ok = ok && activations() == 0;
    ui_state_push(1, 0, 9001);
    ui_state_push(5, 1, 9100); ui_state_push(5, 0, 9101);
    ui_state_push(0, 1, 9102);
    m = sim_wire_last(EMP_OP_ACTIVATE);
    ok = ok && activations() == 1 && m && m->payload[12] == 0 && ui_state()->focused < 0;
    ui_state_push(0, 0, 9103);
    check("HOLD has no drilled-field exception; the stop keeps its own", ok);
}

/* ------------------------------------------------------------------ runner */

/* ------------------------------------------------------------------ EMP/1.8 panels and lists
 *
 * The daemon's system page on a 1.8 device: the stop, CLEAR E-STOP joined to it (a HOLD Action
 * whose Text value is the fleet status), ARM disabled, the System list, then two more fields so
 * the page keeps reading order (knob k shows ID k). */

static const sfield_t panel_page[] = {
    { 0, EMP_KIND_ACTION,   "E-STOP",       0, 0.0, 1, EMP_STYLE_ESTOP, 0, 0, 0, 0 },
    { 1, EMP_KIND_ACTION,   "CLEAR E-STOP", "E-STOPPED\ncontrol surface", 0.0, 1, EMP_STYLE_HOLD,
         EMP_TONE_WARN, 0, 0, EMP_FIELD_JOINED },
    { 2, EMP_KIND_ACTION,   "ARM",          "NOT READY\nno node", 0.0, 1, EMP_STYLE_HOLD,
         EMP_TONE_WARN, 0, 0, EMP_FIELD_DISABLED },
    { 3, EMP_KIND_READONLY, "System",       "Fleet\tE-STOPPED\nNodes\t2/2\nWatches\t1 on duty",
         0.0, 1, EMP_STYLE_LIST, 0, 0, 0, 0 },
    { 4, EMP_KIND_NUMBER,   "Volume",       0, 0.5, 0, 0, 0, 0, 0, 0 },
    { 5, EMP_KIND_ACTION,   "Identify",     0, 0.0, 0, 0, 0, 0, 0, EMP_FIELD_DISABLED },
};
#define PANEL_FIELDS ((uint16_t)(sizeof(panel_page) / sizeof(panel_page[0])))

static void test_panel_decodes(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(400, panel_page, PANEL_FIELDS);
    emp_diag_tick();
    int ok = surf_applied_revision() == 400 && EMP_VERSION_MINOR == 8u;
    ok = ok && surf_field_joined(surf_field(1)) && !surf_field_disabled(surf_field(1))
            && surf_field_is_hold(surf_field(1)) && strcmp(surf_field(1)->text, "E-STOPPED\ncontrol surface") == 0;
    ok = ok && surf_field_disabled(surf_field(2)) && surf_field_is_hold(surf_field(2))
            && !surf_field_joined(surf_field(2));
    ok = ok && surf_field_is_list(surf_field(3)) && !(surf_field(3)->flags & EMP_FIELD_TRUNCATED);
    ok = ok && surf_field_is_estop(surf_field(0)) && !surf_field_joined(surf_field(0))
            && surf_field_disabled(surf_field(5)) && !diag_seen(EMP_DIAG_STYLE_UNSUPPORTED, 0, 0);
    check("1.8 joined, disabled and list decode at minor 8", ok);

    /* LIST on anything but a ReadOnly, and DISABLED on anything but an Action: refused. */
    static const sfield_t bad[] = {
        { 0, EMP_KIND_ACTION,   "List action", 0,   0.0, 1, EMP_STYLE_LIST, 0, 0, 0, 0 },
        { 1, EMP_KIND_NUMBER,   "List number", 0,   0.5, 1, EMP_STYLE_LIST, 0, 0, 0, 0 },
        { 2, EMP_KIND_READONLY, "Inert text",  "x", 0.0, 0, 0, 0, 0, 0, EMP_FIELD_DISABLED },
        { 3, EMP_KIND_ACTION,   "Style 4",     0,   0.0, 1, 4u, 0, 0, 0, 0 },
    };
    load_styled(401, bad, 4);
    emp_diag_tick();
    uint32_t ctx = 0, count = 0;
    ok = surf_applied_revision() == 401;
    ok = ok && !surf_field_is_list(surf_field(0)) && surf_field(0)->style == EMP_STYLE_NORMAL
            && (surf_field(0)->flags & EMP_FIELD_TRUNCATED);
    ok = ok && !surf_field_is_list(surf_field(1)) && (surf_field(1)->flags & EMP_FIELD_TRUNCATED);
    ok = ok && !surf_field_disabled(surf_field(2)) && !(surf_field(2)->flags & EMP_FIELD_DISABLED);
    ok = ok && surf_field(3)->style == EMP_STYLE_NORMAL;
    ok = ok && diag_seen(EMP_DIAG_STYLE_UNSUPPORTED, &ctx, &count) && count == 3
            && ctx == ((0u << 8) | EMP_STYLE_LIST);
    check("list off a ReadOnly, disabled off an Action, style 4: refused", ok);

    /* A minor 7 peer never sends them; if one did, this device answers as a 1.7 device. */
    sim_wire_set_peer_minor(7);
    load_styled(402, panel_page, PANEL_FIELDS);
    emp_diag_tick();
    ok = !surf_field_joined(surf_field(1)) && !surf_field_disabled(surf_field(2))
      && surf_field_is_hold(surf_field(2)) && !surf_field_is_list(surf_field(3))
      && surf_field(3)->style == EMP_STYLE_NORMAL && (surf_field(3)->flags & EMP_FIELD_TRUNCATED)
      && diag_seen(EMP_DIAG_STYLE_UNSUPPORTED, &ctx, 0) && ctx == ((3u << 8) | EMP_STYLE_LIST);
    check("minor 7 peer: 1.8 flags cleared and LIST unknown", ok);
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
}

static void test_disabled_action_is_inert(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(410, panel_page, PANEL_FIELDS);
    ui_state_tick(100);
    sim_wire_reset();
    ui_state_push(2, 1, 100);                                  /* ARM, disabled HOLD */
    ui_state_tick(120);
    int ok = ui_state()->hold_slot < 0 && ui_state_hold_permille(2) == 0;
    ui_state_tick(2000);
    ok = ok && activations() == 0;
    ui_state_push(2, 0, 2010);
    ui_state_push(5, 1, 2020); ui_state_push(5, 0, 2030);      /* disabled plain Action */
    surf_activate(2); surf_activate(5);                        /* and asked for directly */
    ok = ok && activations() == 0 && ui_state()->focused < 0;
    check("a disabled Action neither holds nor fires", ok);

    /* The joined one is live: it holds and fires as EMP/1.7 says. */
    ui_state_push(1, 1, 3000);
    ui_state_tick(3000 + EMP_HOLD_MS + 20);
    const sim_wire_msg_t *m = sim_wire_last(EMP_OP_ACTIVATE);
    ok = activations() == 1 && m && m->payload[12] == 1;
    ui_state_push(1, 0, 4700);
    /* The stop is untouched by all of it: it fires on its press. */
    ui_state_push(0, 1, 4800); ui_state_push(0, 0, 4810);
    m = sim_wire_last(EMP_OP_ACTIVATE);
    ok = ok && activations() == 2 && m && m->payload[12] == 0;
    check("a joined HOLD fires after the hold; the stop on its press", ok);
}

static void list_values(uint64_t revision, const char *text)
{
    buf_t b;
    b_init(&b); b64(&b, revision); b16(&b, 1);
    toned_text(&b, 3, 0, EMP_TONE_NEUTRAL, text);
    surf_handle(EMP_OP_TONED_VALUES, b.b, b.n);
}

static void test_list_scrolls_locally(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(420, panel_page, PANEL_FIELDS);
    int ok = ui_list_count(surf_field(3)) == 3 && ui_state_list_top(3) == 0;
    sim_wire_reset();
    ui_state_rotate(3, UI_LIST_STEP - 1, 10);                  /* not yet a step */
    ok = ok && ui_state_list_top(3) == 0;
    ui_state_rotate(3, 1, 11);
    ok = ok && ui_state_list_top(3) == 1;
    ui_state_rotate(3, UI_LIST_STEP * 20, 12);                 /* clamps at the last entry */
    ok = ok && ui_state_list_top(3) == 2;
    ok = ok && sim_wire_count() == 0;                           /* nothing on the wire */
    check("a list scrolls by entries, clamped, and sends nothing", ok);

    /* A value update keeps the place, clamped to the new length. */
    list_values(420, "Fleet\tARMED\nNodes\t2/2\nWatches\t1 on duty\nLease\tnone\n");
    ok = ui_list_count(surf_field(3)) == 4 && ui_state_list_top(3) == 2;
    list_values(420, "Fleet\tARMED\nNodes\t1/2");
    ok = ok && ui_list_count(surf_field(3)) == 2 && ui_state_list_top(3) == 1;
    ui_state_rotate(3, -UI_LIST_STEP * 9, 13);
    ok = ok && ui_state_list_top(3) == 0;
    ui_state_rotate(3, UI_LIST_STEP, 14);
    ok = ok && ui_state_list_top(3) == 1;
    /* Its press goes back to the top and drills into nothing. */
    ui_state_push(3, 1, 20); ui_state_push(3, 0, 21);
    ok = ok && ui_state_list_top(3) == 0 && ui_state()->focused < 0;
    check("values keep the place; press returns to the top, never drills", ok);

    ui_state_rotate(3, UI_LIST_STEP, 30);
    load_styled(421, panel_page, PANEL_FIELDS);
    check("a descriptor replacement starts the list at the top", ui_state_list_top(3) == 0);
}

static void test_long_text_is_kept(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    load_styled(430, panel_page, PANEL_FIELDS);
    char text[241];
    for (unsigned i = 0; i < 240u; i++) text[i] = (char)('a' + i % 26u);
    text[240] = 0;
    list_values(430, text);
    int ok = strlen(surf_field(3)->text) == 240u && memcmp(surf_field(3)->text, text, 240u) == 0;
    char longer[301];
    for (unsigned i = 0; i < 300u; i++) longer[i] = 'z';
    longer[300] = 0;
    list_values(430, longer);
    ok = ok && strlen(surf_field(3)->text) == SURF_TEXT_MAX - 1u;
    check("text values keep up to 255 bytes, then cut", ok);
}

/* EMP/1.8 PINNED: the fleet's two slots reserved above an application's page. IDs 0 and 1 are
 * pinned; the application has 2..7 and 10..11, and a field at 8 (under a pinned knob on the second
 * page) that must never be shown. */
static void test_pinned_slots(void)
{
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    static const sfield_t page[] = {
        { 0,  EMP_KIND_ACTION,   "E-STOP",       0, 0.0, 1, EMP_STYLE_ESTOP, 0, 0, 0, EMP_FIELD_PINNED },
        { 1,  EMP_KIND_ACTION,   "CLEAR E-STOP", "DISARMED\nlasers dark", 0.0, 1, EMP_STYLE_HOLD,
              EMP_TONE_OK, 0, 0, EMP_FIELD_PINNED | EMP_FIELD_JOINED },
        { 2,  EMP_KIND_NUMBER,   "Cutoff",       0, 0.5, 0, 0, 0, 0, 0, 0 },
        { 3,  EMP_KIND_NUMBER,   "Resonance",    0, 0.5, 0, 0, 0, 0, 0, 0 },
        { 8,  EMP_KIND_NUMBER,   "Hidden",       0, 0.5, 0, 0, 0, 0, 0, 0 },
        { 10, EMP_KIND_NUMBER,   "Rate",         0, 0.5, 0, 0, 0, 0, 0, 0 },
        { 11, EMP_KIND_NUMBER,   "Pinned no",    0, 0.5, 0, 0, 0, 0, 0, EMP_FIELD_PINNED },
    };
    load_styled(440, page, 7);
    int ok = surf_pinned_mask() == 0x03u
          && !(surf_field(11)->flags & EMP_FIELD_PINNED) && (surf_field(11)->flags & EMP_FIELD_TRUNCATED);
    /* Page 0: reading order although only four slots are occupied (no bottom-row packing). */
    ok = ok && ui_state_field_for(0) == 0 && ui_state_field_for(1) == 1 && ui_state_field_for(2) == 2
            && ui_state_field_for(3) == 3 && ui_state_field_for(4) < 0;
    check("pinned slots keep reading order and refuse a pin outside the top row", ok);

    /* Page 1: the pinned fields stay on knobs 0 and 1; ID 8 under them is never shown. */
    ui_state_tick(100);
    ui_state_button(UI_BTN_PAGE_NEXT, 1, 100); ui_state_button(UI_BTN_PAGE_NEXT, 0, 110);
    ok = ui_state()->page == 8 && ui_state_field_for(0) == 0 && ui_state_field_for(1) == 1
      && ui_state_field_for(2) == 10 && ui_state_field_for(3) == 11 && ui_state_knob_for(8) < 0;
    for (unsigned k = 0; k < 8; k++) ok = ok && ui_state_field_for(k) != 8;
    check("a page change keeps the pinned knobs and hides what is under them", ok);

    /* The stop fires from page 1 on its press; a hold on the clear started on page 0 survives a
     * page change, because its knob still addresses the same field. */
    load_styled(441, page, 7);
    ui_state_tick(200);
    sim_wire_reset();
    ui_state_button(UI_BTN_PAGE_NEXT, 1, 200); ui_state_button(UI_BTN_PAGE_NEXT, 0, 210);
    ui_state_push(0, 1, 220); ui_state_push(0, 0, 230);
    const sim_wire_msg_t *m = sim_wire_last(EMP_OP_ACTIVATE);
    ok = m && m->payload[12] == 0 && m->payload[13] == 0;
    ui_state_button(UI_BTN_BACK, 1, 300); ui_state_button(UI_BTN_BACK, 0, 310);
    ui_state_push(1, 1, 320);
    ui_state_button(UI_BTN_PAGE_NEXT, 1, 400); ui_state_button(UI_BTN_PAGE_NEXT, 0, 410);
    ui_state_tick(320 + EMP_HOLD_MS + 20);
    m = sim_wire_last(EMP_OP_ACTIVATE);
    ok = ok && ui_state()->page == 8 && m && m->payload[12] == 1 && activations() == 2;
    ui_state_push(1, 0, 2000);
    check("the pinned stop fires on any page; a pinned hold survives the page change", ok);

    /* Below minor 8 the pin is cleared, as by a 1.7 device. */
    sim_wire_set_peer_minor(7);
    load_styled(442, page, 7);
    check("minor 7 peer: PINNED cleared", surf_pinned_mask() == 0);
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
}

int desc_run_selftests(desc_report_fn report)
{
    failures  = 0;
    report_fn = report;

    test_small_pages_use_bottom_row();
    test_sparse_small_pages_keep_order_and_ids();
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
    test_highlight_validation();
    test_defaults_store_and_reset();
    test_successive_descriptors_replace();
    test_pool_does_not_accumulate();

    test_extension_tag_is_skipped();
    test_text_tag_is_skipped();
    test_action_press_only();
    test_operator_keeps_action_row();
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

    test_style_and_tone_decode();
    test_style_validation();
    test_toned_values();
    test_older_minor_is_unchanged();
    test_estop_press();
    test_top_left_slot();
    test_presentation_colours();

    test_hold_decodes();
    test_hold_fires_once();
    test_hold_cancels();
    test_hold_leaves_others_alone();

    test_panel_decodes();
    test_disabled_action_is_inert();
    test_list_scrolls_locally();
    test_long_text_is_kept();
    test_pinned_slots();
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);

    report_fn = 0;
    return failures;
}
