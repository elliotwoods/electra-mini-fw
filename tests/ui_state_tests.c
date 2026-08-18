/* Host tests for the interaction model: digit arithmetic, press-to-drill, paging, history.
 *
 * These are the parts of the firmware hardest to test with a finger on a knob and easiest to
 * test here. A wrong digit at three decimals, a drill that will not exit, an undo that skips the
 * step you wanted back — each is a minute of typing at this level and a long confusing session
 * at the panel, because the symptom appears under your hand and vanishes when you let go.
 *
 * Several tests exist specifically to pin decisions that were arrived at the hard way, on
 * hardware. Those say so, so that a later simplification back to the obvious shape fails here
 * rather than silently changing how the instrument feels.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ui_state.h"
#include "history.h"
#include "surface.h"

typedef void (*ui_report_fn)(const char *name, int passed);

static int          failures;
static ui_report_fn report_fn;

static void check(const char *name, int passed)
{
    if (!passed) failures++;
    if (report_fn) report_fn(name, passed);
}

static int near(double a, double b) { return fabs(a - b) < 1e-9; }

/* Press and release, which is what a finger does. */
static void tap(unsigned pot, uint32_t t) { ui_state_push(pot, 1, t); ui_state_push(pot, 0, t + 5); }

/* ------------------------------------------------------------------ digit arithmetic */

static void test_msd(void)
{
    int ok = ui_msd(0.0) == 0
          && ui_msd(0.004) == 0            /* floored at 0, never negative */
          && ui_msd(1.0) == 0
          && ui_msd(9.999) == 0
          && ui_msd(10.0) == 1
          && ui_msd(999.0) == 2
          && ui_msd(1000.0) == 3
          && ui_msd(-1234.5) == 3;         /* magnitude, not sign */
    check("msd", ok);
}

static void test_window_bounds(void)
{
    /* At the finest window position the rightmost knob must edit exactly the last displayed
     * decimal — that is the whole contract of min_ws. */
    int ok = ui_min_ws(0) == 3 && ui_min_ws(2) == 1 && ui_min_ws(3) == 0;

    /* Never above the value's own most significant place: a window sitting higher would leave
     * knobs editing leading zeros that are not there. */
    ok = ok && ui_clamp_ws(7, 5000.0, 2) == ui_msd(5000.0);
    ok = ok && ui_clamp_ws(-5, 5000.0, 2) == ui_min_ws(2);
    /* ...unless the precision floor is higher, in which case the floor wins. */
    ok = ok && ui_clamp_ws(7, 5.0, 2) == ui_min_ws(2);
    ok = ok && ui_clamp_ws(0, 0.5, 0) == 3;
    check("window bounds", ok);
}

static void test_digit_at(void)
{
    /* The float trap: floor(|v| / 10^e) on a double starts returning the digit below at three
     * decimals, which is exactly where a control surface lives. 12.345 must read 3,4,5. */
    int ok = ui_digit_at(12.345, 0, 3) == 2
          && ui_digit_at(12.345, 1, 3) == 1
          && ui_digit_at(12.345, -1, 3) == 3
          && ui_digit_at(12.345, -2, 3) == 4
          && ui_digit_at(12.345, -3, 3) == 5;

    ok = ok && ui_digit_at(-6.5, -1, 1) == 5;      /* magnitude; the sign is drawn separately */
    ok = ok && ui_digit_at(0.0, 0, 2) == 0;
    ok = ok && ui_digit_at(999.999, -3, 3) == 9;
    check("digit at place", ok);
}

static void test_roundp(void)
{
    int ok = near(ui_roundp(0.999, 2), 1.00)
          /* 1.005 rounds DOWN, and that is correct rather than a bug: the nearest double to
           * 1.005 is 1.00499999999999989, so there is nothing there to round up. */
          && near(ui_roundp(1.005, 2), 1.00)
          && near(ui_roundp(1.015625, 2), 1.02)      /* exactly representable, rounds up */
          && near(ui_roundp(2.5, 0), 3.0)
          /* Half AWAY from zero, so a knob feels the same either side of zero. */
          && near(ui_roundp(-2.5, 0), -3.0)
          && near(ui_roundp(-0.5, 0), -1.0);
    check("round to precision", ok);
}

/* ------------------------------------------------------------------ the row split */

static void test_row_split_is_not_the_page_size(void)
{
    /* These two numbers used to be one constant, and were equal only by coincidence of a 4+4
     * layout. Merged, a page of 8 makes `pot >= UI_FIELDS_PER_PAGE` never true, so every knob
     * routes to the field branch and the entire digit window becomes unreachable code that
     * still compiles. This test is here to make that failure loud. */
    int ok = UI_FIELDS_PER_PAGE == 8u
          && UI_ROW_SPLIT == 4u
          && UI_DIGITS == 4u
          && (UI_ROW_SPLIT + UI_DIGITS) == UI_FIELDS_PER_PAGE;
    check("row split is distinct from page size", ok);
}

/* ------------------------------------------------------------------ press to drill */

static void test_press_drills_and_exits(void)
{
    surf_demo_descriptor();
    ui_state_init();

    int ok = ui_state()->focused < 0;
    tap(0, 1000);
    ok = ok && ui_state()->focused == 0;

    /* A top knob is a digit control while drilled, so pressing one means "done here". */
    tap(UI_ROW_SPLIT, 1100);
    ok = ok && ui_state()->focused < 0;
    check("press drills in, top press exits", ok);
}

static void test_press_is_edge_not_level(void)
{
    surf_demo_descriptor();
    ui_state_init();

    ui_state_push(0, 1, 1000);
    int ok = ui_state()->focused == 0;

    /* Holding must not re-trigger. A level-triggered drill would toggle every service pass for
     * as long as a finger stayed on the switch. */
    ui_state_push(0, 1, 1020);
    ui_state_push(0, 1, 1040);
    ok = ok && ui_state()->focused == 0;

    ui_state_push(0, 0, 1060);
    ok = ok && ui_state()->focused == 0;   /* release alone does not exit */
    check("press acts on the edge, not the level", ok);
}

static void test_bottom_press_moves_between_fields(void)
{
    surf_demo_descriptor();
    ui_state_init();

    tap(0, 1000);
    int ok = ui_state()->focused == 0;

    /* Pressing a different bottom knob moves to that field rather than leaving, which is what
     * you want when comparing two parameters. */
    tap(1, 1100);
    ok = ok && ui_state()->focused == 1;

    /* Pressing the SAME knob again leaves — otherwise there would be no way out from the row
     * you are already on without reaching for the top. */
    tap(1, 1200);
    ok = ok && ui_state()->focused < 0;
    check("bottom press moves between fields, repeat exits", ok);
}

static void test_drill_is_symmetric(void)
{
    surf_demo_descriptor();
    ui_state_init();

    /* Drill a BOTTOM field: the TOP row becomes the digit editor. */
    tap(0, 1000);
    int ok = ui_state()->focused == 0
          && ui_state()->digit_top == 1
          && ui_digit_row_first() == UI_ROW_SPLIT
          && ui_cell_row_first() == 0;

    /* And a top press exits, because the top row is what is editing digits. */
    tap(UI_ROW_SPLIT, 1100);
    ok = ok && ui_state()->focused < 0;

    /* Drill a TOP field: the BOTTOM row becomes the digit editor, mirrored. */
    tap(UI_ROW_SPLIT, 1200);
    ok = ok && ui_state()->focused == (int32_t)UI_ROW_SPLIT
            && ui_state()->digit_top == 0
            && ui_digit_row_first() == 0
            && ui_cell_row_first() == UI_ROW_SPLIT;

    /* Now it is a BOTTOM press that exits, for the same reason reversed. */
    tap(0, 1300);
    ok = ok && ui_state()->focused < 0;
    check("drilling is symmetric between the rows", ok);
}

static void test_digit_knobs_follow_the_row(void)
{
    surf_demo_descriptor();
    ui_state_init();

    /* Drilled into a top field, the BOTTOM knobs edit digits and the TOP knobs edit fields. */
    tap(UI_ROW_SPLIT, 1000);
    double drilled = surf_field(UI_ROW_SPLIT)->number;
    double neighbour = surf_field(1)->number;

    (void)ui_state_rotate(1, UI_DIGIT_STEP, 1100);      /* a bottom knob: now a digit control */
    int ok = !near(surf_field(UI_ROW_SPLIT)->number, drilled);
    ok = ok && near(surf_field(1)->number, neighbour);  /* and it did NOT edit its own field */

    /* while a top knob still edits its own field */
    double other = surf_field(UI_ROW_SPLIT + 1)->number;
    (void)ui_state_rotate(UI_ROW_SPLIT + 1, 4, 1200);
    ok = ok && !near(surf_field(UI_ROW_SPLIT + 1)->number, other);
    check("digit knobs follow the opposite row", ok);
}

static void test_touch_only_highlights(void)
{
    surf_demo_descriptor();
    ui_state_init();

    /* Touch must not drill. The whole reason for this redesign is that the sensor drops detect
     * for up to 272 ms with a finger still down, so nothing may latch on it. */
    ui_state_touch(0x01, 1000);
    int ok = ui_state()->focused < 0;

    tap(0, 1100);
    ui_state_touch(0x20, 1200);                       /* a finger on top knob index 1 */
    ok = ok && ui_state()->held_digit == 1;

    ui_state_touch(0x00, 1300);
    ok = ok && ui_state()->held_digit == -1;
    ok = ok && ui_state()->focused == 0;              /* and losing touch does NOT exit */
    check("touch only highlights a digit", ok);
}

/* ------------------------------------------------------------------ paging */

static void test_page_buttons(void)
{
    surf_demo_descriptor();
    ui_state_init();

    int ok = ui_state()->page == 0;
    ok = ok && !ui_state_button_enabled(UI_BTN_PAGE_PREV);   /* nowhere to go back to */
    ok = ok && ui_state_button_enabled(UI_BTN_PAGE_NEXT);

    ui_state_button(UI_BTN_PAGE_NEXT, 1, 1000);
    ok = ok && ui_state()->page == UI_FIELDS_PER_PAGE;
    ok = ok && ui_state_field_for(0) == (int32_t)UI_FIELDS_PER_PAGE;

    /* Groups are absolute, so no field appears on two pages and the last page stays short
     * rather than sliding backwards to stay full. */
    for (int i = 0; i < 10; i++) ui_state_button(UI_BTN_PAGE_NEXT, 1, 1100 + (uint32_t)i);
    ok = ok && (ui_state()->page % UI_FIELDS_PER_PAGE) == 0;
    ok = ok && !ui_state_button_enabled(UI_BTN_PAGE_NEXT);

    for (int i = 0; i < 10; i++) ui_state_button(UI_BTN_PAGE_PREV, 1, 1200 + (uint32_t)i);
    ok = ok && ui_state()->page == 0;
    check("paging by button, stable absolute groups", ok);
}

static void test_paging_leaves_the_drilled_field(void)
{
    surf_demo_descriptor();
    ui_state_init();

    tap(0, 1000);
    int ok = ui_state()->focused == 0;

    /* The focused field is not on the new page, so staying drilled would show a field the knobs
     * no longer address. */
    ui_state_button(UI_BTN_PAGE_NEXT, 1, 1100);
    ok = ok && ui_state()->focused < 0;
    check("paging leaves the drilled field", ok);
}

/* ------------------------------------------------------------------ editing */

static void test_readonly_is_not_editable(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_state_button(UI_BTN_PAGE_NEXT, 1, 1000);      /* CPU lives past the first page */

    uint16_t idx = 10;
    const surf_field_t *f = surf_field(idx);
    int ok = (f != 0) && f->kind == EMP_KIND_READONLY;
    if (ok) {
        double before = f->number;
        (void)ui_state_rotate(idx - UI_FIELDS_PER_PAGE, 5, 1100);
        ok = near(surf_field(idx)->number, before);
    }
    check("a read-only field ignores its knob", ok);
}

static void test_choice_steps_and_clamps(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_state_button(UI_BTN_PAGE_NEXT, 1, 1000);

    uint16_t idx = 8;
    unsigned knob = idx - UI_FIELDS_PER_PAGE;
    const surf_field_t *f = surf_field(idx);
    int ok = (f != 0) && f->kind == EMP_KIND_CHOICE && f->choice_count == 5;

    if (ok) {
        uint32_t start = surf_field(idx)->choice;
        (void)ui_state_rotate(knob, 1, 1100);                 /* below the threshold */
        ok = ok && surf_field(idx)->choice == start;

        (void)ui_state_rotate(knob, UI_DISCRETE_STEP, 1110);
        ok = ok && surf_field(idx)->choice == start + 1u;

        /* Clamped at the ends, never wrapped: wrapping lands somewhere nobody aimed for. */
        for (int i = 0; i < 20; i++) (void)ui_state_rotate(knob, UI_DISCRETE_STEP, 1200 + (uint32_t)i);
        ok = ok && surf_field(idx)->choice == 4u;
        for (int i = 0; i < 40; i++) (void)ui_state_rotate(knob, -UI_DISCRETE_STEP, 1300 + (uint32_t)i);
        ok = ok && surf_field(idx)->choice == 0u;
    }
    check("choice steps at threshold and clamps", ok);
}

static void test_choice_labels(void)
{
    surf_demo_descriptor();

    const surf_field_t *f = surf_field(8);
    uint16_t len = 0;
    const char *l = f ? surf_choice_label(f, 2, &len) : 0;

    /* Without this the knob reads "2" where the host said "Sawtooth". */
    int ok = l && len == 8 && memcmp(l, "Sawtooth", 8) == 0;
    ok = ok && surf_choice_label(f, 99, &len) == 0;      /* out of range is NULL, not garbage */
    check("choice labels are stored and readable", ok);
}

static void test_top_row_edits_digits_only_when_drilled(void)
{
    surf_demo_descriptor();
    ui_state_init();

    /* Not drilled: a top knob is an ordinary field control for its own field. */
    double before = surf_field(UI_ROW_SPLIT)->number;
    (void)ui_state_rotate(UI_ROW_SPLIT, 4, 1000);
    int ok = !near(surf_field(UI_ROW_SPLIT)->number, before);

    /* Drilled into field 0, the same knob edits a decimal place of field 0 instead. */
    tap(0, 1100);
    double other = surf_field(UI_ROW_SPLIT)->number;
    double mine  = surf_field(0)->number;
    (void)ui_state_rotate(UI_ROW_SPLIT, UI_DIGIT_STEP, 1200);
    ok = ok && near(surf_field(UI_ROW_SPLIT)->number, other);
    ok = ok && !near(surf_field(0)->number, mine);
    check("top row edits digits only while drilled", ok);
}

/* ------------------------------------------------------------------ history */

static void test_history_commits_on_settle(void)
{
    surf_demo_descriptor();
    ui_state_init();
    tap(0, 1000);

    double start = surf_field(0)->number;

    /* One continuous twiddle. Every detent calls hist_touched, and none of them may commit —
     * otherwise a single sweep of a knob becomes dozens of undo steps and stepping back through
     * them is useless. */
    for (int i = 0; i < 10; i++) (void)ui_state_rotate(0, 1, 1100 + (uint32_t)(i * 20));
    ui_state_tick(1300);
    double swept = surf_field(0)->number;

    ui_state_tick(1300 + HIST_SETTLE_MS + 1);          /* now it settles */

    int ok = !near(swept, start);
    ok = ok && ui_state_button_enabled(UI_BTN_UNDO);

    ui_state_button(UI_BTN_UNDO, 1, 2000);
    ok = ok && near(surf_field(0)->number, start);     /* back to where the twiddle began */

    ui_state_button(UI_BTN_REDO, 1, 2100);
    ok = ok && near(surf_field(0)->number, swept);
    check("one twiddle is one undo step", ok);
}

static void test_undo_commits_a_pending_change_first(void)
{
    surf_demo_descriptor();
    ui_state_init();
    tap(0, 1000);

    double start = surf_field(0)->number;
    (void)ui_state_rotate(0, 5, 1100);
    double changed = surf_field(0)->number;

    /* Undo pressed straight after a change, before the settle timer fires. It must return to
     * where you were, not to where you were two changes ago — getting that wrong is the classic
     * way undo feels broken, because the step you most want back is the one silently skipped. */
    ui_state_button(UI_BTN_UNDO, 1, 1150);
    int ok = !near(changed, start) && near(surf_field(0)->number, start);
    check("undo commits a pending change first", ok);
}

static void test_exit_commits_history(void)
{
    surf_demo_descriptor();
    ui_state_init();
    tap(0, 1000);

    (void)ui_state_rotate(0, 5, 1100);
    tap(UI_ROW_SPLIT, 1150);                    /* leave well before the settle timer */

    /* Leaving ends the adjustment: the user has demonstrably moved on. */
    tap(0, 1200);
    int ok = ui_state_button_enabled(UI_BTN_UNDO);
    check("leaving a field commits its history", ok);
}

static void test_affordances_reflect_what_would_happen(void)
{
    surf_demo_descriptor();
    ui_state_init();

    /* In the overview, none of the per-field buttons can do anything, and saying so is better
     * than looking live and silently ignoring a press. */
    int ok = !ui_state_button_enabled(UI_BTN_EXIT)
          && !ui_state_button_enabled(UI_BTN_UNDO)
          && !ui_state_button_enabled(UI_BTN_REDO)
          && !ui_state_button_enabled(UI_BTN_RESET);

    tap(0, 1000);
    ok = ok && ui_state_button_enabled(UI_BTN_EXIT);
    ok = ok && !ui_state_button_enabled(UI_BTN_UNDO);     /* nothing recorded yet */
    ok = ok && !ui_state_button_enabled(UI_BTN_REDO);
    check("affordances reflect what would happen", ok);
}

/* ------------------------------------------------------------------ runner */

int ui_run_selftests(ui_report_fn report)
{
    failures  = 0;
    report_fn = report;

    test_msd();
    test_window_bounds();
    test_digit_at();
    test_roundp();

    test_row_split_is_not_the_page_size();

    test_press_drills_and_exits();
    test_press_is_edge_not_level();
    test_bottom_press_moves_between_fields();
    test_touch_only_highlights();
    test_drill_is_symmetric();
    test_digit_knobs_follow_the_row();

    test_page_buttons();
    test_paging_leaves_the_drilled_field();

    test_readonly_is_not_editable();
    test_choice_steps_and_clamps();
    test_choice_labels();
    test_top_row_edits_digits_only_when_drilled();

    test_history_commits_on_settle();
    test_undo_commits_a_pending_change_first();
    test_exit_commits_history();
    test_affordances_reflect_what_would_happen();

    return failures;
}
