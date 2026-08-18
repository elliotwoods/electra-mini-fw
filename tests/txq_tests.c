/* Host tests for the outbound queues.
 *
 * These exist because the failure mode here is silence. A queue that coalesces wrongly does not
 * crash, does not corrupt anything, and does not report a drop -- it simply delivers a knob to
 * the wrong place, and the only witness is a hand that moved further than the value did. That
 * is not something you can see from the panel, because the panel shows the device's own idea of
 * the value, which is right.
 *
 * txq.c takes its transmit path as a callback precisely so this file can exist: no USB, no
 * framing, no device.
 */

#include <string.h>

#include "emp.h"
#include "txq.h"

typedef void (*txq_report_fn)(const char *name, int passed);

#define CHECK(cond) do { if (!(cond)) { ok = 0; } } while (0)

static void run(const char *name, int ok, txq_report_fn report, int *failures)
{
    report(name, ok);
    if (!ok) (*failures)++;
}

/* ------------------------------------------------------------------ a fake transport */

typedef struct {
    uint8_t  channel;
    uint8_t  opcode;
    uint8_t  len;
    uint8_t  payload[TXQ_PAYLOAD_MAX];
} sent_t;

static sent_t   sent[64];
static unsigned sent_count;
static unsigned accept_budget;      /* how many emits the transport will take before refusing */
static unsigned drops;

static int fake_emit(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len,
                     void *ctx)
{
    (void)ctx;
    if (!accept_budget) return 0;
    accept_budget--;

    if (sent_count < 64) {
        sent[sent_count].channel = channel;
        sent[sent_count].opcode  = opcode;
        sent[sent_count].len     = (uint8_t)(len < TXQ_PAYLOAD_MAX ? len : TXQ_PAYLOAD_MAX);
        memcpy(sent[sent_count].payload, payload, sent[sent_count].len);
        sent_count++;
    }
    return 1;
}

static void fake_drop(uint8_t channel, uint8_t opcode, const char *why, void *ctx)
{
    (void)channel; (void)opcode; (void)why; (void)ctx;
    drops++;
}

static void begin(txq_t *q)
{
    sent_count = 0;
    drops = 0;
    accept_budget = 1000;
    txq_init(q, fake_emit, fake_drop, 0);
}

/* ------------------------------------------------------------------ message builders */

/* EDIT_DELTA: revision u64, edit_seq u32, id u16, delta i32. */
static uint32_t make_delta(uint8_t *b, uint16_t id, uint32_t seq, int32_t delta)
{
    memset(b, 0, 18);
    b[8]  = (uint8_t)seq;
    b[9]  = (uint8_t)(seq >> 8);
    b[12] = (uint8_t)id;
    b[13] = (uint8_t)(id >> 8);
    uint32_t d = (uint32_t)delta;
    b[14] = (uint8_t)d;
    b[15] = (uint8_t)(d >> 8);
    b[16] = (uint8_t)(d >> 16);
    b[17] = (uint8_t)(d >> 24);
    return 18;
}

/* EDIT: revision u64, edit_seq u32, id u16, cause u8, then a value. */
static uint32_t make_edit(uint8_t *b, uint16_t id, uint8_t cause)
{
    memset(b, 0, 24);
    b[12] = (uint8_t)id;
    b[13] = (uint8_t)(id >> 8);
    b[14] = cause;
    b[15] = EMP_VAL_NUMBER;
    return 24;
}

static uint32_t make_button(uint8_t *b, uint8_t which, uint8_t pressed)
{
    b[0] = which;
    b[1] = pressed;
    return 2;
}

static int32_t delta_of(const sent_t *m)
{
    return (int32_t)((uint32_t)m->payload[14] | ((uint32_t)m->payload[15] << 8)
                   | ((uint32_t)m->payload[16] << 16) | ((uint32_t)m->payload[17] << 24));
}

/* ------------------------------------------------------------------ tests */

static int test_deltas_accumulate(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);

    /* A knob swept quickly while the link is busy: eight detents, one direction, no chance to
     * drain between them. If coalescing REPLACED rather than summed -- which is what "a newer
     * entry for a queued id replaces it" says literally -- seven eighths of the movement would
     * vanish, and it would vanish more the faster the hand moved. */
    for (unsigned i = 0; i < 8; i++) {
        make_delta(b, 42, i + 1, 1);
        txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);
    }

    CHECK(txq_depth(&q) == 1);            /* one message... */
    CHECK(txq_pump(&q) == 1);
    CHECK(sent_count == 1);
    CHECK(delta_of(&sent[0]) == 8);       /* ...carrying the whole sweep */
    CHECK(drops == 0);
    return ok;
}

static int test_deltas_cancel(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);

    /* Back and forth. The sum is the truth: the knob ended where it started, and sending +1
     * would move the value by one detent the hand did not make. */
    make_delta(b, 7, 1, +3);  txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);
    make_delta(b, 7, 2, -3);  txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);

    CHECK(txq_pump(&q) == 1);
    CHECK(sent_count == 1);
    CHECK(delta_of(&sent[0]) == 0);
    return ok;
}

static int test_absolute_edits_supersede(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);

    /* An EDIT says where the knob IS, so only the newest matters -- the opposite of a delta,
     * and the reason the two cannot share one rule. */
    make_edit(b, 9, EMP_CAUSE_ROTATE); txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT, b, 24);
    make_edit(b, 9, EMP_CAUSE_UNDO);   txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT, b, 24);

    CHECK(txq_depth(&q) == 1);
    CHECK(txq_pump(&q) == 1);
    CHECK(sent[0].payload[14] == EMP_CAUSE_UNDO);
    return ok;
}

static int test_different_fields_do_not_merge(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);

    for (uint16_t id = 1; id <= 8; id++) {
        make_delta(b, id, id, (int32_t)id);
        txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);
    }

    CHECK(txq_depth(&q) == 8);
    CHECK(txq_pump(&q) == 8);

    /* And in the order the hand touched them. */
    for (unsigned i = 0; i < 8; i++) CHECK(delta_of(&sent[i]) == (int32_t)(i + 1));
    return ok;
}

static int test_buttons_never_coalesce(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[8];
    begin(&q);

    /* Two presses of the same button are two presses. Merging them would be wrong in a way
     * that merging two positions of the same knob is not. */
    make_button(b, 3, 1); txq_push(&q, EMP_CH_INPUT, EMP_OP_BUTTON, b, 2);
    make_button(b, 3, 0); txq_push(&q, EMP_CH_INPUT, EMP_OP_BUTTON, b, 2);
    make_button(b, 3, 1); txq_push(&q, EMP_CH_INPUT, EMP_OP_BUTTON, b, 2);
    make_button(b, 3, 0); txq_push(&q, EMP_CH_INPUT, EMP_OP_BUTTON, b, 2);

    CHECK(txq_depth(&q) == 4);
    CHECK(txq_pump(&q) == 4);
    CHECK(sent[0].payload[1] == 1 && sent[1].payload[1] == 0);
    CHECK(sent[2].payload[1] == 1 && sent[3].payload[1] == 0);
    return ok;
}

static int test_control_goes_first(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);

    /* A burst of knob movement, and then a heartbeat. Rule F4: the heartbeat must not queue
     * behind the burst, because a busy link is exactly when liveness matters most. */
    for (uint16_t id = 1; id <= 6; id++) {
        make_delta(b, id, id, 1);
        txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);
    }
    b[0] = 0xAB;
    txq_push(&q, EMP_CH_CONTROL, EMP_OP_HEARTBEAT, b, 1);

    CHECK(txq_pump(&q) == 7);
    CHECK(sent[0].channel == EMP_CH_CONTROL);
    CHECK(sent[0].opcode == EMP_OP_HEARTBEAT);
    return ok;
}

static int test_button_and_knob_keep_their_order(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);

    /* The discrete queue is not a priority queue. A press that happened after a turn must
     * arrive after it, or a host reconstructing the gesture sees the button first. */
    make_delta(b, 5, 1, 2);  txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);
    make_button(b, 1, 1);    txq_push(&q, EMP_CH_INPUT, EMP_OP_BUTTON, b, 2);
    make_delta(b, 6, 2, 3);  txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);

    CHECK(txq_pump(&q) == 3);
    CHECK(sent[0].opcode == EMP_OP_EDIT_DELTA);
    CHECK(sent[1].opcode == EMP_OP_BUTTON);
    CHECK(sent[2].opcode == EMP_OP_EDIT_DELTA);
    return ok;
}

static int test_superseded_entry_keeps_its_place(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);

    /* Field 1 is nudged, then field 2, then field 1 again. Field 1 was touched first and must
     * still be sent first: refreshing its position on every nudge would let a knob under a
     * moving finger starve every other control on the panel. */
    make_edit(b, 1, EMP_CAUSE_ROTATE); txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT, b, 24);
    make_edit(b, 2, EMP_CAUSE_ROTATE); txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT, b, 24);
    make_edit(b, 1, EMP_CAUSE_TOUCH);  txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT, b, 24);

    CHECK(txq_depth(&q) == 2);
    CHECK(txq_pump(&q) == 2);
    CHECK(sent[0].payload[12] == 1);
    CHECK(sent[0].payload[14] == EMP_CAUSE_TOUCH);   /* newest content... */
    CHECK(sent[1].payload[12] == 2);                 /* ...oldest position */
    return ok;
}

static int test_a_full_transport_does_not_lose_anything(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);

    /* The transport takes two messages and then refuses. Nothing may be dropped and nothing may
     * be sent twice; the pump simply stops and picks up where it left off. */
    for (uint16_t id = 1; id <= 5; id++) {
        make_delta(b, id, id, (int32_t)id);
        txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);
    }

    accept_budget = 2;
    CHECK(txq_pump(&q) == 2);
    CHECK(txq_depth(&q) == 3);
    CHECK(drops == 0);

    accept_budget = 1000;
    CHECK(txq_pump(&q) == 3);
    CHECK(sent_count == 5);
    for (unsigned i = 0; i < 5; i++) CHECK(delta_of(&sent[i]) == (int32_t)(i + 1));
    return ok;
}

static int test_overflow_is_reported(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[8];
    begin(&q);
    accept_budget = 0;                       /* nothing can leave */

    for (unsigned i = 0; i < TXQ_DISCRETE_DEPTH + 4u; i++) {
        make_button(b, (uint8_t)i, 1);
        txq_push(&q, EMP_CH_INPUT, EMP_OP_BUTTON, b, 2);
    }

    /* Presses cannot be merged, so past the depth they are lost -- and a loss that is not
     * reported is the failure this whole protocol exists to eliminate. */
    CHECK(txq_depth(&q) == TXQ_DISCRETE_DEPTH);
    CHECK(drops == 4);
    CHECK(q.dropped == 4);
    return ok;
}

static int test_knobs_never_overflow(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);
    accept_budget = 0;

    /* The point of coalescing: a thousand movements across the eight physical knobs, with the
     * link completely stalled, must not drop one thing. Resolution degrades; events do not
     * vanish. */
    for (unsigned i = 0; i < 1000; i++) {
        make_delta(b, (uint16_t)(1 + (i % 8u)), i + 1, 1);
        txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);
    }

    CHECK(drops == 0);
    CHECK(txq_depth(&q) == 8);

    accept_budget = 1000;
    CHECK(txq_pump(&q) == 8);

    int32_t total = 0;
    for (unsigned i = 0; i < sent_count; i++) total += delta_of(&sent[i]);
    CHECK(total == 1000);                    /* every detent of it */
    return ok;
}

static int test_clear_drops_the_backlog(void)
{
    int ok = 1;
    txq_t q;
    uint8_t b[32];
    begin(&q);
    accept_budget = 0;

    make_delta(b, 1, 1, 5); txq_push(&q, EMP_CH_INPUT, EMP_OP_EDIT_DELTA, b, 18);
    make_button(b, 2, 1);   txq_push(&q, EMP_CH_INPUT, EMP_OP_BUTTON, b, 2);

    /* Losing the host discards the backlog. A knob position from before the cable was pulled
     * would arrive as a fresh edit and overwrite whatever the host has since decided. */
    txq_clear(&q);
    CHECK(txq_depth(&q) == 0);

    accept_budget = 1000;
    CHECK(txq_pump(&q) == 0);
    CHECK(sent_count == 0);
    return ok;
}

/* ------------------------------------------------------------------ entry */

int txq_run_selftests(txq_report_fn report)
{
    int failures = 0;
    run("deltas accumulate",        test_deltas_accumulate(),                 report, &failures);
    run("deltas cancel",            test_deltas_cancel(),                     report, &failures);
    run("absolute edits supersede", test_absolute_edits_supersede(),          report, &failures);
    run("fields stay separate",     test_different_fields_do_not_merge(),     report, &failures);
    run("buttons never coalesce",   test_buttons_never_coalesce(),            report, &failures);
    run("control goes first",       test_control_goes_first(),                report, &failures);
    run("gesture order preserved",  test_button_and_knob_keep_their_order(),  report, &failures);
    run("superseded keeps place",   test_superseded_entry_keeps_its_place(),  report, &failures);
    run("a full transport waits",   test_a_full_transport_does_not_lose_anything(), report, &failures);
    run("overflow is reported",     test_overflow_is_reported(),              report, &failures);
    run("knobs never overflow",     test_knobs_never_overflow(),              report, &failures);
    run("clear drops the backlog",  test_clear_drops_the_backlog(),           report, &failures);
    return failures;
}
