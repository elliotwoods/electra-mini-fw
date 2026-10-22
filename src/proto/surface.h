/* EMP/1 surface channel: the descriptor the host sends, and the edits the device sends back.
 *
 * This is where the protocol stops being plumbing. The host describes a set of controls; the
 * device stores them, maps them onto physical knobs, and reports what the user does.
 *
 * Storage is fixed and bounded — no allocation anywhere. A descriptor that does not fit is
 * rejected with a reason, which is far better than a device that accepts more than it can hold
 * and then behaves strangely.
 */

#ifndef ELECTRA_EMP_SURFACE_H
#define ELECTRA_EMP_SURFACE_H

#include <stdint.h>
#include "emp.h"

#define SURF_MAX_FIELDS   64u
#define SURF_STRING_POOL  2048u

/* Choice labels, pooled across all fields rather than reserved per field.
 *
 * A Choice field is useless without them: the descriptor carries the option strings and the
 * device was throwing them away, so a knob assigned to "Waveform" displayed "3" instead of
 * "Sawtooth". Pooling means one enum with forty options costs what it costs instead of every
 * field reserving forty slots it will never use. */
#define SURF_MAX_CHOICES  256u
#define SURF_POTS         8u          /* physical knobs, i.e. fields visible at once */
/* A field's retained Text value, NUL included. 96 until EMP/1.8, whose LIST readouts carry
 * several lines in one value; longer text is cut on a character boundary and reported. */
#define SURF_TEXT_MAX     256u

typedef struct {
    uint16_t id;
    uint8_t  kind;
    uint8_t  flags;
    uint16_t present;
    uint8_t  precision;
    uint8_t  lane;
    uint16_t choice_count;

    double   min, max, step;

    char text[SURF_TEXT_MAX]; /* bounded UTF-8 readout, independent of descriptor label pool */
    uint8_t  value_tag;
    double   number;
    uint32_t choice;
    uint8_t  boolean;
    uint8_t  color_count;
    float    color[4];

    uint16_t label_off, label_len;
    uint16_t unit_off,  unit_len;

    /* Index into the shared choice table; `choice_count` entries from here. */
    uint16_t choice_first;

    /* The declared default, present only when EMP_PRESENT_DEFAULT is set. The descriptor has
     * always carried this and the device has always discarded it, which is why there was no
     * Reset: you cannot restore a value you never stored. */
    uint8_t  default_tag;
    uint8_t  default_boolean;
    uint32_t default_choice;
    double   default_number;
    uint8_t  default_color_count;
    float    default_color[4];

    /* EMP/1.6. Both zero (normal, neutral) unless the descriptor or TONED_VALUES says otherwise. */
    uint8_t  style;
    uint8_t  tone;
} surf_field_t;

void surf_init(void);
int surf_operator_layout(void);
/* EMP/1.6: an Action declared with the estop style. */
int surf_field_is_estop(const surf_field_t *f);
/* EMP/1.7: an Action declared with the hold style (fires after EMP_HOLD_MS held, not on press). */
int surf_field_is_hold(const surf_field_t *f);
/* EMP/1.8: a ReadOnly field drawn as a scrolling key/value list. */
int surf_field_is_list(const surf_field_t *f);
/* EMP/1.8: an Action declared DISABLED (neither fires nor holds). */
int surf_field_disabled(const surf_field_t *f);
/* EMP/1.8: declared JOINED to the cell on its left (the renderer decides whether it can be). */
int surf_field_joined(const surf_field_t *f);

/* Handle one surface-channel message. Returns nothing: per the design rule, protocol problems
 * become DIAG and DESC_REQUEST on the wire, never an error returned to a caller. */
void surf_handle(uint8_t opcode, const uint8_t *msg, uint32_t len);

uint64_t surf_applied_revision(void);
/* Number of descriptor records, distinct from the occupied physical slot span. */
uint16_t surf_field_count(void);
uint16_t surf_slot_span(void);
/* Lookup by absolute physical slot. Returns NULL for a hole. */
const surf_field_t *surf_field(uint16_t slot);
const char *surf_string(uint16_t off, uint16_t len);

/* Label for one option of a Choice field. Returns NULL and leaves *len untouched when the
 * descriptor did not carry labels, which is legal -- the caller then falls back to the index. */
const char *surf_choice_label(const surf_field_t *f, uint16_t option, uint16_t *len);

/* Which page of fields the knobs currently show. */
uint16_t surf_page(void);
void surf_set_page(uint16_t first_index);

/* Physical input -> protocol. Called from the input layer.
 *
 * `delta` is in detents. Whether that becomes an absolute EDIT or a relative EDIT_DELTA is
 * decided per field from what the host declared, which is the whole point of carrying min/max/
 * step/precision: the device is locally authoritative wherever it has enough information to
 * be, and the USB round trip stays out of the felt latency of a gesture. */
/* Apply a value locally and tell the host. The decision about WHAT a knob turn means -- which
 * decimal place, which step size, whether it is a discrete option -- belongs to ui_state.c,
 * which owns the interaction model; this layer's job is to hold the value and put it on the
 * wire with the right revision stamp.
 *
 * That split replaced a single surf_on_rotate() that did both, and whose presence-mask tests
 * ran before its kind tests -- so a Choice field declaring only `choice_count` fell through to
 * the "host must integrate this" branch and never reached the code written to handle it. */
void surf_set_number(uint16_t index, double v);
void surf_set_bool(uint16_t index, uint8_t v);
void surf_set_choice(uint16_t index, uint32_t v);
void surf_set_color(uint16_t index, const float *v, uint8_t count);

/* The same, carrying an explicit cause. A restore or an undo is not a knob turn, and the host
 * has to be able to tell them apart -- otherwise a history step looks like the user sweeping a
 * control, which is exactly the thing echo suppression is trying to reason about. */
void surf_set_number_cause(uint16_t index, double v, uint8_t cause);
void surf_set_bool_cause(uint16_t index, uint8_t v, uint8_t cause);
void surf_set_choice_cause(uint16_t index, uint32_t v, uint8_t cause);
void surf_set_color_cause(uint16_t index, const float *v, uint8_t count, uint8_t cause);

/* No bounds, no step, no precision: nothing to integrate against, so the host must do it. */
void surf_send_delta(uint16_t index, int32_t detents);

/* Focus, reported per lane. `index` < 0 means nothing is focused. */
void surf_send_focus(int32_t index, uint16_t touch_mask);
void surf_activate(uint16_t slot);
/* The field has a declared default and its value differs from it: what the screen marks with
 * its reset/store column, and what enables Reset and Store here. */
int surf_field_modified(const surf_field_t *f);
/* EMP/1.5 hosts accept SAVE_DEFAULT; older ones have nowhere to store it. */
int surf_can_save_default(void);
/* Store the field's current value as its default: locally at once, and on the host. */
void surf_save_default(uint16_t slot);
void surf_on_push(unsigned pot, int pressed);
void surf_on_touch(uint16_t touch_mask);
void surf_on_button(unsigned button, int pressed);

/* A locally-built descriptor for judging layout with no host attached. Deliberately awkward:
 * a long label, a negative value, a toggle, and a field with no bounds. */
void surf_demo_descriptor(void);
void surf_demo_operator(void);
/* A single Choice field used by simulator layout scenes. Kept separate so the ordinary demo
 * descriptor remains stable while 3/5/7-slot and mirrored drill layouts are exercised. */
void surf_demo_choice_descriptor(uint16_t slot, const char *field_label,
                                 const char *const *labels, uint16_t count,
                                 uint16_t selected);

#endif /* ELECTRA_EMP_SURFACE_H */
