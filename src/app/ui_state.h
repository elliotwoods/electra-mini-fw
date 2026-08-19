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
 *   - While a number is drilled, the opposite row becomes a sliding four-place digit window;
 *     the outer buttons shift that window and pressing an editor-row knob leaves.
 *   - Choice and Colour use dedicated drilled editors on the opposite row; their outer buttons
 *     are disabled.
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
#define UI_ROW_SPLIT       4u        /* measured panel order: 0-3 bottom, 4-7 top */
#define UI_DIGITS          4u        /* knobs in whichever row is the drilled editor */

#define UI_DISCRETE_STEP   5         /* detents per step on a Toggle or Choice */
#define UI_DIGIT_STEP      4         /* detents per step of a numeric digit */
#define UI_ACTIVE_MS       180u      /* keep rotary feedback visible after the last edit */

/* Front-panel buttons, left to right. The outer pair page in overview and zoom numbers drilled. */
#define UI_BTN_BACK        0u
#define UI_BTN_UNDO        1u
#define UI_BTN_REDO        2u
#define UI_BTN_AUX         3u
#define UI_BTN_SYSTEM      4u
#define UI_BTN_PAGE_NEXT   5u
#define UI_BTN_COUNT       6u

/* Compatibility names used by older tests and call sites. */
#define UI_BTN_EXIT        UI_BTN_BACK
#define UI_BTN_RESET       UI_BTN_SYSTEM
#define UI_BTN_PAGE_PREV   UI_BTN_BACK

typedef enum {
    UI_MODE_SURFACE = 0,
    UI_MODE_SYSTEM,
    UI_MODE_REBOOT_WAIT,
    UI_MODE_CAL_SELECT,
    UI_MODE_CAL_RUN,
    UI_MODE_RESTORE_CONFIRM,
    UI_MODE_BRIGHTNESS
} ui_mode_t;

typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_REBOOT,
    UI_ACTION_CALIBRATE,
    UI_ACTION_CAL_CANCEL,
    UI_ACTION_CAL_RETRY,
    UI_ACTION_CAL_DONE,
    UI_ACTION_RESTORE_DEFAULTS,
    UI_ACTION_BRIGHTNESS_PREVIEW,
    UI_ACTION_BRIGHTNESS_SAVE,
    UI_ACTION_BRIGHTNESS_CANCEL
} ui_action_kind_t;

typedef struct {
    ui_action_kind_t kind;
    uint8_t pot_mask;
    uint8_t brightness_percent;
} ui_action_t;

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

    int8_t   active_pot;      /* last surface pot that changed a value, or -1 */
    uint32_t active_until_ms; /* short visual latch; expiry is wrap-safe in ui_state_tick */

    uint16_t touch_mask;     /* live, 8 bits, panel order -- highlighting only */
    uint16_t press_mask;     /* live pot-switch state, for the pressed affordance */
    uint16_t btn_mask;       /* live front-panel button state, likewise */
    ui_mode_t mode;
    uint8_t system_selection;/* 0 touch calibration, 1 restore defaults, 2 brightness */
    uint8_t brightness_saved;
    uint8_t brightness_preview;
    uint8_t brightness_save_error;
    uint8_t cal_phase;       /* mirror of touch_cal_phase_t for rendering */
    uint8_t cal_pot;
    uint8_t cal_cycle;
    uint8_t cal_valid_mask;
    uint8_t cal_custom;
    uint16_t cal_clear_ms;
    const char *cal_message;
    uint32_t uptime_ms;
    int touch_status;
} ui_state_t;

void ui_state_init(void);
const ui_state_t *ui_state(void);

/* Physical input. `pot` is in measured panel order: 0-3 bottom row, 4-7 top row. */
void ui_state_touch(uint16_t touch_mask, uint32_t now_ms);
void ui_state_push(unsigned pot, int pressed, uint32_t now_ms);
void ui_state_button(unsigned button, int pressed, uint32_t now_ms);
int  ui_state_rotate(unsigned pot, int32_t detents, uint32_t now_ms);
int  ui_state_take_action(ui_action_t *out);
void ui_state_calibration_status(uint8_t phase, uint8_t pot, uint8_t cycle,
                                 uint8_t valid_mask, uint8_t custom, uint16_t clear_ms,
                                 const char *message);
void ui_state_system_status(uint32_t uptime_ms, int touch_status);
void ui_state_brightness_status(uint8_t saved_percent);
void ui_state_brightness_save_result(int success);
void ui_state_surface_changed(void);
void ui_state_surface_cleared(void);
/* Reveal the stable page containing an occupied absolute slot. */
int ui_state_reveal(uint16_t slot);

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
