/* Per-field undo/redo. See history.h for why an adjustment, not a value, is the unit. */

#include <string.h>
#include "history.h"

typedef struct {
    hist_value_t ring[HIST_DEPTH];
    uint8_t      count;          /* entries held, 0..HIST_DEPTH */
    uint8_t      pos;            /* where we are in the ring; count-1 is "the present" */
    uint8_t      pending;        /* a change is in flight and not yet committed */
    uint32_t     due_ms;         /* when that change settles */
} field_hist_t;

static field_hist_t h[SURF_MAX_FIELDS];

void hist_init(void) { memset(h, 0, sizeof(h)); }

static void snapshot(const surf_field_t *f, hist_value_t *v)
{
    v->tag     = f->value_tag;
    v->boolean = f->boolean;
    v->choice  = f->choice;
    v->number  = f->number;
    v->color_count = f->color_count;
    memcpy(v->color, f->color, sizeof(v->color));
}

static int same(const hist_value_t *a, const hist_value_t *b)
{
    if (a->tag != b->tag) return 0;
    switch (a->tag) {
    case EMP_VAL_BOOL:   return a->boolean == b->boolean;
    case EMP_VAL_CHOICE: return a->choice  == b->choice;
    case EMP_VAL_COLOR:
        return a->color_count == b->color_count
            && memcmp(a->color, b->color, sizeof(a->color)) == 0;
    default:             return a->number  == b->number;
    }
}

/* Append the field's CURRENT value as a new entry.
 *
 * Anything ahead of the cursor is discarded, which is the ordinary rule: once you undo and then
 * make a fresh change, the branch you abandoned is gone. Keeping it would need a tree, and a
 * tree is not something anyone wants to navigate with four knobs. */
static void push_entry(uint16_t field)
{
    const surf_field_t *f = surf_field(field);
    if (!f || field >= SURF_MAX_FIELDS) return;

    field_hist_t *fh = &h[field];
    hist_value_t now;
    snapshot(f, &now);

    if (fh->count && same(&fh->ring[fh->pos], &now)) return;   /* nothing actually changed */

    /* Drop the redo branch, then append. */
    fh->count = (uint8_t)(fh->pos + 1u);

    if (fh->count >= HIST_DEPTH) {
        /* Full: slide the window, losing the oldest. A ring with a cursor and a discarded
         * branch is simpler to reason about as a shifted array than as modular arithmetic,
         * and eight entries is far too few for the memmove to matter. */
        memmove(&fh->ring[0], &fh->ring[1], sizeof(fh->ring[0]) * (HIST_DEPTH - 1u));
        fh->count = HIST_DEPTH;
        fh->pos   = (uint8_t)(HIST_DEPTH - 1u);
    } else {
        fh->pos   = fh->count;
        fh->count = (uint8_t)(fh->count + 1u);
    }
    fh->ring[fh->pos] = now;
}

void hist_touched(uint16_t field, uint32_t now_ms)
{
    if (field >= SURF_MAX_FIELDS) return;
    field_hist_t *fh = &h[field];

    /* The very first change to a field records where it STARTED, so the first undo has
     * somewhere to return to. Without this the earliest adjustment is unrecoverable, which is
     * exactly the one most likely to have been a mistake. */
    if (!fh->count) push_entry(field);

    fh->pending = 1;
    fh->due_ms  = now_ms + HIST_SETTLE_MS;
}

void hist_commit(uint16_t field)
{
    if (field >= SURF_MAX_FIELDS) return;
    field_hist_t *fh = &h[field];
    if (!fh->pending) return;
    fh->pending = 0;
    push_entry(field);
}

void hist_tick(uint32_t now_ms)
{
    uint16_t n = surf_slot_span();
    if (n > SURF_MAX_FIELDS) n = SURF_MAX_FIELDS;

    for (uint16_t i = 0; i < n; i++) {
        if (h[i].pending && (int32_t)(now_ms - h[i].due_ms) >= 0) hist_commit(i);
    }
}

int hist_can_undo(uint16_t field)
{
    if (field >= SURF_MAX_FIELDS) return 0;
    return h[field].pending || h[field].pos > 0;
}

int hist_can_redo(uint16_t field)
{
    if (field >= SURF_MAX_FIELDS) return 0;
    return h[field].count && (h[field].pos + 1u) < h[field].count;
}

int hist_undo(uint16_t field, hist_value_t *out)
{
    if (field >= SURF_MAX_FIELDS || !out) return 0;
    field_hist_t *fh = &h[field];

    /* Commit first. Undoing straight after a change should return to where you were, not to
     * where you were two changes ago -- getting that wrong is the classic way undo feels
     * broken, because the step you most want back is the one silently skipped. */
    hist_commit(field);

    if (fh->pos == 0) return 0;
    fh->pos--;
    *out = fh->ring[fh->pos];
    return 1;
}

int hist_redo(uint16_t field, hist_value_t *out)
{
    if (field >= SURF_MAX_FIELDS || !out) return 0;
    field_hist_t *fh = &h[field];

    hist_commit(field);

    if (!fh->count || (fh->pos + 1u) >= fh->count) return 0;
    fh->pos++;
    *out = fh->ring[fh->pos];
    return 1;
}
