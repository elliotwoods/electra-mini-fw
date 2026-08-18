/* EMP/1 surface channel, device side. See docs/protocol.md sections 3.2, 3.3 and 3.5.
 *
 * Three rules here are load-bearing and are the reason this file exists rather than the host
 * simply pushing values at a dumb panel:
 *
 *  1. **The descriptor is a transaction of per-field messages, not one giant blob.** Each
 *     DESC_FIELD parses straight into a staging slot, so there is no multi-kilobyte reassembly
 *     buffer, no allocation, and a failure names a field index instead of "the transfer broke".
 *
 *  2. **The revision gate.** A VALUES for a descriptor we do not have is dropped and answered
 *     with DESC_REQUEST. Applying it would mean writing a value into whatever field happens to
 *     hold that id in a different layout — the exact class of bug the host's own descriptor
 *     stamping exists to prevent, closed here in the other direction.
 *
 *  3. **Echo suppression is causal, not timed.** Every EDIT carries a monotonically increasing
 *     edit_seq; an inbound value is applied only if the field is untouched, or if the host has
 *     acknowledged at least as far as our last edit. The integration this replaces used a
 *     350 ms timer, which is a guess about round-trip time dressed up as a rule.
 */

#include <string.h>
#include "surface.h"
#include "session.h"
#include "diag.h"

/* Provided by session.c: framing and transmission live there, policy lives here. */
void emp_send(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len);

/* TWO arenas, ping-ponged, so a descriptor that fails verification never becomes the live one.
 *
 * This used to be a comment over a set of scalars and a single array, which is not the same
 * thing at all. DESC_BEGIN zeroed the live field count and DESC_FIELD wrote straight into the
 * live array, so a CRC mismatch, a broken sequence or a truncated field left the device showing
 * NOTHING -- with applied_revision still set, so the host's next VALUES passed the revision gate
 * and then silently matched no field at all. Even a successful transfer blanked the panel for
 * its entire duration.
 *
 * The cost is one more arena, about 8.7 KB. That is affordable here, and it is bought back with
 * change to spare by no longer linking the codec tests into the image. */
typedef struct { uint16_t off, len; } surf_choice_t;

typedef struct {
    surf_field_t  fields[SURF_MAX_FIELDS];
    char          pool[SURF_STRING_POOL];
    surf_choice_t choices[SURF_MAX_CHOICES];
    uint16_t      field_count;
    uint16_t      pool_used;
    uint16_t      choices_used;
    uint64_t      revision;
} surf_arena_t;

static surf_arena_t arena[2];
static uint8_t      live_ix;           /* which arena the panel is reading */

#define LIVE   (&arena[live_ix])
#define STAGE  (&arena[live_ix ^ 1u])

static uint16_t     page;

/* Transaction state for the descriptor being received into STAGE. */
static uint64_t     staging_revision;
static uint16_t     staging_expect;
static uint16_t     staging_next;      /* index the next DESC_FIELD must carry */
static uint32_t     staging_crc;       /* running CRC over concatenated field payloads */
static int          staging_open;

/* Echo suppression state, per field. */
static uint32_t     last_edit_seq[SURF_MAX_FIELDS];
static uint32_t     edit_seq;
static uint16_t     touched_mask;

/* ------------------------------------------------------------------ decode helpers */

typedef struct { const uint8_t *p; uint32_t len; uint32_t off; int bad; } rd_t;

static void rd_init(rd_t *r, const uint8_t *p, uint32_t len) { r->p = p; r->len = len; r->off = 0; r->bad = 0; }

static int rd_avail(rd_t *r, uint32_t n)
{
    if (r->bad || r->off + n > r->len) { r->bad = 1; return 0; }
    return 1;
}

static uint8_t  rd8(rd_t *r)  { if (!rd_avail(r, 1)) return 0; return r->p[r->off++]; }
static uint16_t rd16(rd_t *r) { if (!rd_avail(r, 2)) return 0; uint16_t v = (uint16_t)(r->p[r->off] | ((uint16_t)r->p[r->off+1] << 8)); r->off += 2; return v; }
static uint32_t rd32(rd_t *r) { uint32_t lo = rd16(r); uint32_t hi = rd16(r); return lo | (hi << 16); }
static uint64_t rd64(rd_t *r) { uint64_t lo = rd32(r); uint64_t hi = rd32(r); return lo | (hi << 32); }

/* Never cast a buffer pointer to a double: nothing on the wire is aligned, and an unaligned
 * load is undefined behaviour regardless of what this core happens to tolerate. */
static double rdf64(rd_t *r)
{
    double d = 0.0;
    if (!rd_avail(r, 8)) return 0.0;
    memcpy(&d, r->p + r->off, 8);
    r->off += 8;
    return d;
}

/* Is this an f64 the device can do arithmetic with?
 *
 * docs/protocol.md 3.4 forbids encoding a non-finite value, but a device that only trusts that
 * rule is trusting the host. A NaN here does not stay here: it propagates into the bar geometry
 * (every comparison against it is false, so the bar draws at an arbitrary length), into the
 * digit editor, and back out to the host on the next rotation. It is cheaper to refuse it at
 * the boundary than to make every consumer NaN-aware.
 *
 * Tested by bit pattern rather than with <math.h>: src/proto depends on <stdint.h> and
 * <string.h> only, which is what lets this code run under a sanitiser off-target. */
static int finite_f64(double d)
{
    uint64_t bits;
    memcpy(&bits, &d, 8);
    return ((bits >> 52) & 0x7FFu) != 0x7FFu;      /* exponent all ones == inf or NaN */
}

/* Copies into the string pool and returns its offset. Strings are UTF-8 and not terminated on
 * the wire; the pool keeps them that way and carries an explicit length. */
static int rd_string(rd_t *r, uint16_t *off_out, uint16_t *len_out)
{
    uint16_t n = rd16(r);
    if (!rd_avail(r, n)) return 0;

    surf_arena_t *a = STAGE;

    if (a->pool_used + n > SURF_STRING_POOL) {
        /* Out of pool. Record nothing rather than half a string; the field still loads, it
         * simply has no label, and the truncation is reported rather than hidden. */
        r->off += n;
        *off_out = 0;
        *len_out = 0;
        return 0;
    }
    memcpy(a->pool + a->pool_used, r->p + r->off, n);
    *off_out = a->pool_used;
    *len_out = n;
    a->pool_used = (uint16_t)(a->pool_used + n);
    r->off += n;
    return 1;
}

/* Read one value of any tag, storing it if we understand it and STEPPING OVER IT if we do not.
 *
 * Every tag in docs/protocol.md 3.4 carries enough information to determine its own length --
 * that is the whole point of the `0x80-0xFF` extension range being `len` u16 + bytes. The old
 * code read the tag, failed to recognise it, and then read the NEXT member of the field record
 * from wherever the value's body happened to start: label, unit, choices and default all came
 * back as garbage, and the CRC then reported a transfer that had in fact arrived intact.
 *
 * The three return codes matter separately. STORED and SKIPPED both mean the reader is now
 * positioned correctly and the record can continue; UNDECODABLE means it is not, and the caller
 * must abandon the record rather than guess. */
#define RDV_UNDECODABLE 0
#define RDV_STORED      1
#define RDV_SKIPPED     2
#define RDV_NONFINITE   3   /* read correctly, refused on its merits; value left at zero */

static int rd_value(rd_t *r, uint8_t tag, uint8_t *b_out, double *n_out, uint32_t *c_out)
{
    switch (tag) {
    case EMP_VAL_BOOL:   *b_out = rd8(r);  return RDV_STORED;
    case EMP_VAL_CHOICE: *c_out = rd32(r); return RDV_STORED;

    case EMP_VAL_NUMBER: {
        double d = rdf64(r);
        if (!finite_f64(d)) {
            emp_diag(EMP_SEV_WARN, EMP_DIAG_NON_FINITE, 0, "non-finite f64 substituted with 0");
            d = 0.0;
            return RDV_NONFINITE;
        }
        *n_out = d;
        return RDV_STORED;
    }

    case EMP_VAL_TEXT: {
        /* A String. Skipped rather than pooled: nothing on this panel renders a text value, and
         * spending pool on one would cost a label somewhere else. */
        uint16_t n = rd16(r);
        if (!rd_avail(r, n)) { r->bad = 1; return RDV_UNDECODABLE; }
        r->off += n;
        emp_diag(EMP_SEV_INFO, EMP_DIAG_UNKNOWN_VALUE_TAG, EMP_VAL_TEXT,
                 "text values are not rendered");
        return RDV_SKIPPED;
    }

    case EMP_VAL_COLOR: {
        /* `count` u8 + count x f32. Colour is a deferred feature, not an unknown one. */
        uint8_t n = rd8(r);
        uint32_t bytes = (uint32_t)n * 4u;
        if (!rd_avail(r, bytes)) { r->bad = 1; return RDV_UNDECODABLE; }
        r->off += bytes;
        emp_diag(EMP_SEV_INFO, EMP_DIAG_UNKNOWN_VALUE_TAG, EMP_VAL_COLOR,
                 "colour values are not rendered yet");
        return RDV_SKIPPED;
    }

    default:
        if (tag >= 0x80u) {
            /* The extension range, which exists precisely so this case is survivable. */
            uint16_t n = rd16(r);
            if (!rd_avail(r, n)) { r->bad = 1; return RDV_UNDECODABLE; }
            r->off += n;
            emp_diag(EMP_SEV_INFO, EMP_DIAG_UNKNOWN_VALUE_TAG, tag,
                     "extension value tag stepped over");
            return RDV_SKIPPED;
        }
        /* 0x05-0x7F: reserved, and reserved tags carry no length. There is no way to find the
         * end of this value, so there is no honest way to continue reading the record. */
        emp_diag(EMP_SEV_ERROR, EMP_DIAG_VALUE_UNDECODABLE, tag,
                 "reserved value tag has no length");
        return RDV_UNDECODABLE;
    }
}

/* ------------------------------------------------------------------ encode helpers */

typedef struct { uint8_t *p; uint32_t cap; uint32_t len; } wr_t;

static void wr_init(wr_t *w, uint8_t *p, uint32_t cap) { w->p = p; w->cap = cap; w->len = 0; }
static void wr8(wr_t *w, uint8_t v)   { if (w->len < w->cap) w->p[w->len] = v; w->len++; }
static void wr16(wr_t *w, uint16_t v) { wr8(w, (uint8_t)v); wr8(w, (uint8_t)(v >> 8)); }
static void wr32(wr_t *w, uint32_t v) { wr16(w, (uint16_t)v); wr16(w, (uint16_t)(v >> 16)); }
static void wr64(wr_t *w, uint64_t v) { wr32(w, (uint32_t)v); wr32(w, (uint32_t)(v >> 32)); }

/* The encode side of the same rule: a non-finite f64 MUST NOT go on the wire
 * (docs/protocol.md 3.4). Refusing it here is what keeps round-trip equality total, which is
 * the property the host's tests are built on -- a NaN that survived a round trip would not even
 * compare equal to itself. */
static void wrf64(wr_t *w, double d)
{
    if (!finite_f64(d)) {
        emp_diag(EMP_SEV_ERROR, EMP_DIAG_NON_FINITE, 0, "refused to encode a non-finite f64");
        d = 0.0;
    }

    uint8_t b[8];
    memcpy(b, &d, 8);
    for (int i = 0; i < 8; i++) wr8(w, b[i]);
}

/* ------------------------------------------------------------------ outbound */

static void send_desc_request(uint8_t reason)
{
    uint8_t buf[16];
    wr_t w;
    wr_init(&w, buf, sizeof(buf));
    wr8(&w, reason);
    wr64(&w, LIVE->revision);
    emp_send(EMP_CH_INPUT, EMP_OP_DESC_REQUEST, buf, w.len);
}

static void send_desc_ack(uint64_t revision, uint16_t count)
{
    uint8_t buf[16];
    wr_t w;
    wr_init(&w, buf, sizeof(buf));
    wr64(&w, revision);
    wr16(&w, count);
    emp_send(EMP_CH_INPUT, EMP_OP_DESC_ACK, buf, w.len);
}

static void write_value(wr_t *w, const surf_field_t *f)
{
    switch (f->value_tag) {
    case EMP_VAL_BOOL:   wr8(w, EMP_VAL_BOOL);   wr8(w, f->boolean); break;
    case EMP_VAL_CHOICE: wr8(w, EMP_VAL_CHOICE); wr32(w, f->choice); break;
    default:             wr8(w, EMP_VAL_NUMBER); wrf64(w, f->number); break;
    }
}

static void send_edit(uint16_t index, uint8_t cause)
{
    if (index >= LIVE->field_count) return;
    surf_field_t *f = &LIVE->fields[index];

    uint8_t buf[48];
    wr_t w;
    wr_init(&w, buf, sizeof(buf));

    edit_seq++;
    last_edit_seq[index] = edit_seq;

    /* The revision the value was decoded against travels with it. A keypress decoded against
     * an old layout must never be applied to a new one — the host enforces the same rule from
     * its side, and a rule enforced on only one side is not a rule. */
    wr64(&w, LIVE->revision);
    wr32(&w, edit_seq);
    wr16(&w, f->id);
    wr8(&w, cause);
    write_value(&w, f);

    emp_send(EMP_CH_INPUT, EMP_OP_EDIT, buf, w.len);
}

static void send_edit_delta(uint16_t index, int32_t delta)
{
    if (index >= LIVE->field_count) return;
    surf_field_t *f = &LIVE->fields[index];

    uint8_t buf[32];
    wr_t w;
    wr_init(&w, buf, sizeof(buf));

    edit_seq++;
    last_edit_seq[index] = edit_seq;

    wr64(&w, LIVE->revision);
    wr32(&w, edit_seq);
    wr16(&w, f->id);
    wr32(&w, (uint32_t)delta);

    emp_send(EMP_CH_INPUT, EMP_OP_EDIT_DELTA, buf, w.len);
}

/* ------------------------------------------------------------------ descriptor */

static void desc_begin(const uint8_t *msg, uint32_t len)
{
    rd_t r;
    rd_init(&r, msg, len);

    staging_revision = rd64(&r);
    staging_expect   = rd16(&r);
    (void)rd16(&r);                       /* flags */
    (void)rd32(&r);                       /* total_bytes, advisory */

    if (r.bad) return;

    if (staging_expect > SURF_MAX_FIELDS) {
        /* Say so immediately rather than accepting fields until we run out. A host that knows
         * it asked for too much can send a smaller page; a host that discovers it halfway
         * through has already wasted the transfer. */
        emp_diag(EMP_SEV_ERROR, EMP_DIAG_DESC_TOO_MANY_FIELDS, staging_expect,
                 "descriptor larger than this device can hold");
        send_desc_request(EMP_REQ_TOO_MANY_FIELDS);
        staging_open = 0;
        return;
    }

    staging_open  = 1;
    staging_next  = 0;
    staging_crc   = 0xFFFFFFFFu;

    /* Clear the SCRATCH arena. The live one is untouched, so the panel keeps showing the
     * descriptor it already has for the whole duration of the transfer -- and keeps it for good
     * if the transfer never completes. */
    STAGE->pool_used    = 0;
    STAGE->choices_used = 0;
    STAGE->field_count  = 0;
}

static void desc_field(const uint8_t *msg, uint32_t len)
{
    if (!staging_open) return;

    rd_t r;
    rd_init(&r, msg, len);

    uint16_t index = rd16(&r);
    if (r.bad) return;

    /* Indices MUST ascend by exactly one. A gap means a message was lost or reordered, and
     * silently filling the hole would produce a descriptor that looks complete and is wrong. */
    if (index != staging_next) {
        emp_diag(EMP_SEV_ERROR, EMP_DIAG_DESC_SEQUENCE, staging_next,
                 "descriptor field out of sequence");
        send_desc_request(EMP_REQ_SEQUENCE_BROKEN);
        staging_open = 0;
        return;
    }

    /* CRC covers the concatenated field payloads, so DESC_END can verify the whole transaction
     * even though it arrived as many independent messages. */
    staging_crc = emp_crc32c_update(staging_crc, msg + 2, len - 2);

    surf_field_t *f = &STAGE->fields[index];
    memset(f, 0, sizeof(*f));

    (void)rd16(&r);                       /* field_len: forward compatibility, see below */
    f->id           = rd16(&r);
    f->kind         = rd8(&r);
    f->flags        = rd8(&r);
    f->present      = rd16(&r);
    f->min          = rdf64(&r);
    f->max          = rdf64(&r);
    f->step         = rdf64(&r);
    f->precision    = rd8(&r);
    f->lane         = rd8(&r);
    f->choice_count = rd16(&r);

    f->value_tag = rd8(&r);
    switch (rd_value(&r, f->value_tag, &f->boolean, &f->number, &f->choice)) {
    case RDV_STORED:
        break;
    case RDV_SKIPPED:
    case RDV_NONFINITE:
        /* A value we stepped over cleanly, or one we read and refused. Keep the field, drop the
         * value: refusing the whole descriptor because one control used a tag from a newer host
         * would take away every other knob as well. The tag is normalised so that everything
         * downstream can assume value_tag is one of the three this device can render, and the
         * field is marked so the host can see WHICH control it lost rather than inferring it. */
        f->value_tag = EMP_VAL_NUMBER;
        f->number    = 0.0;
        f->flags     = (uint8_t)(f->flags | EMP_FIELD_TRUNCATED);
        break;
    default:
        /* Undecodable: the reader is now at an unknown offset, so nothing after this point in
         * the record means anything and the running CRC is over bytes we mis-split. rd_value
         * has already said which tag it was. */
        send_desc_request(EMP_REQ_SEQUENCE_BROKEN);
        staging_open = 0;
        return;
    }

    if (!rd_string(&r, &f->label_off, &f->label_len)) {
        /* The string pool is full. The field still loads and its knob still works; it simply
         * has no name on screen. That is a degraded control rather than a missing one, which is
         * the right trade -- but it is exactly the kind of degradation that must not be silent,
         * because from the panel it is indistinguishable from a host that sent no label. */
        f->flags = (uint8_t)(f->flags | EMP_FIELD_TRUNCATED);
        emp_diag(EMP_SEV_WARN, EMP_DIAG_STRING_TRUNCATED, index, "label dropped: pool full");
    }
    if (f->present & EMP_PRESENT_UNIT) {
        if (!rd_string(&r, &f->unit_off, &f->unit_len)) {
            f->flags = (uint8_t)(f->flags | EMP_FIELD_TRUNCATED);
            emp_diag(EMP_SEV_WARN, EMP_DIAG_STRING_TRUNCATED, index, "unit dropped: pool full");
        }
    }

    /* Choice option labels, per the field record's order in docs/protocol.md 3.4: they follow
     * `unit`. These used to be dropped on the floor, which made every Choice field display its
     * raw index -- "3" where the host had said "Sawtooth".
     *
     * A field whose labels do not fit keeps its count and loses its labels, and the UI falls
     * back to the index. That is a degraded display of a working control, which is the right
     * way round: refusing the descriptor would take away a knob that would otherwise work. */
    surf_arena_t *a = STAGE;

    f->choice_first = a->choices_used;
    if (f->choice_count) {
        for (uint16_t i = 0; i < f->choice_count; i++) {
            uint16_t off = 0, ln = 0;
            if (!rd_string(&r, &off, &ln)) {
                /* Ran out of pool or of message. Keep whatever landed; stop here. */
                f->flags = (uint8_t)(f->flags | EMP_FIELD_TRUNCATED);
                emp_diag(EMP_SEV_WARN, EMP_DIAG_STRING_TRUNCATED, index,
                         "choice label dropped: pool full");
                if (r.bad) break;
            }
            if (a->choices_used < SURF_MAX_CHOICES) {
                a->choices[a->choices_used].off = off;
                a->choices[a->choices_used].len = ln;
                a->choices_used++;
            } else {
                /* The knob will show "3" where the host said "Sawtooth". Worth saying so. */
                f->flags = (uint8_t)(f->flags | EMP_FIELD_TRUNCATED);
                emp_diag(EMP_SEV_WARN, EMP_DIAG_CHOICES_EXHAUSTED, index,
                         "choice labels dropped: table full");
            }
        }
    }

    /* The declared default follows the choices, per the field record in docs/protocol.md 3.4.
     * Parsed at last: it has always been on the wire and always been dropped, which is why the
     * device had no Reset -- you cannot restore a value you never stored. */
    if (f->present & EMP_PRESENT_DEFAULT) {
        f->default_tag = rd8(&r);
        switch (rd_value(&r, f->default_tag, &f->default_boolean,
                         &f->default_number, &f->default_choice)) {
        case RDV_STORED:
            break;
        case RDV_SKIPPED:
        case RDV_NONFINITE:
            /* Drop the default rather than the field: losing Reset on one control is a far
             * smaller harm than refusing the descriptor. */
            f->present = (uint16_t)(f->present & ~EMP_PRESENT_DEFAULT);
            f->flags   = (uint8_t)(f->flags | EMP_FIELD_TRUNCATED);
            break;
        default:
            send_desc_request(EMP_REQ_SEQUENCE_BROKEN);
            staging_open = 0;
            return;
        }
    }

    if (r.bad) {
        emp_diag(EMP_SEV_ERROR, EMP_DIAG_DESC_SEQUENCE, index, "descriptor field truncated");
        send_desc_request(EMP_REQ_SEQUENCE_BROKEN);
        staging_open = 0;
        return;
    }

    staging_next = (uint16_t)(index + 1);
}

static void desc_end(const uint8_t *msg, uint32_t len)
{
    if (!staging_open) return;

    rd_t r;
    rd_init(&r, msg, len);

    uint64_t revision = rd64(&r);
    uint16_t count    = rd16(&r);
    uint32_t crc      = rd32(&r);
    if (r.bad) return;

    staging_open = 0;

    if (count != staging_next || revision != staging_revision) {
        emp_diag(EMP_SEV_ERROR, EMP_DIAG_DESC_SEQUENCE, staging_next,
                 "descriptor ended with a count or revision we did not build");
        send_desc_request(EMP_REQ_SEQUENCE_BROKEN);
        return;
    }
    if ((~staging_crc) != crc) {
        /* The transfer arrived complete and wrong. Without this check that is indistinguishable
         * from arriving correct, which is the failure the old integration could not see. */
        emp_diag(EMP_SEV_ERROR, EMP_DIAG_DESC_CRC, count,
                 "descriptor arrived complete and wrong");
        send_desc_request(EMP_REQ_CRC_MISMATCH);
        return;
    }

    /* Verified. PUBLISH by swapping, which is a single store -- there is no instant at which
     * the panel can read a half-built descriptor, and the arena that was live becomes the
     * scratch space for the next transfer. */
    STAGE->field_count = count;
    STAGE->revision    = revision;
    live_ix ^= 1u;

    page = 0;
    for (uint16_t i = 0; i < SURF_MAX_FIELDS; i++) last_edit_seq[i] = 0;

    send_desc_ack(revision, count);
}

/* ------------------------------------------------------------------ values */

static int index_of_id(uint16_t id, uint16_t *out)
{
    for (uint16_t i = 0; i < LIVE->field_count; i++) {
        if (LIVE->fields[i].id == id) { *out = i; return 1; }
    }
    return 0;
}

static void handle_values(const uint8_t *msg, uint32_t len)
{
    rd_t r;
    rd_init(&r, msg, len);

    uint64_t revision = rd64(&r);
    uint16_t count    = rd16(&r);
    if (r.bad) return;

    /* The revision gate. 0 means "unstamped", accepted only because legacy fixtures use it. */
    if (revision != 0 && revision != LIVE->revision) {
        emp_diag(EMP_SEV_WARN, EMP_DIAG_REVISION_UNKNOWN, (uint32_t)revision,
                 "values for a descriptor this device does not hold");
        send_desc_request(EMP_REQ_REVISION_UNKNOWN);
        return;
    }

    for (uint16_t n = 0; n < count && !r.bad; n++) {
        uint16_t id        = rd16(&r);
        uint32_t ack_edit  = rd32(&r);
        uint8_t  tag       = rd8(&r);

        double   number = 0.0;
        uint32_t choice = 0;
        uint8_t  boolean = 0;

        /* A tag we can step over costs one control its update, not the batch. A tag we cannot
         * costs the batch, because every value after it would be read from the wrong offset --
         * which is worse than dropping them, since they would be applied to real controls. */
        int got = rd_value(&r, tag, &boolean, &number, &choice);
        if (got == RDV_UNDECODABLE) break;
        if (r.bad) break;

        /* A refused value leaves the control showing the last good one. Substituting zero, as
         * the encode rule does for a fresh descriptor, would make a knob jump to the bottom of
         * its range because of a host-side arithmetic mistake -- on a controller driving
         * something audible, that is the worse of the two failures. */
        if (got == RDV_SKIPPED || got == RDV_NONFINITE) continue;

        uint16_t idx;
        if (!index_of_id(id, &idx)) continue;

        /* Causal echo suppression. A value is applied if the field is not under a finger, or if
         * the host has accounted for everything we have sent about it. A value that arrives
         * acknowledging our latest edit but carrying a DIFFERENT number is a correction — a
         * clamp, a quantisation — and must be applied; anything older is a stale echo of our
         * own gesture and must not be, or the knob fights the hand turning it. */
        int under_hand = (touched_mask & (1u << (idx % SURF_POTS))) != 0;
        if (under_hand && ack_edit < last_edit_seq[idx]) continue;

        surf_field_t *f = &LIVE->fields[idx];
        f->value_tag = tag;
        f->number    = number;
        f->choice    = choice;
        f->boolean   = boolean;
    }
}

/* ------------------------------------------------------------------ public */

void surf_init(void)
{
    memset(arena, 0, sizeof(arena));
    memset(last_edit_seq, 0, sizeof(last_edit_seq));
    live_ix = 0;
    page = 0;
    staging_open = 0;
    edit_seq = 0;
    touched_mask = 0;
}

void surf_handle(uint8_t opcode, const uint8_t *msg, uint32_t len)
{
    switch (opcode) {
    case EMP_OP_DESC_BEGIN: desc_begin(msg, len); break;
    case EMP_OP_DESC_FIELD: desc_field(msg, len); break;
    case EMP_OP_DESC_END:   desc_end(msg, len);   break;
    case EMP_OP_DESC_ABORT: staging_open = 0;     break;
    case EMP_OP_VALUES:     handle_values(msg, len); break;
    case EMP_OP_CLEAR:      surf_init();          break;
    default: break;
    }
}

uint64_t surf_applied_revision(void) { return LIVE->revision; }
uint16_t surf_field_count(void)      { return LIVE->field_count; }
uint16_t surf_page(void)             { return page; }

const surf_field_t *surf_field(uint16_t index)
{
    return (index < LIVE->field_count) ? &LIVE->fields[index] : 0;
}

const char *surf_choice_label(const surf_field_t *f, uint16_t option, uint16_t *len)
{
    if (!f || option >= f->choice_count) return 0;

    uint32_t i = (uint32_t)f->choice_first + option;
    if (i >= LIVE->choices_used) return 0;
    if (!LIVE->choices[i].len) return 0;      /* the descriptor carried no label for this one */

    if (len) *len = LIVE->choices[i].len;
    return LIVE->pool + LIVE->choices[i].off;
}

const char *surf_string(uint16_t off, uint16_t len)
{
    if ((uint32_t)off + len > SURF_STRING_POOL) return 0;
    return LIVE->pool + off;
}


void surf_set_number_cause(uint16_t index, double v, uint8_t cause)
{
    if (index >= LIVE->field_count) return;
    surf_field_t *f = &LIVE->fields[index];
    if (f->kind == EMP_KIND_READONLY) return;      /* a read-only field is not ours to change */

    /* Nothing on the device should be able to produce this, but the arithmetic that reaches
     * here runs through the digit editor and a host-supplied min/max/step -- a step of zero and
     * a scale of infinity are both things a host can legitimately declare. Refusing at the door
     * keeps the NaN out of the stored value, not merely off the wire. */
    if (!finite_f64(v)) {
        emp_diag(EMP_SEV_ERROR, EMP_DIAG_NON_FINITE, index, "refused a non-finite edit");
        return;
    }

    f->number    = v;
    f->value_tag = EMP_VAL_NUMBER;
    send_edit(index, cause);
}

void surf_set_bool_cause(uint16_t index, uint8_t v, uint8_t cause)
{
    if (index >= LIVE->field_count) return;
    surf_field_t *f = &LIVE->fields[index];
    if (f->kind == EMP_KIND_READONLY) return;
    f->boolean   = v ? 1u : 0u;
    f->value_tag = EMP_VAL_BOOL;
    send_edit(index, cause);
}

void surf_set_choice_cause(uint16_t index, uint32_t v, uint8_t cause)
{
    if (index >= LIVE->field_count) return;
    surf_field_t *f = &LIVE->fields[index];
    if (f->kind == EMP_KIND_READONLY) return;
    f->choice    = v;
    f->value_tag = EMP_VAL_CHOICE;
    send_edit(index, cause);
}

void surf_set_number(uint16_t index, double v)   { surf_set_number_cause(index, v, EMP_CAUSE_ROTATE); }
void surf_set_bool(uint16_t index, uint8_t v)    { surf_set_bool_cause(index, v, EMP_CAUSE_ROTATE); }
void surf_set_choice(uint16_t index, uint32_t v) { surf_set_choice_cause(index, v, EMP_CAUSE_ROTATE); }

void surf_send_delta(uint16_t index, int32_t detents)
{
    if (index >= LIVE->field_count || !detents) return;
    send_edit_delta(index, detents);
}


/* A pot press is a UI action now, not an edit.
 *
 * This used to invert a Toggle field on press. With press meaning "drill into this field", a
 * Toggle knob would have done both at once -- drilled in AND flipped the value -- which is two
 * unrelated things from one gesture. Toggling stays on rotation, which already handles it.
 *
 * Kept as a no-op rather than deleted because the console's `sim p` still calls it, and because
 * the addressing it used was itself wrong: it went through surf_page(), which is permanently
 * zero, so a press addressed a different field from a turn as soon as the user paged. Drill
 * state now goes through ui_state, which holds the only correct notion of the current page. */
void surf_on_push(unsigned pot, int pressed)
{
    (void)pot;
    (void)pressed;
}

void surf_on_touch(uint16_t mask)
{
    touched_mask = mask;
}

/* Focus is reported PER LANE, not per control: a vector control occupies one knob per lane and
 * the panel senses touch per knob, so the field id alone under-describes what the hand is on.
 * The raw mask travels too, so a host holding the descriptor can reconstruct a multi-knob
 * gesture that a single (id, lane) pair cannot express. */
void surf_send_focus(int32_t index, uint16_t touch_mask)
{
    uint8_t buf[16];
    wr_t w;
    wr_init(&w, buf, sizeof(buf));

    int have = (index >= 0 && (uint16_t)index < LIVE->field_count);

    wr64(&w, LIVE->revision);
    wr8(&w, (uint8_t)(have ? 1 : 0));
    wr16(&w, have ? LIVE->fields[index].id : 0);
    wr8(&w, have ? LIVE->fields[index].lane : 0);
    wr16(&w, touch_mask);

    emp_send(EMP_CH_INPUT, EMP_OP_FOCUS, buf, w.len);
}

void surf_on_button(unsigned button, int pressed)
{
    /* BUTTON is diagnostics only and produces no host-side edit: device-local navigation must
     * never look like a user editing a value. */
    uint8_t buf[8];
    wr_t w;
    wr_init(&w, buf, sizeof(buf));
    wr8(&w, (uint8_t)button);
    wr8(&w, (uint8_t)(pressed ? 1 : 0));
    emp_send(EMP_CH_INPUT, EMP_OP_BUTTON, buf, w.len);
}

/* ------------------------------------------------------------------ demo */

/* A descriptor built locally, for judging layout without a host attached.
 *
 * Deliberately awkward: a long label that must not wrap, a negative value, a toggle, and a
 * field with no bounds so the UI has to draw something honest rather than inventing a range.
 * A demo made of tidy cases teaches you nothing about the layout. */
void surf_demo_descriptor(void)
{
    static const struct {
        uint16_t id; uint8_t kind; const char *label; const char *unit;
        double v, lo, hi, step; uint8_t precision; uint16_t present;
    } demo[] = {
        { 1, EMP_KIND_NUMBER, "Cutoff",      "Hz",  0.53,  0.0,   1.0, 0.01, 2,
          EMP_PRESENT_MIN | EMP_PRESENT_MAX | EMP_PRESENT_STEP | EMP_PRESENT_UNIT | EMP_PRESENT_PRECISION },
        { 2, EMP_KIND_NUMBER, "Resonance",   0,     0.21,  0.0,   1.0, 0.01, 2,
          EMP_PRESENT_MIN | EMP_PRESENT_MAX | EMP_PRESENT_STEP | EMP_PRESENT_PRECISION },
        { 3, EMP_KIND_TOGGLE, "Bypass",      0,     0.0,   0.0,   0.0, 0.0,  0, 0 },
        { 4, EMP_KIND_NUMBER, "Drive",       "dB", -6.5, -24.0,  24.0, 0.1,  1,
          EMP_PRESENT_MIN | EMP_PRESENT_MAX | EMP_PRESENT_UNIT | EMP_PRESENT_PRECISION },
        { 5, EMP_KIND_NUMBER, "Attack",      "s",   0.012, 0.0,   2.0, 0.001, 3,
          EMP_PRESENT_MIN | EMP_PRESENT_MAX | EMP_PRESENT_UNIT | EMP_PRESENT_PRECISION },
        { 6, EMP_KIND_NUMBER, "Release",     "s",   1.25,  0.0,   5.0, 0.01, 2,
          EMP_PRESENT_MIN | EMP_PRESENT_MAX | EMP_PRESENT_UNIT | EMP_PRESENT_PRECISION },
        { 7, EMP_KIND_NUMBER, "Feedback",    "%",  72.0,   0.0, 100.0, 1.0,  0,
          EMP_PRESENT_MIN | EMP_PRESENT_MAX | EMP_PRESENT_UNIT },
        { 8, EMP_KIND_NUMBER, "Modulation",  0,     0.42,  0.0,   0.0, 0.0,  3,
          EMP_PRESENT_PRECISION },          /* no bounds: the bar must not invent a range */

        /* Past the first page, so the demo exercises paging rather than only the first four
         * knobs -- the device advertises 64 fields and could reach 8. */
        { 9, EMP_KIND_CHOICE,  "Waveform",    0,     2.0,   0.0,   0.0, 0.0,  0,
          EMP_PRESENT_CHOICES },
        {10, EMP_KIND_NUMBER, "Fine Tune",  "ct",  -13.75, -100.0, 100.0, 0.01, 2,
          EMP_PRESENT_MIN | EMP_PRESENT_MAX | EMP_PRESENT_STEP | EMP_PRESENT_UNIT | EMP_PRESENT_PRECISION },
        {11, EMP_KIND_READONLY, "CPU",       "%",   37.0,   0.0, 100.0, 0.0,  0,
          EMP_PRESENT_MIN | EMP_PRESENT_MAX | EMP_PRESENT_UNIT },
        {12, EMP_KIND_TOGGLE,  "Sync",       0,     0.0,   0.0,   0.0, 0.0,  0, 0 },
    };

    /* Option labels for field 9. The whole point of storing these is that a Choice knob must
     * read "Sawtooth", not "3". */
    static const char *const waveforms[] = { "Sine", "Triangle", "Sawtooth", "Square", "Noise" };

    surf_init();
    surf_arena_t *a = LIVE;          /* built in place: nothing to verify, so nothing to stage */

    for (unsigned i = 0; i < sizeof(demo) / sizeof(demo[0]); i++) {
        surf_field_t *f = &a->fields[i];
        memset(f, 0, sizeof(*f));
        f->id        = demo[i].id;
        f->kind      = demo[i].kind;
        f->present   = demo[i].present;
        f->min       = demo[i].lo;
        f->max       = demo[i].hi;
        f->step      = demo[i].step;
        f->precision = demo[i].precision;

        if (demo[i].kind == EMP_KIND_TOGGLE) {
            f->value_tag = EMP_VAL_BOOL;
            f->boolean = (uint8_t)(i & 1);
        } else if (demo[i].kind == EMP_KIND_CHOICE) {
            f->value_tag    = EMP_VAL_CHOICE;
            f->choice       = (uint32_t)demo[i].v;
            f->choice_count = (uint16_t)(sizeof(waveforms) / sizeof(waveforms[0]));
            f->choice_first = a->choices_used;
            for (unsigned k = 0; k < f->choice_count; k++) {
                const char *o = waveforms[k];
                uint16_t on = 0; while (o[on]) on++;
                if (a->pool_used + on <= SURF_STRING_POOL && a->choices_used < SURF_MAX_CHOICES) {
                    memcpy(a->pool + a->pool_used, o, on);
                    a->choices[a->choices_used].off = a->pool_used;
                    a->choices[a->choices_used].len = on;
                    a->choices_used++;
                    a->pool_used = (uint16_t)(a->pool_used + on);
                }
            }
        } else {
            f->value_tag = EMP_VAL_NUMBER;
            f->number = demo[i].v;
        }

        const char *l = demo[i].label;
        uint16_t n = 0; while (l[n]) n++;
        if (a->pool_used + n <= SURF_STRING_POOL) {
            memcpy(a->pool + a->pool_used, l, n);
            f->label_off = a->pool_used; f->label_len = n;
            a->pool_used = (uint16_t)(a->pool_used + n);
        }
        if (demo[i].unit) {
            const char *u = demo[i].unit;
            uint16_t m = 0; while (u[m]) m++;
            if (a->pool_used + m <= SURF_STRING_POOL) {
                memcpy(a->pool + a->pool_used, u, m);
                f->unit_off = a->pool_used; f->unit_len = m;
                a->pool_used = (uint16_t)(a->pool_used + m);
            }
        }
    }
    a->field_count = sizeof(demo) / sizeof(demo[0]);
    a->revision    = 0xDE30;
}
