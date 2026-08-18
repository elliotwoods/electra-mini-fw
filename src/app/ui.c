/* The control-surface UI.
 *
 * Two views, chosen by whether a field is drilled into:
 *
 *   - **Overview.** All eight fields at once, in two rows of four mirroring the panel. This is
 *     the resting state, and with eight knobs each owning a cell there is nothing hidden.
 *   - **Drilled.** One field filling the panel, its number set large enough to read across a
 *     room, with a line from each top-row knob down to the digit it edits.
 *
 * Layout follows the hardware. The top knob row sits physically above the screen and the bottom
 * row below it, so the top cells hang at the top edge and the bottom cells at the bottom, and
 * the digit captions and their link lines run off the top. Nothing here should make you look
 * somewhere other than at the knob you are turning.
 *
 * The band between the rows is the one region no knob points at, which makes it the right place
 * for things about the surface as a whole: where you are in it, and what the six buttons do.
 *
 * Two constraints shaped all of it, both measured:
 *
 *  - **Ink is expensive, area is cheap.** Filled rectangles are hardware-accelerated and cost a
 *    few register writes whatever their size; text and icons travel over SPI pixel by pixel. So
 *    backgrounds, bars and rules are effectively free and glyphs are not.
 *  - **Only repaint what changed.** A full-screen repaint measures 56 ms. Every region tracks
 *    what it last drew, so a still panel costs nothing and a turning knob repaints one number.
 */

#include <string.h>
#include "ui.h"
#include "ui_state.h"
#include "history.h"
#include "icons.h"
#include "ra8876.h"
#include "text.h"
#include "surface.h"

/* A restrained palette: dark ground, one accent, grey for anything secondary. Colour is reserved
 * for state that matters rather than spent on decoration. */
#define C_BG        0x0000
#define C_CELL      0x1082      /* very dark grey: separates a cell from the ground */
#define C_CELL_HOT  0x2124      /* under a finger */
#define C_CELL_DOWN 0x3186      /* switch held: brighter than touched, because it is deliberate */
#define C_RULE      0x39E7
#define C_LABEL     0x8C71
#define C_VALUE     0xFFFF
#define C_UNIT      0x6B4D
#define C_ACCENT    0x07FF      /* cyan: bars, the drilled cell, the live digit */
#define C_TRACK     0x18E3      /* the EMPTY part of a bar */
#define C_DIM       0x4208      /* a digit outside the value's magnitude */
#define C_DISABLED  0x2965      /* an affordance that would do nothing */
#define C_TOGGLE_ON 0x07E0
#define C_LINK      0x4C7F      /* knob-to-digit link lines */

/* One spacing scale, so the layout is composed rather than accumulated. Every gap below is a
 * multiple of this; the previous pass used a different number at each site and it showed. */
#define SP          6
#define MARGIN_X    (2 * SP)

#define COLS        4
#define KNOB_PITCH  (LCD_WIDTH / COLS)
#define KNOB_CX(i)  ((int16_t)(KNOB_PITCH / 2 + (i) * KNOB_PITCH))
#define CELL_W      ((LCD_WIDTH - 2 * MARGIN_X - (COLS - 1) * SP) / COLS)
#define CELL_X(c)   ((int16_t)(MARGIN_X + (c) * (CELL_W + SP)))

/* Vertical bands. The two knob rows own the edges; the middle carries the surface itself. */
#define ROW_TOP_Y   SP
/* 2*ROW_H + BAND_H + four gaps must equal the panel height exactly. At 190 the bottom row ran
 * six pixels off the screen, which the simulator showed immediately and a device would have
 * shown as a clipped fader. */
#define ROW_H       184
#define BAND_Y      (ROW_TOP_Y + ROW_H + SP)          /* 202 */
#define BAND_H      88
#define ROW_BOT_Y   (BAND_Y + BAND_H + SP)            /* 296 */

#define PAGE_Y      BAND_Y
#define LEGEND_Y    (BAND_Y + 36)
#define LEGEND_H    (BAND_H - 36)

/* Drilled view, mirrored about the band.
 *
 * Drilling replaces the area belonging to whichever row became the digit editor, which is
 * always the row OPPOSITE the drilled field. So the readout appears beside the knobs that edit
 * it, the link lines stay short and point at the working hand, and the row holding the drilled
 * field keeps showing its cells so it can still be seen in context.
 *
 * The band never moves: the six buttons are physical and always mean the same thing, so their
 * legend staying put is worth more than the space it costs.
 *
 * The _T constants are the digits-at-top case and the _B ones its mirror. A 121 px readout has
 * to fit between the link lines and the band, which is what sets them; there is no slack. */
#define FC_CAP_T        4
#define FC_LINK_FAR_T   32          /* the end nearest the knobs */
#define FC_LINK_NEAR_T  68          /* the end nearest the digits */
#define FC_DIGIT_T      72
#define FC_DIGIT_H      121

#define FC_DIGIT_B      (LCD_HEIGHT - FC_DIGIT_T - FC_DIGIT_H)
#define FC_LINK_NEAR_B  (LCD_HEIGHT - FC_LINK_NEAR_T)
#define FC_LINK_FAR_B   (LCD_HEIGHT - FC_LINK_FAR_T)
#define FC_CAP_B        (LCD_HEIGHT - FC_CAP_T - 28)

/* ------------------------------------------------------------------ helpers */

/* Scale an RGB565 towards black, for the "everything but the digit under your finger fades
 * back" emphasis that makes the drilled view readable at a glance. */
static uint16_t dim(uint16_t c, uint8_t pct)
{
    uint32_t r = ((c >> 11) & 0x1F) * pct / 100u;
    uint32_t g = ((c >> 5) & 0x3F) * pct / 100u;
    uint32_t b = (c & 0x1F) * pct / 100u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void frame(int16_t x, int16_t y, int16_t w, int16_t h, int16_t t, uint16_t c)
{
    ra8876_set_fg(c);
    ra8876_fill_rect(x, y, w, t);
    ra8876_fill_rect(x, (int16_t)(y + h - t), w, t);
    ra8876_fill_rect(x, y, t, h);
    ra8876_fill_rect((int16_t)(x + w - t), y, t, h);
}

/* Pooled strings are not NUL-terminated on the wire. */
static void str_of(char *out, uint32_t cap, uint16_t off, uint16_t len)
{
    const char *s = surf_string(off, len);
    uint32_t n = (len < cap - 1u) ? len : cap - 1u;
    if (s && n) memcpy(out, s, n); else n = 0;
    out[n] = 0;
}

/* Shorten in place until it fits, ending with a visible mark. A label that has quietly lost its
 * tail reads as a different parameter, which is worse than one that admits it was cut. */
static void fit_text(const font_t *fnt, char *buf, int16_t max_w)
{
    if (text_width(fnt, buf) <= max_w) return;

    uint32_t n = 0;
    while (buf[n]) n++;
    while (n > 1) {
        buf[--n] = 0;
        char save = buf[n - 1];
        buf[n - 1] = '~';
        if (text_width(fnt, buf) <= max_w) return;
        buf[n - 1] = save;
    }
}

static uint8_t precision_of(const surf_field_t *f)
{
    return (uint8_t)((f->present & EMP_PRESENT_PRECISION) ? f->precision : 0);
}

/* Kind-aware, deliberately. Formatting every value as a number is what made the Lua bundle show
 * "Mode: 2" instead of "Mode: Additive". */
static void value_text(const surf_field_t *f, char *out, uint32_t cap)
{
    switch (f->value_tag) {
    case EMP_VAL_BOOL:
        if (cap > 4) memcpy(out, f->boolean ? "ON" : "OFF", f->boolean ? 3u : 4u);
        else out[0] = 0;
        break;

    case EMP_VAL_CHOICE: {
        uint16_t len = 0;
        const char *l = surf_choice_label(f, (uint16_t)f->choice, &len);
        if (l) {
            uint32_t n = (len < cap - 1u) ? len : cap - 1u;
            memcpy(out, l, n);
            out[n] = 0;
        } else {
            /* No labels in the descriptor: the index is all there is, and showing it is honest. */
            (void)text_format(out, cap, (double)f->choice, 0);
        }
        break;
    }

    default:
        (void)text_format(out, cap, f->number, precision_of(f));
        break;
    }
}

/* How full a bar is, 0..span. A field without bounds gets a centred pip instead: a fill level
 * implies a range, and inventing one would be a lie. */
static int16_t bar_extent(const surf_field_t *f, int16_t span)
{
    if (f->kind == EMP_KIND_TOGGLE) return f->boolean ? span : 0;

    if (f->kind == EMP_KIND_CHOICE && f->choice_count > 1u) {
        return (int16_t)((int32_t)span * (int32_t)f->choice / (int32_t)(f->choice_count - 1u));
    }

    if ((f->present & (EMP_PRESENT_MIN | EMP_PRESENT_MAX)) !=
        (EMP_PRESENT_MIN | EMP_PRESENT_MAX)) return -1;

    double range = f->max - f->min;
    if (range <= 0.0) return 0;

    double t = (f->number - f->min) / range;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return (int16_t)(t * (double)span);
}

/* ------------------------------------------------------------------ change tracking */

typedef struct {
    int      valid;
    uint16_t id;
    double   number;
    uint32_t choice;
    uint8_t  boolean;
    uint8_t  tag;
    uint8_t  touched;
    uint8_t  pressed;
    uint8_t  drilled;
    int16_t  bar_px;
} cell_cache_t;

static cell_cache_t last_cell[UI_FIELDS_PER_PAGE];
static int          last_view = -1;         /* -1 nothing drawn, 0 overview, 1 drilled */
static int32_t      last_focus = -2;
static uint16_t     last_page = 0xFFFF;
static double       last_value;
static int8_t       last_ws, last_held;
static uint8_t      last_legend[UI_BTN_COUNT];
static int          legend_valid;

static void forget_everything(void)
{
    memset(last_cell, 0, sizeof(last_cell));
    last_view    = -1;
    last_focus   = -2;
    last_page    = 0xFFFF;
    last_ws      = -99;
    last_held    = -99;
    legend_valid = 0;
}

void ui_reset(void)
{
    forget_everything();
    ra8876_set_fg(C_BG);
    ra8876_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

/* ------------------------------------------------------------------ page position */

static void draw_page_strip(int16_t y)
{
    ra8876_set_fg(C_BG);
    ra8876_fill_rect(0, y, LCD_WIDTH, 32);

    uint16_t total = surf_field_count();
    uint16_t page  = ui_state()->page;
    if (!total) return;

    int16_t tx = MARGIN_X, tw = LCD_WIDTH - 2 * MARGIN_X;
    ra8876_set_fg(C_TRACK);
    ra8876_fill_rect(tx, (int16_t)(y + 26), tw, 4);

    int16_t thumb = (int16_t)((int32_t)tw * (int32_t)UI_FIELDS_PER_PAGE / (int32_t)total);
    if (thumb < 30) thumb = 30;
    if (thumb > tw) thumb = tw;

    int16_t span = (int16_t)(total > UI_FIELDS_PER_PAGE ? total - UI_FIELDS_PER_PAGE : 1);
    int16_t pos  = (int16_t)((int32_t)(tw - thumb) * (int32_t)page / (int32_t)span);

    ra8876_set_fg(C_ACCENT);
    ra8876_fill_rect((int16_t)(tx + pos), (int16_t)(y + 26), thumb, 4);

    char buf[24], n[8];
    uint16_t first = (uint16_t)(page + 1u);
    uint16_t last  = (uint16_t)(page + UI_FIELDS_PER_PAGE);
    if (last > total) last = total;

    unsigned k = 0;
    (void)text_format(n, sizeof(n), (double)first, 0);
    for (unsigned i = 0; n[i] && k < sizeof(buf) - 1; i++) buf[k++] = n[i];
    if (k < sizeof(buf) - 1) buf[k++] = '-';
    (void)text_format(n, sizeof(n), (double)last, 0);
    for (unsigned i = 0; n[i] && k < sizeof(buf) - 1; i++) buf[k++] = n[i];
    if (k + 3 < sizeof(buf) - 1) { buf[k++] = ' '; buf[k++] = '/'; buf[k++] = ' '; }
    (void)text_format(n, sizeof(n), (double)total, 0);
    for (unsigned i = 0; n[i] && k < sizeof(buf) - 1; i++) buf[k++] = n[i];
    buf[k] = 0;

    text_draw(&font_small, MARGIN_X, y, buf, C_LABEL, C_BG);

    /* While drilled, the band also carries the field's name. It is the most relevant thing on
     * screen and there is no room for it above, where the digits reach almost to the band. */
    int32_t focused = ui_state()->focused;
    if (focused >= 0) {
        const surf_field_t *f = surf_field((uint16_t)focused);
        if (f) {
            char lbl[48];
            str_of(lbl, sizeof(lbl), f->label_off, f->label_len);
            fit_text(&font_small, lbl, LCD_WIDTH / 2);
            text_draw_centred(&font_small, LCD_WIDTH / 2, y, lbl, C_VALUE, C_BG);
        }
    }
}

/* ------------------------------------------------------------------ button legend */

/* What the six buttons do, drawn where they are, so the mapping never has to be remembered.
 * A button that would do nothing is drawn disabled rather than looking live and ignoring you. */
static void draw_legend(int full)
{
    static const char *const glyph[UI_BTN_COUNT] = {
        ICON_EXIT, ICON_UNDO, ICON_REDO, ICON_RESET, ICON_PREV, ICON_NEXT
    };

    /* Two bits per button: whether it would act, and whether it is being held. Packed together
     * so one memcmp decides the whole strip, which keeps a still panel free. */
    uint8_t want[UI_BTN_COUNT];
    for (unsigned b = 0; b < UI_BTN_COUNT; b++) {
        want[b] = (uint8_t)(ui_state_button_enabled(b)
                | ((ui_state()->btn_mask & (1u << b)) ? 2u : 0u));
    }

    if (!full && legend_valid && memcmp(want, last_legend, sizeof(want)) == 0) return;

    int16_t w = (LCD_WIDTH - 2 * MARGIN_X - (int16_t)(UI_BTN_COUNT - 1) * SP)
              / (int16_t)UI_BTN_COUNT;

    for (unsigned b = 0; b < UI_BTN_COUNT; b++) {
        int16_t x = (int16_t)(MARGIN_X + (int16_t)b * (w + SP));
        int live = (want[b] & 1u) != 0;
        int down = (want[b] & 2u) != 0;

        uint16_t bg = down ? C_CELL_DOWN : C_CELL;
        ra8876_set_fg(bg);
        ra8876_fill_rect(x, LEGEND_Y, w, LEGEND_H);
        frame(x, LEGEND_Y, w, LEGEND_H, 1, down ? C_ACCENT : (live ? C_RULE : C_CELL));

        text_draw_centred(&font_icons, (int16_t)(x + w / 2),
                          (int16_t)(LEGEND_Y + (LEGEND_H - text_height(&font_icons)) / 2),
                          glyph[b], live ? C_LABEL : C_DISABLED, bg);
    }

    memcpy(last_legend, want, sizeof(want));
    legend_valid = 1;
}

/* ------------------------------------------------------------------ overview */

static void draw_cell(unsigned knob, const surf_field_t *f, int touched, int pressed, int drilled)
{
    int16_t x = CELL_X(knob % COLS);
    int16_t y = (knob < UI_ROW_SPLIT) ? ROW_BOT_Y : ROW_TOP_Y;
    int16_t h = ROW_H;

    /* A press is deliberate where a touch is incidental, so it reads brighter. Without this a
     * press had no feedback at all until the view changed, which makes a switch feel dead. */
    uint16_t bg = pressed ? C_CELL_DOWN : (touched ? C_CELL_HOT : C_CELL);
    ra8876_set_fg(bg);
    ra8876_fill_rect(x, y, CELL_W, h);
    frame(x, y, CELL_W, h, 2, drilled ? C_ACCENT : (touched ? C_LABEL : C_RULE));

    if (!f) {
        text_draw_centred(&font_ui, (int16_t)(x + CELL_W / 2),
                          (int16_t)(y + h / 2 - 20), "--", C_RULE, bg);
        return;
    }

    char buf[48];
    int16_t bw = 12;
    int16_t bx = (int16_t)(x + CELL_W - 2 * SP - bw);
    int16_t avail = (int16_t)(bx - SP - (x + 2 * SP));

    str_of(buf, sizeof(buf), f->label_off, f->label_len);
    fit_text(&font_ui, buf, (int16_t)(CELL_W - 4 * SP));
    text_draw(&font_ui, (int16_t)(x + 2 * SP), (int16_t)(y + SP), buf, C_LABEL, bg);

    /* The value on the cell's centre line, left-aligned: a column of numbers sharing a left edge
     * is far easier to compare down the panel than one that shifts with every digit count. */
    int16_t vy = (int16_t)(y + h / 2 - text_height(&font_value) / 2);
    value_text(f, buf, sizeof(buf));

    /* Numbers get the monospaced face so digits do not shift as they change; words get the
     * proportional one, since monospacing "Sawtooth" buys nothing and costs the width. */
    const font_t *vf = (f->value_tag == EMP_VAL_NUMBER) ? &font_value : &font_ui;
    fit_text(vf, buf, avail);

    uint16_t vc = C_VALUE;
    if (f->value_tag == EMP_VAL_BOOL) vc = f->boolean ? C_TOGGLE_ON : C_UNIT;
    if (f->kind == EMP_KIND_READONLY)  vc = C_LABEL;
    text_draw(vf, (int16_t)(x + 2 * SP), vy, buf, vc, bg);

    if (f->present & EMP_PRESENT_UNIT) {
        str_of(buf, sizeof(buf), f->unit_off, f->unit_len);
        fit_text(&font_small, buf, avail);
        text_draw(&font_small, (int16_t)(x + 2 * SP),
                  (int16_t)(vy + text_height(vf) - 4), buf, C_UNIT, bg);
    }

    /* A fader down the cell's right edge: it uses the height, four of them side by side read as
     * a set, and it matches the range bar in the drilled view. */
    int16_t by = (int16_t)(y + 48);
    int16_t bh = (int16_t)(h - 48 - 2 * SP);
    int16_t ext = bar_extent(f, bh);

    ra8876_set_fg(C_TRACK);
    ra8876_fill_rect(bx, by, bw, bh);
    if (ext > 0) {
        ra8876_set_fg(f->kind == EMP_KIND_READONLY ? C_LABEL : C_ACCENT);
        ra8876_fill_rect(bx, (int16_t)(by + bh - ext), bw, ext);
    } else if (ext < 0) {
        ra8876_set_fg(C_RULE);
        ra8876_fill_rect(bx, (int16_t)(by + bh / 2 - 2), bw, 4);
    }
}

/* Draw the cells in [first, last). Both views use this: the overview draws all eight, the
 * drilled view draws only the bottom row, whose knobs are still editing their own fields. */
static void render_cells(int full, unsigned first, unsigned last)
{
    const ui_state_t *st = ui_state();

    for (unsigned k = first; k < last; k++) {
        int32_t idx = ui_state_field_for(k);
        const surf_field_t *f = (idx >= 0) ? surf_field((uint16_t)idx) : 0;
        int touched = (st->touch_mask & (1u << k)) != 0;
        int pressed = (st->press_mask & (1u << k)) != 0;

        uint8_t drilled = (uint8_t)(idx >= 0 && idx == st->focused);
        int dirty = full || !last_cell[k].valid
                 || last_cell[k].touched != (uint8_t)touched
                 || last_cell[k].pressed != (uint8_t)pressed
                 || last_cell[k].drilled != drilled;

        int16_t bh = (int16_t)(ROW_H - 48 - 2 * SP);
        if (f && !dirty) {
            dirty = last_cell[k].id      != f->id
                 || last_cell[k].tag     != f->value_tag
                 || last_cell[k].boolean != f->boolean
                 || last_cell[k].choice  != f->choice
                 || last_cell[k].number  != f->number
                 || last_cell[k].bar_px  != bar_extent(f, bh);
        } else if (!f && last_cell[k].valid && last_cell[k].id == 0xFFFF) {
            dirty = 0;
        }
        if (!dirty) continue;

        draw_cell(k, f, touched, pressed, idx >= 0 && idx == st->focused);

        last_cell[k].valid   = 1;
        last_cell[k].touched = (uint8_t)touched;
        last_cell[k].pressed = (uint8_t)pressed;
        last_cell[k].drilled = (uint8_t)(idx >= 0 && idx == st->focused);
        last_cell[k].id      = f ? f->id : 0xFFFF;
        last_cell[k].tag     = f ? f->value_tag : 0;
        last_cell[k].number  = f ? f->number : 0;
        last_cell[k].choice  = f ? f->choice : 0;
        last_cell[k].boolean = f ? f->boolean : 0;
        last_cell[k].bar_px  = f ? bar_extent(f, bh) : 0;
    }
}

static void render_overview(int full)
{
    const ui_state_t *st = ui_state();

    if (full) {
        ra8876_set_fg(C_BG);
        ra8876_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    }
    if (full || st->page != last_page) {
        draw_page_strip(PAGE_Y);
        last_page = st->page;
    }
    draw_legend(full);
    render_cells(full, 0, UI_FIELDS_PER_PAGE);
}

/* ------------------------------------------------------------------ drilled readout */

typedef struct { int16_t x; char ch; int8_t e; int8_t knob; uint8_t grey; } digit_t;

#define MAX_DIGITS 20

/* The union of "the whole number" and "the knob window", so growing a value never hides the
 * digits you just created and shrinking one never leaves knobs pointing at nothing. */
static unsigned layout_digits(const surf_field_t *f, digit_t *out, int16_t *total_w)
{
    const ui_state_t *st = ui_state();
    uint8_t prec = precision_of(f);
    double  v    = f->number;

    int8_t vmsd = ui_msd(v);
    int8_t topE = st->ws > vmsd ? st->ws : vmsd;
    int8_t botE = (int8_t)(st->ws - (int8_t)(UI_DIGITS - 1));
    if (botE > -(int8_t)prec) botE = -(int8_t)prec;

    int16_t adv = text_width(&font_display, "0");
    unsigned n = 0;
    int16_t x = 0;

    if (v < 0 && n < MAX_DIGITS) {
        out[n].x = x; out[n].ch = '-'; out[n].e = 127; out[n].knob = -1; out[n].grey = 0;
        n++; x = (int16_t)(x + adv);
    }

    for (int8_t e = topE; e >= botE && n < MAX_DIGITS; e--) {
        out[n].x    = x;
        out[n].ch   = (char)('0' + ui_digit_at(v, e, prec));
        out[n].e    = e;
        out[n].grey = (uint8_t)(e > vmsd);        /* a leading zero the number does not have */
        out[n].knob = (e <= st->ws && e > (int8_t)(st->ws - (int8_t)UI_DIGITS))
                    ? (int8_t)(st->ws - e) : -1;
        n++; x = (int16_t)(x + adv);

        if (e == 0 && prec && n < MAX_DIGITS) {
            out[n].x = x; out[n].ch = '.'; out[n].e = 127; out[n].knob = -1; out[n].grey = 0;
            n++; x = (int16_t)(x + adv);
        }
    }
    *total_w = x;
    return n;
}

static void render_drilled(int full, const surf_field_t *f)
{
    const ui_state_t *st = ui_state();
    char buf[48];

    const int top = st->digit_top;
    const int16_t cap_y    = top ? FC_CAP_T       : FC_CAP_B;
    const int16_t link_far = top ? FC_LINK_FAR_T  : FC_LINK_FAR_B;
    const int16_t link_nr  = top ? FC_LINK_NEAR_T : FC_LINK_NEAR_B;
    const int16_t digit_y  = top ? FC_DIGIT_T     : FC_DIGIT_B;
    const int16_t reg_y    = top ? 0 : (int16_t)(BAND_Y + BAND_H);
    const int16_t reg_h    = top ? BAND_Y : (int16_t)(LCD_HEIGHT - BAND_Y - BAND_H);

    if (full) {
        /* Clear only the half the readout occupies. The band and the other row of cells belong
         * to both views and are drawn by the shared paths below, so clearing the whole panel
         * here would make them flash on every entry into a field. */
        ra8876_set_fg(C_BG);
        ra8876_fill_rect(0, reg_y, LCD_WIDTH, reg_h);
    }

    draw_legend(full);

    /* Anything that is not a plain number has no digit window to show, so the value itself is
     * the whole story. */
    if (f->kind != EMP_KIND_NUMBER) {
        ra8876_set_fg(C_BG);
        ra8876_fill_rect(0, digit_y, LCD_WIDTH, FC_DIGIT_H);
        value_text(f, buf, sizeof(buf));
        fit_text(&font_value, buf, LCD_WIDTH - 4 * MARGIN_X);
        text_draw_centred(&font_value, LCD_WIDTH / 2, (int16_t)(digit_y + 30),
                          buf, C_VALUE, C_BG);
        return;
    }

    digit_t d[MAX_DIGITS];
    int16_t w = 0;
    unsigned n = layout_digits(f, d, &w);
    int16_t x0 = (int16_t)((LCD_WIDTH - w) / 2);

    /* Captions and link lines repaint only when the window or the held knob changes, never on a
     * value change: clearing this band on each detent made the top third of the panel flash. */
    static int8_t  links_ws = -99, links_held = -99, links_top = -1;
    static int16_t links_x0 = -1;

    if (full || links_ws != st->ws || links_held != st->held_digit || links_x0 != x0
             || links_top != (int8_t)top) {
        int16_t clr_y = top ? cap_y : link_nr;
        int16_t clr_h = top ? (int16_t)(link_nr - cap_y)
                            : (int16_t)(cap_y + 28 - link_nr);
        ra8876_set_fg(C_BG);
        ra8876_fill_rect(0, clr_y, LCD_WIDTH, clr_h);

        for (unsigned k = 0; k < UI_DIGITS; k++) {
            int8_t e = (int8_t)(st->ws - (int8_t)k);
            int held = (st->held_digit == (int8_t)k);

            double place = 1.0;
            if (e >= 0) { for (int8_t i = 0; i < e; i++) place *= 10.0; }
            else        { for (int8_t i = 0; i > e; i--) place /= 10.0; }
            (void)text_format(buf, sizeof(buf), place, (uint8_t)(e < 0 ? -e : 0));

            text_draw_centred(&font_small, KNOB_CX(k), cap_y, buf,
                              held ? C_ACCENT : C_LABEL, C_BG);

            /* The line from the knob down to the digit it edits. Without it, the correspondence
             * between four identical dials and four positions in a number has to be memorised. */
            for (unsigned i = 0; i < n; i++) {
                if (d[i].knob != (int8_t)k) continue;
                int16_t dx = (int16_t)(x0 + d[i].x + text_width(&font_display, "0") / 2);

                /* Stub out from the knob, diagonal across, stub in to the digit. The shape is
                 * identical either way up; only the direction of travel flips. */
                int16_t knob_end  = top ? link_far : (int16_t)(link_far - 10);
                int16_t digit_end = top ? (int16_t)(link_nr - 10) : link_nr;

                ra8876_set_fg(held ? C_ACCENT : C_LINK);
                ra8876_fill_rect((int16_t)(KNOB_CX(k) - 1), knob_end, 2, 10);
                ra8876_line((int16_t)KNOB_CX(k), (int16_t)(top ? knob_end + 10 : knob_end),
                            dx, (int16_t)(top ? digit_end : digit_end + 10));
                ra8876_fill_rect((int16_t)(dx - 1), digit_end, 2, 10);
                break;
            }
        }
        links_ws   = st->ws;
        links_held = st->held_digit;
        links_x0   = x0;
        links_top  = (int8_t)top;
    }

    /* The number, one digit at a time. Repainting the whole band per change cost about 50 ms of
     * SPI and showed as the value flashing black on every detent; a knob changes one digit. */
    static char     last_ch[MAX_DIGITS];
    static uint16_t last_col[MAX_DIGITS];
    static unsigned last_n_digits;
    static int16_t  last_digit_x0 = -1;
    static int16_t  last_digit_y  = -1;

    uint8_t other = (uint8_t)(st->held_digit >= 0 ? 45u : 100u);

    int relayout = full || n != last_n_digits || x0 != last_digit_x0 || digit_y != last_digit_y;
    if (relayout) {
        ra8876_set_fg(C_BG);
        ra8876_fill_rect(0, digit_y, LCD_WIDTH, (int16_t)(text_height(&font_display) + 4));
        for (unsigned i = 0; i < MAX_DIGITS; i++) last_ch[i] = 0;
    }
    last_digit_y = digit_y;

    int16_t adv = text_width(&font_display, "0");

    for (unsigned i = 0; i < n; i++) {
        uint16_t c;
        if (d[i].grey)                                          c = C_DIM;
        else if (d[i].knob >= 0 && d[i].knob == st->held_digit) c = C_VALUE;
        else                                                    c = dim(C_VALUE, other);

        if (!relayout && last_ch[i] == d[i].ch && last_col[i] == c) continue;

        int16_t gx = (int16_t)(x0 + d[i].x);
        if (!relayout) {
            ra8876_set_fg(C_BG);
            ra8876_fill_rect(gx, digit_y, adv, (int16_t)(text_height(&font_display) + 4));
        }
        char one[2] = { d[i].ch, 0 };
        text_draw(&font_display, gx, digit_y, one, c, C_BG);

        last_ch[i]  = d[i].ch;
        last_col[i] = c;
    }
    last_n_digits = n;
    last_digit_x0 = x0;

    if (f->present & EMP_PRESENT_UNIT) {
        str_of(buf, sizeof(buf), f->unit_off, f->unit_len);
        text_draw(&font_small, (int16_t)(x0 + w + SP),
                  (int16_t)(digit_y + text_height(&font_display) - 34), buf, C_UNIT, C_BG);
    }

    if ((f->present & (EMP_PRESENT_MIN | EMP_PRESENT_MAX)) ==
        (EMP_PRESENT_MIN | EMP_PRESENT_MAX) && f->max > f->min) {
        int16_t bx = (int16_t)(LCD_WIDTH - 2 * MARGIN_X - 12), by = digit_y;
        int16_t bh = (int16_t)text_height(&font_display);
        ra8876_set_fg(C_TRACK);
        ra8876_fill_rect(bx, by, 12, bh);

        double t = (f->number - f->min) / (f->max - f->min);
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        int16_t fh = (int16_t)(t * (double)bh);
        ra8876_set_fg(C_ACCENT);
        ra8876_fill_rect(bx, (int16_t)(by + bh - fh), 12, fh);
    }
}

/* ------------------------------------------------------------------ entry point */

void ui_render(void)
{
    const ui_state_t *st = ui_state();
    int view = (st->focused >= 0) ? 1 : 0;
    int full = (view != last_view);

    if (view == 0) {
        render_overview(full);
    } else {
        const surf_field_t *f = surf_field((uint16_t)st->focused);
        if (!f) { render_overview(full); view = 0; }
        else {
            int changed = full
                       || st->focused    != last_focus
                       || st->ws         != last_ws
                       || st->held_digit != last_held
                       || f->number      != last_value
                       || st->page       != last_page;

            if (changed) {
                int rebuild = full || st->focused != last_focus;
                render_drilled(rebuild, f);
                if (rebuild) draw_page_strip(PAGE_Y);
                else         draw_legend(0);       /* undo/redo availability moves as you edit */
                render_cells(rebuild, ui_cell_row_first(),
                             ui_cell_row_first() + UI_DIGITS);

                last_focus = st->focused;
                last_ws    = st->ws;
                last_held  = st->held_digit;
                last_value = f->number;
                last_page  = st->page;
            } else {
                draw_legend(0);
                render_cells(0, ui_cell_row_first(), ui_cell_row_first() + UI_DIGITS);
            }
        }
    }

    if (view != last_view) {
        last_view = view;
        /* The two views draw over each other's regions, so a stale "nothing changed" would
         * leave fragments of the previous one on screen. */
        if (view == 0) last_focus = -2;
        else           memset(last_cell, 0, sizeof(last_cell));
    }
}

/* ------------------------------------------------------------------ demo scenes */

void ui_render_demo(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_render();
}

void ui_render_demo_focused(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_state_push(0, 1, 1000);           /* press the first knob: drill in */
    ui_state_push(0, 0, 1010);
    ui_state_touch(0x20, 1020);          /* a finger resting on the third digit knob */
    ui_render();
}

/* Drilled into a TOP-row field, so the digit editor is the BOTTOM row and the readout appears
 * below the band. The mirror of ui_render_demo_focused, and the reason both exist: the two
 * halves are laid out by the same code with the geometry flipped, so a mistake in one and not
 * the other is exactly what wants catching side by side. */
void ui_render_demo_focused_top(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_state_push(UI_ROW_SPLIT, 1, 1000);      /* press a top-row knob */
    ui_state_push(UI_ROW_SPLIT, 0, 1010);
    ui_state_touch(0x02, 1020);                /* a finger on bottom knob 1, now a digit knob */
    ui_render();
}

void ui_render_demo_page2(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_state_button(UI_BTN_PAGE_NEXT, 1, 1000);
    ui_render();
}
