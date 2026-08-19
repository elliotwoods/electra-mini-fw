/* Per-field undo/redo.
 *
 * "Undo" on a control surface is not the same idea as undo in an editor. A knob emits dozens of
 * values per second while it is being turned, and stepping back through those one at a time
 * would be useless. What a hand actually does is make an ADJUSTMENT -- a continuous twiddle,
 * however long it lasts -- and it is adjustments that want undoing.
 *
 * So a change is committed to history once the value has been STILL for a moment, or when you
 * leave the field. One twiddle is one step, whether it took a quarter turn or ten seconds.
 *
 * Deliberately pure logic with no I/O, like ui_state.c, so the commit rule can be tested on a
 * workstation instead of by turning a knob and hoping.
 *
 * The ring is per field and local to the device for now. Later this is meant to be assisted by
 * the host, which knows things the device cannot -- what a parameter means, whether two changes
 * belong to one gesture, what a preset boundary is. The commit rule is one function and callers
 * only ever go through this interface, so that swap does not reach into anything else.
 */

#ifndef ELECTRA_HISTORY_H
#define ELECTRA_HISTORY_H

#include <stdint.h>
#include "surface.h"

/* Entries per field. Eight is enough to walk back through a session of adjusting one control
 * without turning the whole descriptor into a memory problem: 64 fields x 8 x 16 bytes is 8 KB. */
#define HIST_DEPTH 8u

/* How long a value must sit unchanged before the adjustment counts as finished. Short enough
 * that a pause mid-gesture is not mistaken for the end of one, long enough that it does not fire
 * between two detents of a slow deliberate turn. */
#define HIST_SETTLE_MS 400u

typedef struct {
    uint8_t  tag;            /* EMP_VAL_* */
    uint8_t  boolean;
    uint32_t choice;
    double   number;
    uint8_t  color_count;
    float    color[4];
} hist_value_t;

void hist_init(void);

/* Called whenever a field's value changes, from any cause. Does not commit -- it starts or
 * restarts the settle timer for that field. */
void hist_touched(uint16_t field, uint32_t now_ms);

/* Called every service pass. Commits any field whose settle timer has expired. */
void hist_tick(uint32_t now_ms);

/* Commit a field immediately, whatever its timer says. Use when leaving a field or changing
 * page: the adjustment is over because the user has moved on. */
void hist_commit(uint16_t field);

/* Step back / forward. Returns non-zero and fills `out` when there is somewhere to go.
 *
 * Both commit any pending adjustment first, so that stepping back from a value you have just
 * changed returns to where you were rather than to where you were two changes ago -- which is
 * the single most annoying way to get undo wrong. */
int hist_undo(uint16_t field, hist_value_t *out);
int hist_redo(uint16_t field, hist_value_t *out);

/* For drawing affordances: a button that would do nothing should look like it. */
int hist_can_undo(uint16_t field);
int hist_can_redo(uint16_t field);

#endif /* ELECTRA_HISTORY_H */
