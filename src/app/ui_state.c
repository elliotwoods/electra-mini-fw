/* Interaction state: drill-down, the digit window, paging, history. See ui_state.h. */

#include <string.h>
#include "ui_state.h"
#include "history.h"

static ui_state_t st;
static uint16_t   prev_touch;
static int32_t    discrete_accum[UI_FIELDS_PER_PAGE];
static int32_t    digit_accum[UI_DIGITS];
static ui_action_t pending_action;

/* ------------------------------------------------------------------ digit arithmetic */

static double pow10i(int n)
{
    double r = 1.0;
    if (n >= 0) { while (n--) r *= 10.0; }
    else        { while (n++) r /= 10.0; }
    return r;
}

int8_t ui_msd(double v)
{
    if (v < 0) v = -v;

    /* Counted rather than computed with log10: this runs on a core with software doubles and no
     * libm worth linking, and the answer is wanted as an exact integer place rather than a float
     * that might land a hair under the boundary at 1000. */
    int8_t e = 0;
    while (v >= 10.0 && e < 18) { v /= 10.0; e++; }
    return e;                          /* floored at 0 by construction: |v| < 1 gives 0 */
}

int8_t ui_min_ws(uint8_t precision)
{
    return (int8_t)((int)UI_DIGITS - 1 - (int)precision);
}

int8_t ui_clamp_ws(int8_t ws, double v, uint8_t precision)
{
    int8_t lo = ui_min_ws(precision);
    int8_t hi = ui_msd(v);
    if (hi < lo) hi = lo;
    if (ws < lo) ws = lo;
    if (ws > hi) ws = hi;
    return ws;
}

double ui_roundp(double v, uint8_t precision)
{
    double scale = pow10i(precision);
    double s = v * scale;
    /* Half away from zero, so a knob feels the same either side of zero. */
    s = (s >= 0.0) ? (double)(long long)(s + 0.5) : (double)(long long)(s - 0.5);
    return s / scale;
}

uint8_t ui_digit_at(double v, int8_t e, uint8_t precision)
{
    if (v < 0) v = -v;
    if (e < -(int8_t)precision) return 0;

    /* Scaled integer. floor(|v| / 10^e) on a double starts returning the digit below at three
     * decimals, which is exactly where a control surface lives. */
    double scaled = v * pow10i(precision) + 0.5;
    if (scaled < 0.0) scaled = 0.0;
    if (scaled > 9.0e15) return 0;
    unsigned long long fixed = (unsigned long long)scaled;

    int shift = (int)e + (int)precision;
    while (shift-- > 0) fixed /= 10ull;
    return (uint8_t)(fixed % 10ull);
}

/* ------------------------------------------------------------------ helpers */

static uint8_t precision_of(const surf_field_t *f)
{
    return (uint8_t)((f->present & EMP_PRESENT_PRECISION) ? f->precision : 0);
}

static void rebuild_window(void)
{
    const surf_field_t *f = (st.focused >= 0) ? surf_field((uint16_t)st.focused) : 0;
    if (!f) { st.ws = 0; return; }
    st.ws = ui_clamp_ws(ui_msd(f->number), f->number, precision_of(f));
}

/* Leaving a field ends the adjustment in progress, so history commits now rather than waiting
 * out its settle timer. The user has demonstrably moved on. */
static void leave_focus(void)
{
    if (st.focused < 0) return;
    hist_commit((uint16_t)st.focused);
    surf_send_focus(-1, 0);
    st.focused = -1;
    memset(digit_accum, 0, sizeof(digit_accum));
}

static void enter_focus(int32_t idx)
{
    if (idx < 0) return;
    st.focused = idx;

    /* The digit editor is the OTHER row. See ui_state.h: this is what makes drilling symmetric,
     * so a knob on either row behaves the same way and the readout always appears beside the
     * knobs that edit it rather than always at the top. */
    st.digit_top = (uint8_t)(((uint32_t)idx - st.page) < UI_ROW_SPLIT);
    memset(digit_accum, 0, sizeof(digit_accum));
    rebuild_window();
    surf_send_focus(idx, st.touch_mask);
}

void ui_state_init(void)
{
    memset(&st, 0, sizeof(st));
    st.focused    = -1;
    st.held_digit = -1;
    st.active_pot = -1;
    prev_touch    = 0;
    memset(discrete_accum, 0, sizeof(discrete_accum));
    memset(digit_accum, 0, sizeof(digit_accum));
    hist_init();
    st.mode = UI_MODE_SURFACE;
    st.brightness_saved = 100u;
    st.brightness_preview = 100u;
    st.cal_message = "Ready";
    pending_action.kind = UI_ACTION_NONE;
}

const ui_state_t *ui_state(void) { return &st; }

unsigned ui_digit_row_first(void) { return st.digit_top ? UI_ROW_SPLIT : 0u; }
unsigned ui_cell_row_first(void)  { return st.digit_top ? 0u : UI_ROW_SPLIT; }

int32_t ui_state_field_for(unsigned knob)
{
    if (knob >= UI_FIELDS_PER_PAGE) return -1;
    uint32_t idx = (uint32_t)st.page + knob;
    return (idx < SURF_MAX_FIELDS && surf_field((uint16_t)idx)) ? (int32_t)idx : -1;
}

static uint16_t last_page_start(void)
{
    uint16_t n = surf_slot_span();
    if (!n) return 0;
    return (uint16_t)(((n - 1u) / UI_FIELDS_PER_PAGE) * UI_FIELDS_PER_PAGE);
}

/* Pages are stable absolute groups, so no field ever appears on two different pages and a short
 * last page stays short rather than sliding backwards to stay full. */
static void set_page(int32_t start)
{
    if (start < 0) start = 0;
    if (start > (int32_t)last_page_start()) start = (int32_t)last_page_start();
    if ((uint16_t)start == st.page) return;

    leave_focus();                    /* the focused field may not be on the new page */
    st.page = (uint16_t)start;
    memset(discrete_accum, 0, sizeof(discrete_accum));
    surf_set_page(st.page);
}

void ui_state_surface_changed(void)
{
    leave_focus();
    st.page = 0;
    hist_init();
    memset(discrete_accum, 0, sizeof(discrete_accum));
    memset(digit_accum, 0, sizeof(digit_accum));
    surf_set_page(0);
}

void ui_state_surface_cleared(void)
{
    ui_state_init();
    surf_set_page(0);
}

int ui_state_reveal(uint16_t slot)
{
    if (!surf_field(slot)) return 0;
    leave_focus();
    st.mode = UI_MODE_SURFACE;
    uint16_t target = (uint16_t)((slot / UI_FIELDS_PER_PAGE) * UI_FIELDS_PER_PAGE);
    if (st.page == target) surf_set_page(target);
    else set_page(target);
    return 1;
}

/* ------------------------------------------------------------------ touch */

void ui_state_touch(uint16_t touch_mask, uint32_t now_ms)
{
    (void)now_ms;
    st.touch_mask = touch_mask;

    uint16_t changed = (uint16_t)(touch_mask ^ prev_touch);
    prev_touch = touch_mask;
    if (!changed) return;

    if (st.focused >= 0) {
        const surf_field_t *f = surf_field((uint16_t)st.focused);
        if (f && f->kind == EMP_KIND_COLOR) surf_send_focus(st.focused, touch_mask);
    }

    /* Touch does exactly one thing now: say which digit a finger rests on, so the readout can
     * emphasise it. Nothing latches on it and nothing navigates by it, which is precisely why
     * its unreliability stopped mattering. */
    unsigned base = ui_digit_row_first();
    for (unsigned p = base; p < base + UI_DIGITS; p++) {
        uint16_t bit = (uint16_t)(1u << p);
        if (!(changed & bit)) continue;
        unsigned k = p - base;
        if (touch_mask & bit)                st.held_digit = (int8_t)k;
        else if (st.held_digit == (int8_t)k) st.held_digit = -1;
    }
}

/* ------------------------------------------------------------------ press */

void ui_state_push(unsigned pot, int pressed, uint32_t now_ms)
{
    (void)now_ms;
    if (pot >= UI_FIELDS_PER_PAGE) return;

    uint16_t bit = (uint16_t)(1u << pot);
    if (pressed) st.press_mask |= bit;
    else         st.press_mask = (uint16_t)(st.press_mask & ~bit);

    if (!pressed) return;               /* act on the press edge; release is only an affordance */

    if (st.mode == UI_MODE_SYSTEM) {
        if (st.system_selection == 0u) st.mode = UI_MODE_CAL_SELECT;
        else if (st.system_selection == 1u) st.mode = UI_MODE_RESTORE_CONFIRM;
        else {
            st.brightness_preview = st.brightness_saved;
            st.brightness_save_error = 0;
            st.mode = UI_MODE_BRIGHTNESS;
        }
        return;
    }
    if (st.mode == UI_MODE_CAL_SELECT) {
        pending_action.kind = UI_ACTION_CALIBRATE;
        pending_action.pot_mask = (uint8_t)(1u << pot);
        st.mode = UI_MODE_CAL_RUN;
        return;
    }
    if (st.mode != UI_MODE_SURFACE) return;

    if (st.focused >= 0) {
        /* While drilled, the digit row's knobs are digit controls, so pressing one means "done
         * here". The other row still addresses its own fields, so pressing one of those moves
         * to that field instead of leaving, which is what you want when comparing two. */
        unsigned dbase = ui_digit_row_first();
        if (pot >= dbase && pot < dbase + UI_DIGITS) { leave_focus(); return; }

        int32_t idx = ui_state_field_for(pot);
        int32_t was = st.focused;
        leave_focus();
        if (idx >= 0 && idx != was) enter_focus(idx);
        return;
    }

    enter_focus(ui_state_field_for(pot));
}

/* ------------------------------------------------------------------ buttons */

static void apply_value(uint16_t idx, const hist_value_t *v, uint8_t cause)
{
    switch (v->tag) {
    case EMP_VAL_BOOL:   surf_set_bool_cause(idx, v->boolean, cause);  break;
    case EMP_VAL_CHOICE: surf_set_choice_cause(idx, v->choice, cause); break;
    case EMP_VAL_COLOR:  surf_set_color_cause(idx, v->color, v->color_count, cause); break;
    default:             surf_set_number_cause(idx, v->number, cause); break;
    }
}

int ui_state_button_enabled(unsigned button)
{
    if (st.mode == UI_MODE_REBOOT_WAIT) return button == UI_BTN_SYSTEM;
    if (st.mode == UI_MODE_SYSTEM) return button == UI_BTN_BACK || button == UI_BTN_SYSTEM;
    if (st.mode == UI_MODE_CAL_SELECT)
        return button == UI_BTN_BACK || button == UI_BTN_SYSTEM;
    if (st.mode == UI_MODE_CAL_RUN)
        return (button == UI_BTN_BACK && st.cal_phase != 6u)
            || (button == UI_BTN_SYSTEM && (st.cal_phase == 6u || st.cal_phase == 7u));
    if (st.mode == UI_MODE_RESTORE_CONFIRM)
        return button == UI_BTN_BACK || button == UI_BTN_SYSTEM;
    if (st.mode == UI_MODE_BRIGHTNESS)
        return button == UI_BTN_BACK || button == UI_BTN_SYSTEM;

    int32_t f = st.focused;
    const surf_field_t *fd = f >= 0 ? surf_field((uint16_t)f) : 0;
    switch (button) {
    case UI_BTN_BACK:
        if (f >= 0) return fd && fd->kind == EMP_KIND_NUMBER
                          && st.ws < ui_msd(fd->number);
        return st.page > 0;
    case UI_BTN_UNDO: return f >= 0 && hist_can_undo((uint16_t)f);
    case UI_BTN_REDO: return f >= 0 && hist_can_redo((uint16_t)f);
    case UI_BTN_SYSTEM: {
        if (f < 0) return 1;
        return fd && (fd->present & EMP_PRESENT_DEFAULT) && fd->kind != EMP_KIND_READONLY;
    }
    case UI_BTN_PAGE_NEXT:
        if (f >= 0) return fd && fd->kind == EMP_KIND_NUMBER
                          && st.ws > ui_min_ws(precision_of(fd));
        return st.page < last_page_start();
    default: return 0;
    }
}

void ui_state_button(unsigned button, int pressed, uint32_t now_ms)
{
    if (button >= UI_BTN_COUNT) return;

    /* Tracked whether or not the button does anything, because the legend shows the press. A
     * control that visibly responds to being pressed and then declines to act reads as
     * "not now"; one that does nothing at all reads as broken. */
    uint16_t bit = (uint16_t)(1u << button);
    if (pressed) st.btn_mask |= bit;
    else         st.btn_mask = (uint16_t)(st.btn_mask & ~bit);

    if (!pressed) {
        /* Reboot is release-gated. Resetting on the press edge carries the still-held physical
         * Button 5 into the lower boot stage, where that same button is intentionally the USB
         * Disk Mode chord. Waiting for the debounced release makes an ordinary reboot remain
         * ordinary, and UI_MODE_REBOOT_WAIT gives the held interval visible feedback. */
        if (button == UI_BTN_SYSTEM && st.mode == UI_MODE_REBOOT_WAIT)
            pending_action.kind = UI_ACTION_REBOOT;
        return;
    }
    if (!ui_state_button_enabled(button)) return;   /* the greyed affordance already said so */

    if (st.mode != UI_MODE_SURFACE) {
        if (st.mode == UI_MODE_REBOOT_WAIT) return;
        if (button == UI_BTN_BACK) {
            if (st.mode == UI_MODE_SYSTEM) st.mode = UI_MODE_SURFACE;
            else if (st.mode == UI_MODE_BRIGHTNESS) {
                pending_action.kind = UI_ACTION_BRIGHTNESS_CANCEL;
                pending_action.brightness_percent = st.brightness_saved;
                st.brightness_preview = st.brightness_saved;
                st.brightness_save_error = 0;
                st.mode = UI_MODE_SYSTEM;
            }
            else if (st.mode == UI_MODE_CAL_RUN) {
                pending_action.kind = UI_ACTION_CAL_CANCEL;
                st.mode = UI_MODE_CAL_SELECT;
            } else st.mode = UI_MODE_SYSTEM;
            return;
        }
        if (button == UI_BTN_SYSTEM) {
            if (st.mode == UI_MODE_SYSTEM) st.mode = UI_MODE_REBOOT_WAIT;
            else if (st.mode == UI_MODE_BRIGHTNESS) {
                pending_action.kind = UI_ACTION_BRIGHTNESS_SAVE;
                pending_action.brightness_percent = st.brightness_preview;
                st.brightness_save_error = 0;
            }
            else if (st.mode == UI_MODE_CAL_SELECT) {
                pending_action.kind = UI_ACTION_CALIBRATE;
                pending_action.pot_mask = 0xFFu;
                st.mode = UI_MODE_CAL_RUN;
            } else if (st.mode == UI_MODE_RESTORE_CONFIRM) {
                pending_action.kind = UI_ACTION_RESTORE_DEFAULTS;
                st.mode = UI_MODE_SYSTEM;
            } else if (st.mode == UI_MODE_CAL_RUN && st.cal_phase == 7u) {
                pending_action.kind = UI_ACTION_CAL_RETRY;
            } else if (st.mode == UI_MODE_CAL_RUN && st.cal_phase == 6u) {
                pending_action.kind = UI_ACTION_CAL_DONE;
                st.mode = UI_MODE_CAL_SELECT;
            }
        }
        return;
    }

    switch (button) {
    case UI_BTN_BACK:
        if (st.focused >= 0) { st.ws++; memset(digit_accum, 0, sizeof(digit_accum)); }
        else set_page((int32_t)st.page - (int32_t)UI_FIELDS_PER_PAGE);
        break;

    case UI_BTN_UNDO: {
        hist_value_t v;
        if (hist_undo((uint16_t)st.focused, &v)) {
            apply_value((uint16_t)st.focused, &v, EMP_CAUSE_UNDO);
            rebuild_window();
        }
        break;
    }
    case UI_BTN_REDO: {
        hist_value_t v;
        if (hist_redo((uint16_t)st.focused, &v)) {
            apply_value((uint16_t)st.focused, &v, EMP_CAUSE_UNDO);
            rebuild_window();
        }
        break;
    }
    case UI_BTN_SYSTEM: {
        if (st.focused < 0) { st.mode = UI_MODE_SYSTEM; break; }
        const surf_field_t *f = surf_field((uint16_t)st.focused);
        if (!f) break;
        hist_touched((uint16_t)st.focused, now_ms);
        hist_value_t v;
        v.tag     = f->default_tag;
        v.boolean = f->default_boolean;
        v.choice  = f->default_choice;
        v.number  = f->default_number;
        v.color_count = f->default_color_count;
        memcpy(v.color, f->default_color, sizeof(v.color));
        apply_value((uint16_t)st.focused, &v, EMP_CAUSE_RESET);
        hist_commit((uint16_t)st.focused);   /* a reset is finished the instant it happens */
        rebuild_window();
        break;
    }

    case UI_BTN_PAGE_NEXT:
        if (st.focused >= 0) { st.ws--; memset(digit_accum, 0, sizeof(digit_accum)); }
        else set_page((int32_t)st.page + (int32_t)UI_FIELDS_PER_PAGE);
        break;
    default: break;
    }
}

void ui_state_tick(uint32_t now_ms)
{
    hist_tick(now_ms);
    if (st.active_pot >= 0 && (int32_t)(now_ms - st.active_until_ms) >= 0)
        st.active_pot = -1;
}

static void note_rotation(unsigned pot, uint32_t now_ms)
{
    st.active_pot = (int8_t)pot;
    st.active_until_ms = now_ms + UI_ACTIVE_MS;
}

/* ------------------------------------------------------------------ rotation */

static double semantic_step(const surf_field_t *f)
{
    if ((f->present & EMP_PRESENT_STEP) && f->step > 0.0) return f->step;

    if ((f->present & (EMP_PRESENT_MIN | EMP_PRESENT_MAX)) ==
        (EMP_PRESENT_MIN | EMP_PRESENT_MAX)) {
        double range = f->max - f->min;
        if (range > 0.0) return range / 100.0;      /* a hundred detents end to end */
    }
    return pow10i(-(int)precision_of(f));            /* one unit of what is displayed */
}

static double clamp_to_bounds(const surf_field_t *f, double v)
{
    if ((f->present & EMP_PRESENT_MIN) && v < f->min) v = f->min;
    if ((f->present & EMP_PRESENT_MAX) && v > f->max) v = f->max;
    return v;
}

/* A top knob edits one decimal place of the focused value. Only while drilled: in the overview
 * that same knob is an ordinary field control. */
static int rotate_digit(unsigned pot, int32_t detents, uint32_t now_ms)
{
    const surf_field_t *f = surf_field((uint16_t)st.focused);
    if (!f || f->kind == EMP_KIND_READONLY) return 0;
    if (f->kind == EMP_KIND_COLOR) {
        unsigned k = pot - ui_digit_row_first();
        if (k >= f->color_count) return 0;
        float color[4];
        memcpy(color, f->color, sizeof(color));
        color[k] = (float)clamp_to_bounds(f, (double)color[k]
                                          + (double)detents * semantic_step(f));
        if (color[k] == f->color[k]) return 0;
        hist_touched((uint16_t)st.focused, now_ms);
        surf_set_color((uint16_t)st.focused, color, f->color_count);
        return 1;
    }
    if (f->kind == EMP_KIND_TOGGLE || f->kind == EMP_KIND_CHOICE) return 0;

    unsigned k = pot - ui_digit_row_first();
    uint8_t prec = precision_of(f);

    /* Coarser than the raw detent rate: 64 detents a revolution would run one decimal place six
     * times round itself per turn. The remainder is kept so a slow turn still arrives. */
    digit_accum[k] += detents;
    int32_t steps = digit_accum[k] / UI_DIGIT_STEP;
    if (!steps) return 0;
    digit_accum[k] -= steps * UI_DIGIT_STEP;

    double place = pow10i((int)st.ws - (int)k);
    double v = ui_roundp(clamp_to_bounds(f, f->number + (double)steps * place), prec);

    /* Re-clamp the window in BOTH directions. Clamping only as a value shrinks leaves digits
     * that appear as it grows greyed and unreachable by any knob. */
    st.ws = ui_clamp_ws(st.ws, v, prec);

    hist_touched((uint16_t)st.focused, now_ms);
    surf_set_number((uint16_t)st.focused, v);
    return 1;
}

int ui_state_rotate(unsigned pot, int32_t detents, uint32_t now_ms)
{
    if (!detents || pot >= UI_FIELDS_PER_PAGE) return 0;

    if (st.mode == UI_MODE_SYSTEM) {
        int32_t next = (int32_t)st.system_selection + (detents > 0 ? 1 : -1);
        if (next < 0) next = 0;
        if (next > 2) next = 2;
        if ((uint8_t)next == st.system_selection) return 0;
        st.system_selection = (uint8_t)next;
        return 1;
    }
    if (st.mode == UI_MODE_BRIGHTNESS) {
        int32_t next = (int32_t)st.brightness_preview + detents;
        if (next < 10) next = 10;
        if (next > 100) next = 100;
        if ((uint8_t)next == st.brightness_preview) return 0;
        st.brightness_preview = (uint8_t)next;
        st.brightness_save_error = 0;
        pending_action.kind = UI_ACTION_BRIGHTNESS_PREVIEW;
        pending_action.brightness_percent = st.brightness_preview;
        return 1;
    }
    if (st.mode != UI_MODE_SURFACE) return 0;

    if (st.focused >= 0) {
        unsigned dbase = ui_digit_row_first();
        if (pot >= dbase && pot < dbase + UI_DIGITS) {
            int changed = rotate_digit(pot, detents, now_ms);
            if (changed) note_rotation(pot, now_ms);
            return changed;
        }
    }

    int32_t idx = ui_state_field_for(pot);
    if (idx < 0) return 0;

    const surf_field_t *f = surf_field((uint16_t)idx);
    if (!f || f->kind == EMP_KIND_READONLY) return 0;

    if (f->kind == EMP_KIND_COLOR && f->color_count >= 3u) {
        float color[4];
        memcpy(color, f->color, sizeof(color));
        float intensity = color[0];
        if (color[1] > intensity) intensity = color[1];
        if (color[2] > intensity) intensity = color[2];
        float next = (float)clamp_to_bounds(f,
                     (double)intensity + (double)detents * semantic_step(f));
        if (next == intensity) return 0;
        if (intensity > 0.0f) {
            float scale = next / intensity;
            for (unsigned i = 0; i < 3u; i++)
                color[i] = (float)clamp_to_bounds(f, (double)color[i] * (double)scale);
        } else {
            color[0] = color[1] = color[2] = next;
        }
        hist_touched((uint16_t)idx, now_ms);
        surf_set_color((uint16_t)idx, color, f->color_count);
        note_rotation(pot, now_ms);
        return 1;
    }

    if (f->kind == EMP_KIND_TOGGLE || f->kind == EMP_KIND_CHOICE) {
        /* Discrete kinds need a coarser wrist than a continuous one, or a nudge jumps three
         * options. Clamped, never wrapped: wrapping lands somewhere nobody aimed for. */
        discrete_accum[pot] += detents;
        int32_t steps = discrete_accum[pot] / UI_DISCRETE_STEP;
        if (!steps) return 0;
        discrete_accum[pot] -= steps * UI_DISCRETE_STEP;

        if (f->kind == EMP_KIND_TOGGLE) {
            int32_t b = (int32_t)f->boolean + steps;
            if (b < 0) b = 0;
            if (b > 1) b = 1;
            if ((uint8_t)b == f->boolean) return 0;
            hist_touched((uint16_t)idx, now_ms);
            surf_set_bool((uint16_t)idx, (uint8_t)b);
        } else {
            int32_t n = (int32_t)(f->choice_count ? f->choice_count : 1u);
            int32_t c = (int32_t)f->choice + steps;
            if (c < 0) c = 0;
            if (c > n - 1) c = n - 1;
            if ((uint32_t)c == f->choice) return 0;
            hist_touched((uint16_t)idx, now_ms);
            surf_set_choice((uint16_t)idx, (uint32_t)c);
        }
        note_rotation(pot, now_ms);
        return 1;
    }

    uint8_t prec = precision_of(f);
    double v = ui_roundp(clamp_to_bounds(f, f->number + (double)detents * semantic_step(f)), prec);
    if (v == f->number) return 0;

    hist_touched((uint16_t)idx, now_ms);
    surf_set_number((uint16_t)idx, v);
    if (st.focused == idx) st.ws = ui_clamp_ws(st.ws, v, prec);
    note_rotation(pot, now_ms);
    return 1;
}

int ui_state_take_action(ui_action_t *out)
{
    if (!out || pending_action.kind == UI_ACTION_NONE) return 0;
    *out = pending_action;
    pending_action.kind = UI_ACTION_NONE;
    pending_action.pot_mask = 0;
    pending_action.brightness_percent = 0;
    return 1;
}

void ui_state_calibration_status(uint8_t phase, uint8_t pot, uint8_t cycle,
                                 uint8_t valid_mask, uint8_t custom, uint16_t clear_ms,
                                 const char *message)
{
    st.cal_phase = phase;
    st.cal_pot = pot;
    st.cal_cycle = cycle;
    st.cal_valid_mask = valid_mask;
    st.cal_custom = custom;
    st.cal_clear_ms = clear_ms;
    st.cal_message = message;
}

void ui_state_system_status(uint32_t uptime_ms, int touch_status)
{
    st.uptime_ms = uptime_ms;
    st.touch_status = touch_status;
}

void ui_state_brightness_status(uint8_t saved_percent)
{
    if (saved_percent < 10u || saved_percent > 100u) saved_percent = 100u;
    st.brightness_saved = saved_percent;
    if (st.mode != UI_MODE_BRIGHTNESS) st.brightness_preview = saved_percent;
}

void ui_state_brightness_save_result(int success)
{
    if (st.mode != UI_MODE_BRIGHTNESS) return;
    if (success) {
        st.brightness_saved = st.brightness_preview;
        st.brightness_save_error = 0;
        st.mode = UI_MODE_SYSTEM;
    } else {
        st.brightness_save_error = 1;
    }
}
