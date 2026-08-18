/* Interaction state for the control surface: drill-down, the digit window, paging, history.
 *
 * Deliberately separate from ui.c, which draws. This half is pure logic with no hardware in it,
 * so the ergonomics that are hard to get right can be unit-tested on a workstation rather than
 * debugged with a finger on a knob.
 *
 * The model:
 *
 *   - The overview shows ALL EIGHT fields, one per knob, in two rows mirroring the panel.
 *   - Pressing a knob's own switch drills into that field. The control you press is the control
 *     you edit, so there is no separate notion of a selected field to display or move.
 *   - While drilled, the TOP row becomes a sliding four-decimal-place window over the value and
 *     the bottom row keeps editing its own fields. Pressing any top knob leaves.
 *
 * Drill state is deliberately MECHANICAL rather than capacitive. The previous model held the
 * drilled view for as long as a finger rested on a knob, and the sensor cannot support that: a
 * 90-second capture recorded it dropping detect 17 times out of 19 releases with a finger
 * plainly still down, for as long as 272 ms at a time. Covering that needed a 300 ms release
 * debounce, which made every drill-out lag by design. A switch simply closes.
 *
 * Touch keeps the one job it is good at -- highlighting which digit a finger is resting on --
 * where a dropped reading costs a moment of highlight and nothing else.
 */

#ifndef ELECTRA_UI_STATE_H
#define ELECTRA_UI_STATE_H

#include <stdint.h>
#include "surface.h"

/* Two numbers that were one, and had to be separated before the page could grow.
 *
 * UI_FIELDS_PER_PAGE used to mean both "how many fields a page holds" and "the knob index where
 * the bottom row ends and the top row begins". Those were equal only by coincidence of a 4+4
 * layout. Setting the page size to 8 with them still merged makes `pot >= UI_FIELDS_PER_PAGE`
 * never true, so every knob routes to the bottom-row branch and the entire digit window becomes
 * unreachable code that still compiles. */
#define UI_FIELDS_PER_PAGE 8u        /* fields visible at once: one per knob */
#define UI_ROW_SPLIT       4u        /* knobs 0-3 are the bottom row, 4-7 the top */
#define UI_DIGITS          4u        /* the top row, when drilled */

#define UI_DISCRETE_STEP   5         /* detents per step on a Toggle or Choice */
#define UI_DIGIT_STEP      4         /* detents per step of a digit, on the top row */

/* Front-panel buttons, left to right. */
#define UI_BTN_EXIT        0u
#define UI_BTN_UNDO        1u
#define UI_BTN_REDO        2u
#define UI_BTN_RESET       3u
#define UI_BTN_PAGE_PREV   4u
#define UI_BTN_PAGE_NEXT   5u
#define UI_BTN_COUNT       6u

typedef struct {
    int32_t  focused;        /* absolute field index being edited, or -1 for the overview */
    uint16_t page;           /* absolute index of the first field on the visible page */

    int8_t   ws;             /* digit window: exponent of its leftmost place */
    int8_t   held_digit;     /* which digit knob a finger is on (0..3), or -1 */

    /* Which row is currently the digit editor: 1 = the top row, 0 = the bottom row.
     *
     * It is always the row OPPOSITE the drilled field. Drill into something on the bottom row
     * and the top row becomes the digit window; drill into the top row and the bottom row does.
     * That keeps the readout on the same side as the knobs editing it, so the link lines stay
     * short and point at the hand that is actually working -- and it leaves the row containing
     * the drilled field showing its cells, so you can still see it in context. */
    uint8_t  digit_top;

    uint16_t touch_mask;     /* live, 8 bits, panel order -- highlighting only */
    uint16_t press_mask;     /* live pot-switch state, for the pressed affordance */
    uint16_t btn_mask;       /* live front-panel button state, likewise */
} ui_state_t;

void ui_state_init(void);
const ui_state_t *ui_state(void);

/* Physical input. `pot` is in panel order: 0-3 bottom row, 4-7 top row. */
void ui_state_touch(uint16_t touch_mask, uint32_t now_ms);
void ui_state_push(unsigned pot, int pressed, uint32_t now_ms);
void ui_state_button(unsigned button, int pressed, uint32_t now_ms);
int  ui_state_rotate(unsigned pot, int32_t detents, uint32_t now_ms);

/* Called every service pass; runs the history settle timer. */
void ui_state_tick(uint32_t now_ms);

/* Which absolute field a knob addresses, or -1 past the end of a short last page. */
int32_t ui_state_field_for(unsigned knob);

/* First knob index of the row currently acting as the digit editor, and of the row still
 * showing cells. Both are UI_DIGITS wide. Meaningless unless something is drilled. */
unsigned ui_digit_row_first(void);
unsigned ui_cell_row_first(void);

/* Would this button do anything right now? Drives the greyed-out affordances, so a control that
 * cannot act looks like it rather than silently ignoring a press. */
int ui_state_button_enabled(unsigned button);

/* --- digit-window arithmetic, exposed because the renderer needs the same answers --- */

int8_t  ui_msd(double v);
int8_t  ui_min_ws(uint8_t precision);
int8_t  ui_clamp_ws(int8_t ws, double v, uint8_t precision);
uint8_t ui_digit_at(double v, int8_t e, uint8_t precision);
double  ui_roundp(double v, uint8_t precision);

#endif /* ELECTRA_UI_STATE_H */
