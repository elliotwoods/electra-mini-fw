/* See txq.h for why nothing sends from where it is produced, and what each queue is for. */

#include <string.h>

#include "emp.h"
#include "txq.h"

void txq_init(txq_t *q, txq_emit_fn emit, txq_drop_fn drop, void *ctx)
{
    memset(q, 0, sizeof(*q));
    q->emit = emit;
    q->drop = drop;
    q->ctx  = ctx;
}

void txq_clear(txq_t *q)
{
    memset(q->coalesce, 0, sizeof(q->coalesce));
    memset(q->discrete, 0, sizeof(q->discrete));
    q->discrete_head = q->discrete_count = 0;
    q->control_used = 0;
    q->control_count = 0;
    q->stamp = 0;
}

static void dropped(txq_t *q, uint8_t channel, uint8_t opcode, const char *why)
{
    q->dropped++;
    if (q->drop) q->drop(channel, opcode, why, q->ctx);
}

static int coalescing_opcode(uint8_t opcode)
{
    return opcode == EMP_OP_EDIT || opcode == EMP_OP_EDIT_DELTA
        || opcode == EMP_OP_FOCUS || opcode == EMP_OP_SCREEN;
}

/* Which field a message is about, for coalescing. EDIT and EDIT_DELTA both carry revision u64,
 * then edit_seq u32, then id u16, so the id sits at offset 12 in each. FOCUS and SCREEN
 * describe the panel rather than one field and are therefore singletons. */
static uint16_t coalesce_key(uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    if ((opcode == EMP_OP_EDIT || opcode == EMP_OP_EDIT_DELTA) && len >= 14) {
        return (uint16_t)(payload[12] | (payload[13] << 8));
    }
    return 0;
}

/* EDIT_DELTA's delta, at offset 14 after revision u64, edit_seq u32 and id u16. */
#define DELTA_OFF 14u
#define DELTA_END 18u

static int32_t get_delta(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[DELTA_OFF] | ((uint32_t)p[DELTA_OFF + 1] << 8)
                   | ((uint32_t)p[DELTA_OFF + 2] << 16) | ((uint32_t)p[DELTA_OFF + 3] << 24));
}

static void put_delta(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;
    p[DELTA_OFF]     = (uint8_t)u;
    p[DELTA_OFF + 1] = (uint8_t)(u >> 8);
    p[DELTA_OFF + 2] = (uint8_t)(u >> 16);
    p[DELTA_OFF + 3] = (uint8_t)(u >> 24);
}

static void push_coalescing(txq_t *q, uint8_t channel, uint8_t opcode,
                            const uint8_t *payload, uint32_t len)
{
    uint16_t key = coalesce_key(opcode, payload, len);

    txq_slot_t *slot = 0;
    for (unsigned i = 0; i < TXQ_COALESCE_SLOTS; i++) {
        if (q->coalesce[i].stamp && q->coalesce[i].opcode == opcode
            && q->coalesce[i].key == key) {
            slot = &q->coalesce[i];
            break;
        }
    }

    if (slot && opcode == EMP_OP_EDIT_DELTA && len >= DELTA_END && slot->len >= DELTA_END) {
        /* Deltas ACCUMULATE; they do not replace.
         *
         * This is the one place where "a newer entry supersedes the older" would be wrong. A
         * delta says how far the knob MOVED, not where it is, so discarding the earlier one
         * loses that movement outright -- a fast sweep would arrive shorter than the hand made
         * it, and the faster the sweep the more it would lose. Summing is what makes coalescing
         * lossless here rather than merely bounded.
         *
         * The stamp is deliberately NOT refreshed: the event keeps its place in the queue, so
         * an absolute EDIT issued afterwards still goes out afterwards. */
        int32_t sum = get_delta(slot->payload) + get_delta(payload);
        memcpy(slot->payload, payload, len);        /* newest revision and edit_seq */
        slot->len = (uint8_t)len;
        put_delta(slot->payload, sum);
        return;
    }

    if (!slot) {
        for (unsigned i = 0; i < TXQ_COALESCE_SLOTS; i++) {
            if (!q->coalesce[i].stamp) { slot = &q->coalesce[i]; break; }
        }
    }
    if (!slot) {
        dropped(q, channel, opcode, "coalescing queue full");
        return;
    }

    /* A superseded entry keeps its original position, so ordering between DIFFERENT controls is
     * the order the hand touched them, not the order they were last nudged. */
    if (!slot->stamp) slot->stamp = ++q->stamp;
    slot->key     = key;
    slot->channel = channel;
    slot->opcode  = opcode;
    slot->len     = (uint8_t)len;
    memcpy(slot->payload, payload, len);
}

static void push_discrete(txq_t *q, uint8_t channel, uint8_t opcode,
                          const uint8_t *payload, uint32_t len)
{
    if (q->discrete_count >= TXQ_DISCRETE_DEPTH) {
        dropped(q, channel, opcode, "discrete queue full");
        return;
    }

    unsigned i = (q->discrete_head + q->discrete_count) % TXQ_DISCRETE_DEPTH;
    q->discrete[i].stamp   = ++q->stamp;
    q->discrete[i].channel = channel;
    q->discrete[i].opcode  = opcode;
    q->discrete[i].len     = (uint8_t)len;
    if (len) memcpy(q->discrete[i].payload, payload, len);
    q->discrete_count++;
}

static void push_control(txq_t *q, uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    uint32_t need = TXQ_CONTROL_HDR + len;

    if (need > TXQ_CONTROL_BYTES || q->control_used + need > TXQ_CONTROL_BYTES) {
        /* No drop callback here: the callback's whole purpose is to raise a DIAG, and a DIAG is
         * itself a control message -- reporting this failure would queue another copy of it. */
        q->dropped++;
        return;
    }

    uint8_t *p = q->control + q->control_used;
    p[0] = opcode;
    p[1] = (uint8_t)len;
    p[2] = (uint8_t)(len >> 8);
    if (len) memcpy(p + TXQ_CONTROL_HDR, payload, len);

    q->control_used = (uint16_t)(q->control_used + need);
    q->control_count++;
}

void txq_push(txq_t *q, uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    if (channel == EMP_CH_CONTROL) { push_control(q, opcode, payload, len); return; }

    if (len > TXQ_PAYLOAD_MAX) {
        /* Nothing on the input channel comes near this, and truncating one would put a
         * malformed message on the wire -- which the CRC would not catch, because it would be
         * computed over the truncation. */
        dropped(q, channel, opcode, "input message too large to queue");
        return;
    }

    if (coalescing_opcode(opcode)) push_coalescing(q, channel, opcode, payload, len);
    else                           push_discrete(q, channel, opcode, payload, len);
}

/* Oldest stamp still waiting in the coalescing queue, or 0 if it is empty. */
static uint32_t oldest_coalesced(const txq_t *q)
{
    uint32_t oldest = 0;
    for (unsigned i = 0; i < TXQ_COALESCE_SLOTS; i++) {
        if (q->coalesce[i].stamp && (!oldest || q->coalesce[i].stamp < oldest)) {
            oldest = q->coalesce[i].stamp;
        }
    }
    return oldest;
}

unsigned txq_pump(txq_t *q)
{
    unsigned sent = 0;

    for (;;) {
        /* Control first, unconditionally. Rule F4 exists so a heartbeat cannot queue behind a
         * burst of knob movement. */
        if (q->control_count) {
            const uint8_t *p = q->control;
            uint16_t len = (uint16_t)(p[1] | (p[2] << 8));

            if (!q->emit(EMP_CH_CONTROL, p[0], p + TXQ_CONTROL_HDR, len, q->ctx)) return sent;

            uint16_t took = (uint16_t)(TXQ_CONTROL_HDR + len);
            q->control_used = (uint16_t)(q->control_used - took);
            if (q->control_used) memmove(q->control, q->control + took, q->control_used);
            q->control_count--;
            sent++;
            continue;
        }

        uint32_t oldest = oldest_coalesced(q);

        /* A button press and a knob turn are delivered in the order they happened, which is the
         * order the hand made them -- so the discrete queue goes first only when it is actually
         * older, not merely because it is discrete. */
        if (q->discrete_count) {
            txq_slot_t *d = &q->discrete[q->discrete_head];
            if (!oldest || d->stamp < oldest) {
                if (!q->emit(d->channel, d->opcode, d->payload, d->len, q->ctx)) return sent;
                q->discrete_head = (uint8_t)((q->discrete_head + 1u) % TXQ_DISCRETE_DEPTH);
                q->discrete_count--;
                sent++;
                continue;
            }
        }

        if (!oldest) return sent;

        txq_slot_t *pick = 0;
        for (unsigned i = 0; i < TXQ_COALESCE_SLOTS; i++) {
            if (q->coalesce[i].stamp == oldest) { pick = &q->coalesce[i]; break; }
        }
        if (!pick) return sent;

        if (!q->emit(pick->channel, pick->opcode, pick->payload, pick->len, q->ctx)) return sent;
        pick->stamp = 0;
        sent++;
    }
}

unsigned txq_depth(const txq_t *q)
{
    unsigned n = (unsigned)q->control_count + (unsigned)q->discrete_count;
    for (unsigned i = 0; i < TXQ_COALESCE_SLOTS; i++) {
        if (q->coalesce[i].stamp) n++;
    }
    return n;
}
