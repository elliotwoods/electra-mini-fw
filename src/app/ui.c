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
 *  - **Only repaint what changed.** Full-screen repaint latency is recorded in graphics evidence. Every region tracks
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
#include "session.h"

#include "theme.h"
#include "graphics.h"

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
#define BAND_Y      (ROW_TOP_Y + ROW_H + SP)          /* 196 */
#define BAND_H      88
#define ROW_BOT_Y   (BAND_Y + BAND_H + SP)            /* 290 */

#define PAGE_Y      BAND_Y
#define LEGEND_Y    (BAND_Y + 36)
#define LEGEND_H    (BAND_H - 36)

/* Drilled view, contained inside the row it replaces.
 *
 * Drilling replaces the area belonging to whichever row became the digit editor, which is
 * always the row OPPOSITE the drilled field. So the readout appears beside the knobs that edit
 * it, the link lines stay short and point at the working hand, and the row holding the drilled
 * field keeps showing its cells so it can still be seen in context.
 *
 * The band never moves: the six buttons are physical and always mean the same thing, so their
 * legend staying put is worth more than the space it costs.
 *
 * Drill-specific ink must stay inside the bounding rectangle previously occupied by the four
 * control cards. The old geometry placed the bottom editor in the gap above its cards and put
 * the focused title in the middle strip. The two rows are exact screen mirrors once their real
 * bounds are used: top y=6..189 and bottom y=290..473. */
#define FC_X            MARGIN_X
#define FC_W            (LCD_WIDTH - 2 * MARGIN_X)

#define FC_CAP_T        ROW_TOP_Y
#define FC_LINK_FAR_T   34          /* the end nearest the knobs */
#define FC_LINK_NEAR_T  65          /* the end nearest the digits */
#define FC_DIGIT_T      69
#define FC_DIGIT_H      121

#define FC_DIGIT_B      (LCD_HEIGHT - FC_DIGIT_T - FC_DIGIT_H)
#define FC_LINK_NEAR_B  (LCD_HEIGHT - FC_LINK_NEAR_T)
#define FC_LINK_FAR_B   (LCD_HEIGHT - FC_LINK_FAR_T)
#define FC_CAP_B        (LCD_HEIGHT - FC_CAP_T - 28)

/* ------------------------------------------------------------------ helpers */

/* Short fills finish within the controller's bounded busy wait, including on view changes. */
static void clear_screen(void)
{
    gfx_set_fg(C_BG);
    for (int16_t y = 0; y < LCD_HEIGHT; y += 80)
        gfx_fill_rect(0, y, LCD_WIDTH, 80);
}

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
    gfx_border((gfx_rect_t){x,y,w,h}, 5, (uint8_t)t, c);
}

/* Switch position, state word and thumb symbol each identify the boolean independently.
 * Keep the word on the unoccupied half so the moving thumb never masks it. */
static void draw_bool_switch(int16_t x, int16_t y, int16_t w, int16_t h, int on,
                             const font_t *face)
{
    int16_t pad = (int16_t)(h / 10);
    int16_t diameter = (int16_t)(h - 2 * pad);
    int16_t tx = (int16_t)(on ? x + w - pad - diameter : x + pad);
    int16_t ty = (int16_t)(y + pad);
    gfx_rect_t track = {x,y,w,h};
    gfx_paint_t paint = gfx_solid(on ? dim(C_TOGGLE_ON,25) : C_TRACK,track);
    gfx_paint(track,paint,(uint8_t)(h/2));
    gfx_border(track,(uint8_t)(h/2),2,on ? C_TOGGLE_ON : C_INDICATOR);

    /* An inset dark rim separates the bright circular thumb from the track. */
    gfx_rect_t thumb = {tx,ty,diameter,diameter};
    gfx_paint(thumb,gfx_solid(C_BG,thumb),(uint8_t)(diameter/2));
    thumb=(gfx_rect_t){tx+2,ty+2,diameter-4,diameter-4};
    gfx_paint(thumb,gfx_solid(C_VALUE,thumb),(uint8_t)((diameter-4)/2));
    int16_t cx=(int16_t)(tx+diameter/2),cy=(int16_t)(ty+diameter/2);
    int16_t mark=(int16_t)(diameter/5),stroke=diameter>=64 ? 4 : 3;
    gfx_set_fg(C_BG);
    if(on) {
        for(int16_t i=0;i<stroke;i++) {
            gfx_line(cx-mark,cy+i,cx-mark/3,cy+mark/2+i);
            gfx_line(cx-mark/3,cy+mark/2+i,cx+mark,cy-mark/2+i);
        }
    } else gfx_fill_rect(cx-mark,cy-stroke/2,2*mark,stroke);

    int16_t word_cx=(int16_t)(on ? x+(w-pad-diameter)/2 : x+(w+pad+diameter)/2);
    text_draw_centred(face,word_cx,(int16_t)(y+(h-text_height(face))/2),
                      on ? "ON" : "OFF",C_VALUE,paint.first);
}

/* Pooled strings are not NUL-terminated on the wire. */
static void str_of(char *out, uint32_t cap, uint16_t off, uint16_t len)
{
    const char *s = surf_string(off, len);
    uint32_t n = (len < cap - 1u) ? len : cap - 1u;
    if (s && n) {
        if(len>=cap&&n>1u) {out[0]='~';memcpy(out+1,s+len-(n-1u),n-1u);}
        else memcpy(out,s,n);
    } else n = 0;
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

static float color_normal(const surf_field_t *f, float v)
{
    double lo = (f->present & EMP_PRESENT_MIN) ? f->min : 0.0;
    double hi = (f->present & EMP_PRESENT_MAX) ? f->max : 1.0;
    if (hi <= lo) return 0.0f;
    double n = ((double)v - lo) / (hi - lo);
    if (n < 0.0) n = 0.0;
    if (n > 1.0) n = 1.0;
    return (float)n;
}

static uint16_t color565(const surf_field_t *f)
{
    uint16_t r = (uint16_t)(color_normal(f, f->color[0]) * 31.0f + 0.5f);
    uint16_t g = (uint16_t)(color_normal(f, f->color[1]) * 63.0f + 0.5f);
    uint16_t b = (uint16_t)(color_normal(f, f->color[2]) * 31.0f + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Kind-aware, deliberately. Formatting every value as a number is what made the Lua bundle show
 * "Mode: 2" instead of "Mode: Additive". */
static void value_text(const surf_field_t *f, char *out, uint32_t cap)
{
    if (f->kind == EMP_KIND_ACTION) { if (cap >= 6u) memcpy(out, "PRESS", 6u); return; }
    if (f->value_tag == EMP_VAL_TEXT) {
        uint32_t n = (uint32_t)strlen(f->text); if (n >= cap) n = cap - 1u;
        memcpy(out, f->text, n); out[n] = 0; return;
    }
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

    case EMP_VAL_COLOR:
        if (cap > 7u) memcpy(out, "COLOUR", 7u);
        else out[0] = 0;
        break;

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

    if (f->kind == EMP_KIND_COLOR && f->color_count >= 3u) {
        float v = color_normal(f, f->color[0]);
        float g = color_normal(f, f->color[1]);
        float b = color_normal(f, f->color[2]);
        if (g > v) v = g;
        if (b > v) v = b;
        return (int16_t)(v * (float)span);
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
    uint8_t  active;
    uint8_t  remote;
    uint8_t  color_count;
    uint8_t  modified;
    uint8_t  style;          /* EMP/1.6 */
    uint8_t  tone;
    float    color[4];
    int16_t  bar_px;
    int16_t  hold_px;        /* EMP/1.7 fill extent, HOLD_SENT once fired */
} cell_cache_t;

static cell_cache_t last_cell[UI_FIELDS_PER_PAGE];
static int          last_view = -1;         /* -1 nothing drawn, 0 overview, 1 drilled */
static int32_t      last_focus = -2;
static uint16_t     last_page = 0xFFFF;
static double       last_value;
static uint32_t     last_choice;
static uint8_t      last_boolean, last_value_tag, last_tone;
static char         last_drill_text[96];
static uint8_t      last_color_count;
static float        last_color[4];
static int8_t       last_ws, last_held, last_zoom;
static uint8_t      last_legend[UI_BTN_COUNT];
static int          legend_valid;

static void forget_everything(void)
{
    memset(last_cell, 0, sizeof(last_cell));
    last_view    = -1;
    last_focus   = -2;
    last_page    = 0xFFFF;
    last_value_tag = 0xFFu;
    last_ws      = -99;
    last_held    = -99;
    last_zoom    = -1;
    legend_valid = 0;
}

void ui_reset(void)
{
    gfx_begin();
    forget_everything();
    clear_screen();
    /* Keep the visible frame intact; ui_render publishes the completed replacement. */
}

/* ------------------------------------------------------------------ page position */

static void draw_page_strip(int16_t y)
{
    gfx_set_fg(C_BG);
    gfx_fill_rect(0, y, LCD_WIDTH, 32);

    uint16_t total = surf_slot_span();
    uint16_t page  = ui_state()->page;
    if (!total) return;

    /* One fixed square per occupied slot, arranged in the same two rows of four as the panel. */
    uint16_t pages = (uint16_t)((total + UI_FIELDS_PER_PAGE - 1u) / UI_FIELDS_PER_PAGE);
    if (pages > 1u) {
        const int16_t block = 10;
        const int16_t gap = 4;
        const int16_t page_gap = 10;
        const int16_t group_w = (int16_t)(UI_ROW_SPLIT * block
                                + (UI_ROW_SPLIT - 1u) * gap);
        int16_t map_w = (int16_t)(pages * group_w + (pages - 1u) * page_gap);
        int16_t x0 = (int16_t)((LCD_WIDTH - map_w) / 2);
        int16_t y0 = (int16_t)(y + (32 - (2 * block + gap)) / 2);
        for (uint16_t i = 0; i < pages * UI_FIELDS_PER_PAGE; i++) {
            uint16_t pg = (uint16_t)(i / UI_FIELDS_PER_PAGE);
            uint16_t lane = (uint16_t)(i % UI_FIELDS_PER_PAGE);
            if (ui_state_field_for_page((uint16_t)(pg * UI_FIELDS_PER_PAGE), lane) < 0) continue;
            int16_t col = (int16_t)(lane % UI_ROW_SPLIT);
            int16_t row = lane < UI_ROW_SPLIT ? 0 : 1;
            int16_t x = (int16_t)(x0 + pg * (group_w + page_gap) + col * (block + gap));
            gfx_set_fg(pg == page / UI_FIELDS_PER_PAGE ? C_NAV : C_OUTLINE);
            gfx_fill_rect(x, (int16_t)(y0 + row * (block + gap)), block, block);
        }
    }

    /* The selected card remains visible while drilled and already carries the field name.
     * Repeating it here used the middle strip for drill-specific ink and crowded the page map. */
}

/* While Zoom is held the strip between the rows says so, in place of the page map: the dials have
 * stopped editing, and the panel must not look as though they still do. */
static void draw_zoom_banner(int16_t y)
{
    gfx_set_fg(C_BG);
    gfx_fill_rect(0, y, LCD_WIDTH, 32);
    text_draw_centred(&font_small, LCD_WIDTH / 2, (int16_t)(y + (32 - text_height(&font_small)) / 2),
                      "ZOOM  -  turn any dial to move the digit window", C_ACCENT, C_BG);
}

/* ------------------------------------------------------------------ button legend */

/* What the six buttons do, drawn where they are. Irrelevant controls are absent, not disabled. */
static void draw_legend(int full)
{
    /* The drilled set; the overview swaps in page arrows and settings below. */
    static const char *const glyph[UI_BTN_COUNT] = {
        ICON_EXIT, ICON_UNDO, ICON_REDO, ICON_STORE, ICON_RESET, ICON_ZOOM
    };

    /* Two bits per button: whether it would act, and whether it is being held. Packed together
     * so one memcmp decides the whole strip, which keeps a still panel free. */
    uint8_t want[UI_BTN_COUNT];
    for (unsigned b = 0; b < UI_BTN_COUNT; b++) {
        int enabled = ui_state_button_enabled(b);
        want[b] = enabled ? (uint8_t)(1u | ((ui_state()->btn_mask & (1u << b)) ? 2u : 0u)) : 0u;
    }

    if (!full && legend_valid && memcmp(want, last_legend, sizeof(want)) == 0) return;

    int16_t w = (LCD_WIDTH - 2 * MARGIN_X - (int16_t)(UI_BTN_COUNT - 1) * SP)
              / (int16_t)UI_BTN_COUNT;

    for (unsigned b = 0; b < UI_BTN_COUNT; b++) {
        int16_t x = (int16_t)(MARGIN_X + (int16_t)b * (w + SP));
        int live = (want[b] & 1u) != 0;
        int down = (want[b] & 2u) != 0;

        if (!live) {
            gfx_set_fg(C_BG);
            gfx_fill_rect(x, LEGEND_Y, w, LEGEND_H);
            continue;
        }

        uint16_t bg = down ? C_CELL_DOWN : C_CELL;
        gfx_set_fg(bg);
        gfx_fill_rect(x, LEGEND_Y, w, LEGEND_H);
        frame(x, LEGEND_Y, w, LEGEND_H, 1, down ? C_NAV : C_OUTLINE);

        const char *icon = glyph[b];
        const char *label = 0;
        if (ui_state()->mode == UI_MODE_CAL_SELECT) {
            if (b == UI_BTN_BACK) label = "BACK";
            else if (b == UI_BTN_SYSTEM) label = "ALL";
        } else if (ui_state()->mode == UI_MODE_CAL_RUN) {
            if (b == UI_BTN_BACK) label = "CANCEL";
            else if (b == UI_BTN_SYSTEM && ui_state()->cal_phase == 7u) label = "RETRY";
            else if (b == UI_BTN_SYSTEM && ui_state()->cal_phase == 6u) label = "DONE";
        } else if (ui_state()->mode == UI_MODE_BRIGHTNESS) {
            if (b == UI_BTN_BACK) label = "CANCEL";
            else if (b == UI_BTN_SYSTEM) label = "DONE";
        }
        if (ui_state()->mode == UI_MODE_SURFACE && ui_state()->focused < 0) {
            if (b == UI_BTN_BACK) icon = ICON_PREV;
            else if (b == UI_BTN_SYSTEM) icon = ICON_SETTINGS;
            else if (b == UI_BTN_PAGE_NEXT) icon = ICON_NEXT;
        }
        const font_t *face = label ? &font_small : &font_icons;
        text_draw_centred(face, (int16_t)(x + w / 2),
                          (int16_t)(LEGEND_Y + (LEGEND_H - text_height(face)) / 2),
                          label ? label : icon, C_ICON, bg);
    }

    memcpy(last_legend, want, sizeof(want));
    legend_valid = 1;
}

/* ------------------------------------------------------------------ overview */

static void cell_geometry(unsigned knob, int16_t *x, int16_t *y)
{
    *x = CELL_X(knob % COLS);
    *y = (knob < UI_ROW_SPLIT) ? ROW_TOP_Y : ROW_BOT_Y;
}

static uint16_t cell_frame_color(int touched, int pressed, int drilled, int active)
{
    if (drilled || pressed) return C_NAV;
    if (active || touched) return C_ACCENT;
    return C_OUTLINE;
}

/* Match the browser: local activity is solid cyan, mirrored activity dashed cyan. */
static void activity_outline(int16_t x, int16_t y, int16_t w, int16_t h, int local, int remote)
{
    if (local) { frame(x,y,w,h,2,C_ACCENT); return; }
    if (!remote) return;
    gfx_set_fg(C_ACCENT);
    for (int16_t a=6; a<w-6; a+=14) {
        int16_t n=(int16_t)(w-6-a); if(n>8)n=8;
        gfx_fill_rect(x+a,y,n,2); gfx_fill_rect(x+a,y+h-2,n,2);
    }
    for (int16_t a=6; a<h-6; a+=14) {
        int16_t n=(int16_t)(h-6-a); if(n>8)n=8;
        gfx_fill_rect(x,y+a,2,n); gfx_fill_rect(x+w-2,y+a,2,n);
    }
}

/* `bg` is the cell's own card colour (C_STOP for an EMP/1.6 stop button), so restoring the
 * inset after a press puts back red rather than painting grey stripes across the stop. */
static void draw_cell_frame(unsigned knob, uint16_t bg, int touched, int pressed, int drilled,
                            int active, int remote)
{
    int16_t x, y;
    cell_geometry(knob, &x, &y);
    frame(x, y, CELL_W, ROW_H, 2, cell_frame_color(touched, pressed, drilled, active));
    activity_outline(x,y,CELL_W,ROW_H,touched || active,remote);
    gfx_set_fg(bg); gfx_fill_rect(x + 7, y + 3, CELL_W - 14, 3);
    if (touched || active) { gfx_set_fg(C_LIVE); gfx_fill_rect(x + CELL_W/2 - 14, y + 3, 28, 3); }
    if (pressed) frame(x + 4, y + 4, CELL_W - 8, ROW_H - 8, 1, C_INDICATOR);
    else { /* Restore the inset when a switch releases. */
        gfx_restore((gfx_rect_t){x+4,y+4,CELL_W-8,1},bg);
        gfx_restore((gfx_rect_t){x+4,y+ROW_H-5,CELL_W-8,1},bg);
        gfx_restore((gfx_rect_t){x+4,y+5,1,ROW_H-10},bg);
        gfx_restore((gfx_rect_t){x+CELL_W-5,y+5,1,ROW_H-10},bg);
        /* ...and its rounded corners, which the four straight strips miss. A drilled field is
         * repainted whole, so this only showed after an Action press, which never drills. */
        gfx_restore((gfx_rect_t){x+4,y+4,6,6},bg);
        gfx_restore((gfx_rect_t){x+CELL_W-10,y+4,6,6},bg);
        gfx_restore((gfx_rect_t){x+4,y+ROW_H-10,6,6},bg);
        gfx_restore((gfx_rect_t){x+CELL_W-10,y+ROW_H-10,6,6},bg);
    }
    if (drilled) { gfx_set_fg(C_NAV); gfx_fill_rect(x+3,y+12,3,ROW_H-24); }
    else gfx_restore((gfx_rect_t){x+3,y+12,3,ROW_H-24},bg);
}

/* EMP/1.6 estop: the whole cell is the operator layout's stop plate, labelled with the field's
 * own label. It fires on knob press like any Action; nothing about it is turnable. */
static void draw_estop_face(unsigned knob, const surf_field_t *f)
{
    int16_t x, y;
    cell_geometry(knob, &x, &y);
    const uint16_t bg = C_STOP;
    gfx_restore((gfx_rect_t){(int16_t)(x + 8), (int16_t)(y + 8),
                             (int16_t)(CELL_W - 16), (int16_t)(ROW_H - 16)}, bg);

    char label[48];
    str_of(label, sizeof(label), f->label_off, f->label_len);
    if (!label[0]) memcpy(label, "STOP", 5u);
    fit_text(&font_ui, label, (int16_t)(CELL_W - 4 * SP));
    int16_t ly = (int16_t)(y + ROW_H / 2 - text_height(&font_ui));
    text_draw_centred(&font_ui, (int16_t)(x + CELL_W / 2), ly, label, C_VALUE, bg);
    text_draw_centred(&font_small, (int16_t)(x + CELL_W / 2),
                      (int16_t)(ly + text_height(&font_ui) + 2 * SP), "PRESS", C_VALUE, bg);
}

/* EMP/1.7 hold-to-fire. The fill is quantised to pixels so a held knob repaints only when the bar
 * visibly grows; HOLD_SENT marks the fired state, which is drawn full and green. */
#define HOLD_SENT 0x7FFF
static int16_t hold_extent(int32_t slot, int16_t span)
{
    uint16_t pm = ui_state_hold_permille(slot);
    if (pm >= 1000u) return HOLD_SENT;
    return (int16_t)((int32_t)span * pm / 1000);
}

/* A meter track with a solid fill: the hold's colour means something (amber = not yet, green =
 * sent), so it does not take the usual meter's gradient into teal. */
static void hold_meter(gfx_rect_t r, int16_t extent, uint16_t colour)
{
    gfx_meter(r, 0, 1, colour);
    if (extent > r.w - 4) extent = (int16_t)(r.w - 4);
    if (extent <= 0) return;
    gfx_set_fg(colour);
    gfx_fill_rect((int16_t)(r.x + 2), (int16_t)(r.y + 2), extent, (int16_t)(r.h - 4));
}

/* The label above as usual; under it a caption, and across the foot of the cell a fill that
 * grows while the knob is held. Plain ink at rest, amber while holding, green "DONE" once the
 * ACTIVATE has gone, until the knob is released. Nothing here is a stop plate. */
static void draw_hold_face(unsigned knob, const surf_field_t *f)
{
    int16_t x, y;
    cell_geometry(knob, &x, &y);
    const uint16_t bg = C_CELL;
    gfx_set_fg(bg);
    gfx_fill_rect((int16_t)(x + 2), (int16_t)(y + 70), (int16_t)(CELL_W - 4), (int16_t)(ROW_H - 72));

    const int16_t bx = (int16_t)(x + 2 * SP), bw = (int16_t)(CELL_W - 4 * SP), bh = 16;
    const int16_t by = (int16_t)(y + ROW_H - 2 * SP - bh);
    int16_t ext = hold_extent(f->id, (int16_t)(bw - 4));
    int sent = ext == HOLD_SENT;
    const char *caption = sent ? "DONE" : "HOLD";
    uint16_t ink = sent ? C_OK : ext > 0 ? C_WARN : C_VALUE;
    int16_t vy = (int16_t)(y + 14 + ROW_H / 2 - text_height(&font_value) / 2);
    text_draw(&font_value, bx, vy, caption, ink, bg);
    hold_meter((gfx_rect_t){bx, by, bw, bh}, sent ? (int16_t)(bw - 4) : ext, sent ? C_OK : C_WARN);
}

/* A value away from its default carries the screen's reset glyph in the cell's free bottom-left
 * corner, and its readout takes the accent. On screen the same state shows the reset/store
 * column beside the control, so one look at either says which values somebody has changed. */
static void draw_modified_badge(int16_t x, int16_t y)
{
    /* Bottom right, beside the fader: the one corner no value, unit or switch reaches. */
    int16_t right = (int16_t)(x + CELL_W - 2 * SP - 12 - SP);
    text_draw_right(&font_icons, right, (int16_t)(y + ROW_H - text_height(&font_icons) - 2),
                    ICON_RESET, C_ACCENT, C_CELL);
}

/* Clear and repaint only the value region. Keeping the label and cell background untouched is
 * what prevents title strobing while a rotary is producing a fast stream of value updates. */
static void draw_cell_dynamic(unsigned knob, const surf_field_t *f)
{
    int16_t x, y;
    cell_geometry(knob, &x, &y);
    const int16_t h = ROW_H;
    const uint16_t bg = C_CELL;

    if (surf_field_is_estop(f)) { draw_estop_face(knob, f); return; }
    if (surf_field_is_hold(f))  { draw_hold_face(knob, f);  return; }

    gfx_set_fg(bg);
    gfx_fill_rect((int16_t)(x + 2), (int16_t)(y + 70),
                     (int16_t)(CELL_W - 4), (int16_t)(h - 72));

    if (!f) {
        text_draw_centred(&font_ui, (int16_t)(x + CELL_W / 2),
                          (int16_t)(y + h / 2 - 20), "--", C_OUTLINE, bg);
        return;
    }

    char buf[48];
    int16_t bw = 12;
    int16_t bx = (int16_t)(x + CELL_W - 2 * SP - bw);
    int16_t avail = (int16_t)(bx - SP - (x + 2 * SP));

    /* The value on the cell's centre line, left-aligned: a column of numbers sharing a left edge
     * is far easier to compare down the panel than one that shifts with every digit count. */
    int16_t vy = (int16_t)(y + 14 + h / 2 - text_height(&font_value) / 2);
    value_text(f, buf, sizeof(buf));
    int modified = surf_field_modified(f);
    if (modified) draw_modified_badge(x, y);

    if (f->value_tag == EMP_VAL_BOOL) {
        const int16_t sw = 148, sh = 54;
        draw_bool_switch((int16_t)(x + (CELL_W - sw) / 2),
                       (int16_t)(vy - (sh - text_height(&font_ui)) / 2),
                       sw, sh, f->boolean, &font_ui);
        return;                         /* the switch replaces the numeric side fader */
    }

    if (f->value_tag == EMP_VAL_COLOR && f->color_count >= 3u) {
        int16_t sw = (int16_t)(CELL_W - 4 * SP);
        int16_t sh = 54;
        int16_t sx = (int16_t)(x + 2 * SP);
        int16_t sy = (int16_t)(vy - (sh - text_height(&font_ui)) / 2);
        gfx_checker((gfx_rect_t){sx,sy,sw,sh},color565(f),
                    f->color_count==4 ? (uint8_t)(255*color_normal(f,f->color[3])) : 255u);
        frame(sx, sy, sw, sh, 2, C_OUTLINE);
        return;
    }

    /* Numbers get the monospaced face so digits do not shift as they change; words get the
     * proportional one, since monospacing "Sawtooth" buys nothing and costs the width. */
    const font_t *vf = (f->value_tag == EMP_VAL_NUMBER) ? &font_value : &font_ui;
    if (text_width(vf, buf) > avail) vf = (f->value_tag == EMP_VAL_NUMBER) ? &font_compact : &font_small;
    fit_text(vf, buf, avail);

    uint16_t vc = ui_value_color(f, modified);       /* EMP/1.6 tone, else the usual ink */
    if (f->kind == EMP_KIND_READONLY) {
        text_draw_right(&font_small, x + CELL_W - 2*SP, y + h - 30, "READ ONLY", C_UNIT, bg);
    }
    text_draw(vf, (int16_t)(x + 2 * SP), vy, buf, vc, bg);

    if (f->present & EMP_PRESENT_UNIT) {
        str_of(buf, sizeof(buf), f->unit_off, f->unit_len);
        fit_text(&font_small, buf, avail);
        text_draw(&font_small, (int16_t)(x + 2 * SP),
                  (int16_t)(vy + text_height(vf) - 4), buf, C_UNIT, bg);
    }

    /* A fader down the cell's right edge: it uses the height, four of them side by side read as
     * a set, and it matches the range bar in the drilled view. */
    int16_t by = (int16_t)(y + 70);
    int16_t bh = (int16_t)(h - 70 - 2 * SP);
    int16_t ext = bar_extent(f, bh);

    gfx_meter((gfx_rect_t){bx,by,bw,bh}, ext, 0, ui_meter_color(f));
    if (ext < 0) {
        gfx_set_fg(C_INDICATOR);gfx_fill_rect(bx+2, by+bh/2-2, bw-4, 4);
    }

}

/* Preserve identifying suffixes (Offset X/Y, frequency units) before truncating a title.
 * Labels get two lines, independent of the value repaint region below y+70. */
static void draw_cell_label(int16_t x, int16_t y, char *label)
{
    const int16_t width = CELL_W - 4 * SP;
    const font_t *faces[] = { &font_ui, &font_small };
    for (unsigned face = 0; face < 2; face++) {
        const font_t *font = faces[face];
        if (text_width(font, label) <= width) {
            text_draw(font, x, y, label, C_TITLE, C_CELL);
            return;
        }
        for (unsigned split = (unsigned)strlen(label); split > 0; split--) {
            if (label[split] != ' ') continue;
            label[split] = 0;
            int fits = text_width(font, label) <= width
                    && text_width(font, label + split + 1) <= width;
            if (fits) {
                text_draw(font, x, y, label, C_TITLE, C_CELL);
                text_draw(font, x, y + text_height(font), label + split + 1, C_TITLE, C_CELL);
                return;
            }
            label[split] = ' ';
        }
    }
    /* Keep the identifying suffix when even two lines cannot fit. */
    unsigned length=(unsigned)strlen(label),tail=length>8u?length-8u:0u;
    char suffix[12];suffix[0]='~';memcpy(suffix+1,label+tail,length-tail+1u);
    while(tail>0&&text_width(&font_small,suffix)>width){tail++;suffix[0]='~';memcpy(suffix+1,label+tail,length-tail+1u);}
    text_draw(&font_small, x, y, suffix, C_TITLE, C_CELL);
}

static void draw_cell(unsigned knob, const surf_field_t *f,
                      int touched, int pressed, int drilled, int active, int remote)
{
    int16_t x, y;
    cell_geometry(knob, &x, &y);
    const uint16_t bg = ui_cell_background(f);
    if (bg == C_STOP) {
        /* Always as a card: the red plate is a real gradient, and a fill over an already
         * registered one would paint it square, corners included. */
        gfx_card((gfx_rect_t){x, y, CELL_W, ROW_H}, bg);
    } else {
        gfx_set_fg(bg);
        gfx_fill_rect(x, y, CELL_W, ROW_H);
    }

    if (f && !surf_field_is_estop(f)) {          /* a stop carries its label on its face */
        char label[48];
        str_of(label, sizeof(label), f->label_off, f->label_len);
        draw_cell_label((int16_t)(x + 2 * SP), (int16_t)(y + SP), label);
    }
    draw_cell_dynamic(knob, f);
    draw_cell_frame(knob, bg, touched, pressed, drilled, active, remote);
}

/* Draw the cells in [first, last). The overview draws all eight; a drilled view draws the row
 * that still owns ordinary fields while the opposite row edits the focused value. */
static void render_cells(int full, unsigned first, unsigned last)
{
    static char last_text[8][96];
    const ui_state_t *st = ui_state();

    for (unsigned k = first; k < last; k++) {
        int32_t idx = ui_state_field_for(k);
        const surf_field_t *f = (idx >= 0) ? surf_field((uint16_t)idx) : 0;
        int touched = (st->touch_mask & (1u << k)) != 0;
        int pressed = (st->press_mask & (1u << k)) != 0;
        int active = st->active_pot == (int8_t)k;
        int remote = idx >= 0 && (st->remote_highlights & (UINT64_C(1) << idx)) != 0;

        uint8_t drilled = (uint8_t)(idx >= 0 && idx == st->focused);
        int static_dirty = full || !last_cell[k].valid
                        || last_cell[k].id != (f ? f->id : 0xFFFFu)
                        || last_cell[k].tag != (f ? f->value_tag : 0u)
                        || last_cell[k].style != (f ? f->style : 0u);
        int chrome_dirty = static_dirty
                        || last_cell[k].touched != (uint8_t)touched
                        || last_cell[k].pressed != (uint8_t)pressed
                        || last_cell[k].drilled != drilled
                        || last_cell[k].active != (uint8_t)active
                        || last_cell[k].remote != (uint8_t)remote;

        int16_t bh = (int16_t)(ROW_H - 48 - 2 * SP);
        int value_dirty = static_dirty;
        if (f && !value_dirty) {
            value_dirty = strcmp(last_text[k], f->text) != 0 || last_cell[k].boolean != f->boolean
                       || last_cell[k].choice  != f->choice
                       || last_cell[k].number  != f->number
                       || last_cell[k].color_count != f->color_count
                       || last_cell[k].modified != (uint8_t)surf_field_modified(f)
                       || last_cell[k].tone != f->tone
                       || memcmp(last_cell[k].color, f->color, sizeof(f->color)) != 0
                       || last_cell[k].bar_px  != bar_extent(f, bh)
                       || last_cell[k].hold_px != hold_extent(idx, CELL_W - 4 * SP - 4);
        }
        if (!static_dirty && !chrome_dirty && !value_dirty) continue;
        /* A HOLD face repaints while its knob is down, and its value region runs under the pressed
         * inset; put the chrome back over it. */
        if (value_dirty && surf_field_is_hold(f)) chrome_dirty = 1;

        if (static_dirty)
            draw_cell(k, f, touched, pressed, drilled, active, remote);
        else {
            if (value_dirty) draw_cell_dynamic(k, f);
            if (chrome_dirty) draw_cell_frame(k, ui_cell_background(f), touched, pressed, drilled,
                                              active, remote);
        }

        last_cell[k].valid   = 1;
        last_cell[k].touched = (uint8_t)touched;
        last_cell[k].pressed = (uint8_t)pressed;
        last_cell[k].drilled = drilled;
        last_cell[k].active  = (uint8_t)active;
        last_cell[k].remote  = (uint8_t)remote;
        last_cell[k].id      = f ? f->id : 0xFFFF;
        if (f) memcpy(last_text[k], f->text, sizeof(f->text)); else last_text[k][0] = 0;
        last_cell[k].tag     = f ? f->value_tag : 0;
        last_cell[k].number  = f ? f->number : 0;
        last_cell[k].choice  = f ? f->choice : 0;
        last_cell[k].boolean = f ? f->boolean : 0;
        last_cell[k].color_count = f ? f->color_count : 0;
        last_cell[k].modified = (uint8_t)(f ? surf_field_modified(f) : 0);
        last_cell[k].style   = f ? f->style : 0;
        last_cell[k].tone    = f ? f->tone : 0;
        if (f) memcpy(last_cell[k].color, f->color, sizeof(f->color));
        else memset(last_cell[k].color, 0, sizeof(last_cell[k].color));
        last_cell[k].bar_px  = f ? bar_extent(f, bh) : 0;
        last_cell[k].hold_px = hold_extent(idx, CELL_W - 4 * SP - 4);
    }
}

/* Operator overview: commands at the physical top knobs, large observed readout,
 * settings at the bottom knobs. Opt-in descriptor flag; other applications keep the grid.
 * Each region repaints only on change, so telemetry never flashes the whole panel. */
static void render_operator(int full)
{
    static char previous[8][96];
    static uint8_t previous_tone[8];
    static int16_t previous_hold[8];
    static uint16_t previous_press,previous_touch;
    static uint64_t previous_remote;
    const ui_state_t *st = ui_state();
    const uint16_t ground = C_BG, panel = C_CELL, ink = C_VALUE, muted = C_TITLE;
    const uint16_t stop_bg = C_STOP;
    if (full) {
        gfx_set_fg(ground);
        for (int16_t y = 0; y < 480; y += 80) gfx_fill_rect(0, y, 800, 80);
        memset(previous, 0xff, sizeof(previous)); previous_press = previous_touch = 0xffff;
        memset(previous_tone, 0xff, sizeof(previous_tone));
        text_draw(&font_small, 24, 114, "MANUAL CONTROL", muted, ground);
        text_draw_right(&font_small, 776, 114, "1 / 2", muted, ground);
        text_draw(&font_small, 24, 318, "ACTUAL POSITION / rev", muted, ground);
        text_draw_right(&font_small, 776, 318, "READ ONLY", muted, ground);
    }
    for (unsigned k = 0; k < 8; ++k) {
        const surf_field_t *f = surf_field((uint16_t)k);
        char value[96] = {0}; if (f) value_text(f, value, sizeof(value));
        int pressed = (st->press_mask & (1u << (k ^ 4u))) != 0;
        int touched = (st->touch_mask & (1u << (k ^ 4u))) != 0;
        uint8_t tone = f ? f->tone : 0u;
        int16_t hold = surf_field_is_hold(f) ? hold_extent((int32_t)k, 156) : 0;
        if (!full && strcmp(previous[k], value) == 0 && previous_tone[k] == tone &&
            previous_hold[k] == hold &&
            ((previous_press ^ st->press_mask) & (1u << (k ^ 4u))) == 0 &&
            ((previous_touch ^ st->touch_mask) & (1u << (k ^ 4u))) == 0 &&
            !((previous_remote ^ st->remote_highlights) & (UINT64_C(1) << k))) continue;
        memcpy(previous[k], value, sizeof(value)); previous_tone[k] = tone; previous_hold[k] = hold;
        if (k >= 4) {
            int16_t x = (int16_t)(16 + (k - 4) * 196);
            int stop = k == 7 || surf_field_is_estop(f);     /* EMP/1.6 estop takes the same plate */
            uint16_t bg = stop ? stop_bg : panel;
            if (pressed) bg = stop ? C_STOP : C_CELL_DOWN;
            gfx_card((gfx_rect_t){x,12,184,88},bg);
            char label[48] = {0}; if (f) str_of(label, sizeof(label), f->label_off, f->label_len);
            /* Whole words on two lines beat truncating the action an operator will execute. */
            if (k == 7) { text_draw_centred(&font_ui, x + 92, 22, "STOP", ink, bg);
                text_draw_centred(&font_small, x + 92, 60, "PRESS / ESCAPE", ink, bg); }
            else if (surf_field_is_hold(f)) {             /* EMP/1.7: caption and a fill */
                fit_text(&font_ui, label, 164);
                text_draw_centred(&font_ui, x + 92, 22, label, ink, bg);
                int sent = hold == HOLD_SENT;
                text_draw_centred(&font_small, x + 92, 56, sent ? "DONE" : "HOLD",
                                  sent ? C_OK : hold > 0 ? C_WARN : muted, bg);
                hold_meter((gfx_rect_t){(int16_t)(x + 12), 80, 160, 12}, sent ? 156 : hold,
                           sent ? C_OK : C_WARN);
            }
            else { fit_text(&font_ui, label, 164);
                text_draw_centred(&font_ui, x + 92, 22, label, ink, bg);
                text_draw_centred(&font_small, x + 92, 60, "PRESS", stop ? ink : muted, bg); }
        } else if (k == 3) {
            char *status = strchr(value, '\n');
            if (status) {
                *status++ = 0;
                gfx_set_fg(ground); gfx_fill_rect(24, 114, 660, 30);
                fit_text(&font_small, status, 660);
                text_draw(&font_small, 24, 114, status, muted, ground);
            }
            gfx_set_fg(ground); gfx_fill_rect(24, 157, 752, 154);
            int numeric = value[0] && (value[0] == '-' || (value[0] >= '0' && value[0] <= '9'));
            const font_t *face = numeric ? &font_display : &font_ui;
            if (text_width(face, value) > 740) face = &font_value;
            fit_text(face, value, 740);
            text_draw(face, 24, numeric ? 158 : 208, value, f ? ui_value_color(f, 0) : ink, ground);
            gfx_card((gfx_rect_t){604,366,180,102},panel);
            text_draw(&font_small,616,374,"MORE SETTINGS",muted,panel);
            text_draw(&font_ui,616,414,"PRESS  >",ink,panel);
        } else {
            int16_t x = (int16_t)(16 + k * 196);
            gfx_card((gfx_rect_t){x,366,184,102},panel);
            const char *label = k == 0 ? "AXIS" : k == 1 ? "STEP / rev" : "SPEED / ustep/s";
            text_draw(&font_small, x + 12, 374, label, muted, panel);
            const font_t *face = k == 0 ? &font_ui : &font_value;
            fit_text(face, value, 160);
            text_draw(face, x + 12, 411, value, ink, panel);
        }
        int16_t px=(int16_t)(16+(k>=4?k-4:k)*196),py=k>=4?12:366;
        int16_t pw=k==3?180:184,ph=k>=4?88:102;
        activity_outline(px,py,pw,ph,touched,(st->remote_highlights & (UINT64_C(1) << k)) != 0);
        if (k==3) activity_outline(24,157,752,154,touched,(st->remote_highlights & (UINT64_C(1) << 3)) != 0);
        if(touched){gfx_set_fg(C_LIVE);gfx_fill_rect(px+pw/2-20,py+3,40,3);}
        if(pressed)frame(px+4,py+4,pw-8,ph-8,2,C_INDICATOR);
    }
    previous_press = st->press_mask;previous_touch=st->touch_mask;
    previous_remote = st->remote_highlights;
}

/* Settings stay on the same visual scale and never take over the Stop knob. */
static void render_operator_settings(int full)
{
    static char previous[8][96];
    static uint8_t previous_tone[8];
    static uint16_t previous_press,previous_touch;
    static uint64_t previous_remote;
    const ui_state_t *st = ui_state();
    const uint16_t ground = C_BG, panel = C_CELL, ink = C_VALUE, muted = C_TITLE;
    if (full) {
        gfx_set_fg(ground);
        for (int16_t y = 0; y < 480; y += 80) gfx_fill_rect(0, y, 800, 80);
        memset(previous, 0xff, sizeof(previous)); previous_press = previous_touch = 0xffff;
        memset(previous_tone, 0xff, sizeof(previous_tone));
        text_draw(&font_small, 24, 114, "MOTION SETTINGS", muted, ground);
        text_draw_right(&font_small, 776, 114, "2 / 2", muted, ground);
        text_draw(&font_small, 24, 318, "BUTTON 1: BACK TO MANUAL CONTROL", muted, ground);
    }
    for (unsigned k = 0; k < 8; ++k) {
        const surf_field_t *f = surf_field((uint16_t)(8 + k));
        char value[96] = {0}; if (f) value_text(f, value, sizeof(value));
        uint16_t bit=(uint16_t)(1u<<(k^4u));
        int pressed=(st->press_mask&bit)!=0,touched=(st->touch_mask&bit)!=0;
        uint8_t tone = f ? f->tone : 0u;
        if (!full && strcmp(previous[k], value) == 0 && previous_tone[k] == tone && !((previous_press ^ st->press_mask) & bit) && !((previous_touch ^ st->touch_mask)&bit) &&
            !((previous_remote ^ st->remote_highlights) & (UINT64_C(1) << (8+k)))) continue;
        memcpy(previous[k], value, sizeof(value)); previous_tone[k] = tone;
        if (k < 4) {
            int16_t x = (int16_t)(16 + k * 196);
            const char *labels[] = { "COMMAND VIA", "ACCEL / ustep/s2", "MIN SPEED", "TARGET / rev" };
            gfx_card((gfx_rect_t){x,366,184,102},panel);
            text_draw(&font_small, x + 12, 374, labels[k], muted, panel);
            const font_t *face = k == 0 ? &font_ui : &font_value;
            fit_text(face, value, 160); text_draw(face, x + 12, 411, value, ink, panel);
        } else if (k == 4 || k == 7) {
            int16_t x = k == 4 ? 16 : 604;
            uint16_t bg = k == 7 ? C_STOP : panel;
            if (st->press_mask & (1u << (k ^ 4u))) bg = k == 7 ? C_STOP : C_CELL_DOWN;
            gfx_card((gfx_rect_t){x,12,184,88},bg);
            text_draw_centred(&font_ui, x + 92, 22, k == 4 ? "Move" : "STOP", ink, bg);
            text_draw_centred(&font_small, x + 92, 60, k == 4 ? "TO TARGET" : "PRESS / ESCAPE", ink, bg);
        } else if (k == 5) {
            gfx_set_fg(ground); gfx_fill_rect(212, 12, 380, 88);
            text_draw(&font_small, 224, 24, "COMMAND ROUTE", muted, ground);
            int connected = f && ((f->value_tag == EMP_VAL_BOOL && f->boolean) ||
                (f->value_tag == EMP_VAL_TEXT && strcmp(f->text, "true") == 0));
            text_draw(&font_ui, 224, 56, connected ? "Connected" : "Disconnected", connected ? C_LIVE : C_TITLE, ground);
        } else {
            gfx_set_fg(ground); gfx_fill_rect(24, 170, 752, 124);
            text_draw(&font_small, 24, 170, "OPERATION", muted, ground);
            fit_text(&font_ui, value, 744);
            text_draw(&font_ui, 24, 218, value, f ? ui_value_color(f, 0) : ink, ground);
        }
        if(k==5) activity_outline(212,12,380,88,touched,(st->remote_highlights & (UINT64_C(1) << 13)) != 0);
        if(k==6) activity_outline(24,170,752,124,touched,(st->remote_highlights & (UINT64_C(1) << 14)) != 0);
        if(k<4||k==4||k==7) {
            int16_t px=(int16_t)(16+(k>=4?k-4:k)*196),py=k>=4?12:366,ph=k>=4?88:102;
            activity_outline(px,py,184,ph,touched,(st->remote_highlights & (UINT64_C(1) << (8+k))) != 0);
            if(touched){gfx_set_fg(C_LIVE);gfx_fill_rect(px+72,py+3,40,3);}
            if(pressed)frame(px+4,py+4,176,ph-8,2,C_INDICATOR);
        }
    }
    previous_press = st->press_mask;previous_touch=st->touch_mask;
    previous_remote = st->remote_highlights;
}

static void render_overview(int full)
{
    const ui_state_t *st = ui_state();
    static int was_operator = -1;
    int operator_page = surf_operator_layout() && st->page < 16 ? (int)(st->page / 8 + 1) : 0;
    if (operator_page != was_operator) full = 1;
    was_operator = operator_page;
    if (operator_page == 1) { render_operator(full); return; }
    if (operator_page == 2) { render_operator_settings(full); return; }

    if (full) {
        clear_screen();
    }
    if (full || st->page != last_page) {
        draw_page_strip(PAGE_Y);
        last_page = st->page;
    }
    draw_legend(full);
    render_cells(full, 0, UI_FIELDS_PER_PAGE);
}

/* ------------------------------------------------------------------ local system views */

static void system_fact(int16_t x, int16_t y, const char *label, const char *value, const font_t *face)
{
    text_draw(&font_small, x, y, label, C_TITLE, C_BG);
    char copy[80];
    strncpy(copy, value, sizeof(copy) - 1); copy[sizeof(copy) - 1] = 0;
    fit_text(face, copy, 352);
    text_draw(face, x, (int16_t)(y + 28), copy, C_VALUE, C_BG);
}

static void draw_system_footer(int full)
{
    const ui_state_t *s = ui_state();
    static uint16_t old_buttons = 0xffff;
    static int old_mode = -1;
    if (!full && old_buttons == s->btn_mask && old_mode == (int)s->mode) return;
    old_buttons = s->btn_mask; old_mode = s->mode;
    const char *back = s->mode == UI_MODE_BRIGHTNESS ? "1  Cancel" : "1  Back";
    const char *next = s->mode == UI_MODE_SYSTEM ? "5  Restart"
        : s->mode == UI_MODE_BRIGHTNESS ? "5  Save brightness"
        : s->mode == UI_MODE_CAL_SELECT ? "5  Calibrate all"
        : s->mode == UI_MODE_RESTORE_CONFIRM ? "5  Restore defaults"
        : s->mode == UI_MODE_CAL_RUN ? (s->cal_phase == 7 ? "5  Retry" : "5  Done") : "5  Release to restart";
    gfx_set_fg(C_BG); gfx_fill_rect(0, 418, LCD_WIDTH, 62);
    gfx_set_fg(C_OUTLINE); gfx_fill_rect(24, 418, 752, 1);
    if (ui_state_button_enabled(UI_BTN_BACK))
        text_draw(&font_small, 24, 442, back, s->btn_mask & 1 ? C_ACCENT : C_TITLE, C_BG);
    if (ui_state_button_enabled(UI_BTN_SYSTEM))
        text_draw_right(&font_small, 776, 442, next, s->btn_mask & (1u << UI_BTN_SYSTEM) ? C_ACCENT : C_TITLE, C_BG);
}

/* The drawing order deliberately mirrors the enclosure: dials 5-8 above the screen and 1-4
 * below it. Keeping the same map on selection and while running makes the active channel's
 * physical location unambiguous. */
static void draw_cal_dial_map(int active, int selection)
{
    const ui_state_t *s = ui_state();
    for (unsigned p = 0; p < 8u; p++) {
        int16_t x = CELL_X(p % UI_ROW_SPLIT);
        int16_t y = p < UI_ROW_SPLIT ? (selection ? 116 : 72) : (selection ? 302 : 338);
        int16_t h = selection ? 92 : 58;
        uint16_t bg = active == (int)p ? C_CELL_DOWN
                    : ((s->cal_valid_mask & (1u << p)) ? C_CELL_HOT : C_CELL);
        gfx_set_fg(bg);
        gfx_fill_rect(x, y, CELL_W, h);
        frame(x, y, CELL_W, h, active == (int)p ? 3 : 2,
              active == (int)p ? C_NAV : C_OUTLINE);
        char label[12] = { 'D','i','a','l',' ',(char)('1' + p),0 };
        text_draw_centred(&font_ui, (int16_t)(x + CELL_W / 2),
                          (int16_t)(y + (selection ? 12 : 8)), label, C_VALUE, bg);
        if (selection)
            text_draw_centred(&font_small, (int16_t)(x + CELL_W / 2), (int16_t)(y + 58),
                              (s->cal_valid_mask & (1u << p)) ? "CUSTOM" : "DEFAULT",
                              C_TITLE, bg);
    }
}

static const char *cal_stage(uint8_t phase)
{
    switch (phase) {
    case 1: return "CLEAR PANEL";
    case 2: return "TOUCH DIAL";
    case 3: return "TOUCH + TURN";
    case 4: return "RELEASE DIAL";
    case 5: return "FINAL CLEAR CHECK";
    case 6: return "COMPLETE";
    case 7: return "CALIBRATION FAILED";
    default: return "STARTING";
    }
}

static void render_brightness_editor(void)
{
    const ui_state_t *s = ui_state();
    char value[16];
    (void)text_format(value, sizeof(value), (double)s->brightness_preview, 0);
    unsigned n = (unsigned)strlen(value); value[n++] = '%'; value[n] = 0;
    text_draw_centred(&font_display, 400, 94, value, C_VALUE, C_BG);
    text_draw_centred(&font_small, 400, 246, "Turn any dial to adjust", C_TITLE, C_BG);
    const int16_t bx = 96, by = 306, bw = 608;
    int16_t fill = (int16_t)(((uint32_t)(s->brightness_preview - 10u) * (bw-4)) / 90u);
    gfx_meter((gfx_rect_t){bx,by,bw,14},fill,1,C_ACCENT);
    text_draw(&font_small, bx, 332, "10%", C_TITLE, C_BG);
    text_draw_right(&font_small, bx + bw, 332, "100%", C_TITLE, C_BG);
    if (s->brightness_save_error)
        text_draw_centred(&font_small, 400, 378, "Could not save. Press button 5 to retry.", C_ALERT, C_BG);
}

static void render_system_view(int full)
{
    const ui_state_t *s = ui_state();
    if (full) {
        clear_screen();
    }

    const char *title = "Device settings";
    if (s->mode == UI_MODE_REBOOT_WAIT) title = "RESTARTING";
    else if (s->mode == UI_MODE_CAL_SELECT) title = "TOUCH CALIBRATION";
    else if (s->mode == UI_MODE_CAL_RUN) title = "CALIBRATING TOUCH";
    else if (s->mode == UI_MODE_RESTORE_CONFIRM) title = "RESTORE TOUCH DEFAULTS";
    else if (s->mode == UI_MODE_BRIGHTNESS) title = "Brightness";
    text_draw(&font_ui, 24, 20, title, C_VALUE, C_BG);
    gfx_set_fg(C_OUTLINE);
    gfx_fill_rect(MARGIN_X, 58, LCD_WIDTH - 2 * MARGIN_X, 2);
    if (s->mode == UI_MODE_REBOOT_WAIT) {
        text_draw_centred(&font_ui, LCD_WIDTH / 2, 175,
                          "Release button 5 to restart", C_VALUE, C_BG);
        text_draw_centred(&font_small, LCD_WIDTH / 2, 210,
                          "The application will restart normally.", C_TITLE, C_BG);
    } else if (s->mode == UI_MODE_SYSTEM) {
        char n[24];
        uint32_t fw = emp_device_firmware_version();
        char version[32], part[8];
        unsigned k = 0;
        (void)text_format(part, sizeof(part), (double)((fw >> 16) & 0xFFu), 0);
        for (unsigned i = 0; part[i]; i++) version[k++] = part[i];
        version[k++] = '.';
        (void)text_format(part, sizeof(part), (double)((fw >> 8) & 0xFFu), 0);
        for (unsigned i = 0; part[i]; i++) version[k++] = part[i];
        version[k++] = '.';
        (void)text_format(part, sizeof(part), (double)(fw & 0xFFu), 0);
        for (unsigned i = 0; part[i]; i++) version[k++] = part[i];
        version[k++] = ' '; version[k++] = '/'; version[k++] = ' ';
        version[k++] = 'E'; version[k++] = 'M'; version[k++] = 'P'; version[k++] = ' ';
        version[k++] = (char)('0' + EMP_VERSION_MAJOR); version[k++] = '.';
        version[k++] = (char)('0' + EMP_VERSION_MINOR); version[k] = 0;

        text_draw_right(&font_small, 776, 27, emp_device_model(), C_TITLE, C_BG);
        system_fact(24, 84, "FIRMWARE", version, &font_ui);
        system_fact(424, 84, "SERIAL NUMBER", emp_device_serial(), &font_ui);
        system_fact(24, 166, "BUILD", emp_device_build_id(), &font_small);
        (void)text_format(n, sizeof(n), (double)(s->uptime_ms / 3600000u), 0);
        unsigned used = (unsigned)strlen(n); n[used++] = 'h'; n[used++] = ' ';
        (void)text_format(n + used, sizeof(n) - used, (double)((s->uptime_ms / 60000u) % 60u), 0);
        used = (unsigned)strlen(n); n[used++] = 'm'; n[used] = 0;
        system_fact(424, 166, "UPTIME", n, &font_small);
        text_draw(&font_small, 24, 238, "Touch sensors", C_TITLE, C_BG);
        text_draw(&font_small, 208, 238, s->touch_status == 0
            ? (s->cal_custom ? "Ready / custom calibration" : "Ready / factory defaults") : "Sensor fault",
            s->touch_status == 0 ? C_LIVE : C_ALERT, C_BG);
        text_draw(&font_small, 24, 276, "Turn any dial to choose / press to open", C_TITLE, C_BG);
        const char *items[3] = { "Calibrate", "Restore", "Brightness" };
        const char *details[3] = { "Touch sensors", "Touch defaults", "" };
        for (unsigned i = 0; i < 3; i++) {
            int16_t x = (int16_t)(24 + i * 256);
            int selected = s->system_selection == i;
            uint16_t bg = selected ? C_CELL_HOT : C_CELL;
            gfx_set_fg(bg); gfx_fill_rect(x, 310, 240, 92);
            if (selected) { gfx_set_fg(C_ACCENT); gfx_fill_rect(x, 310, 3, 92); }
            text_draw(&font_ui, x + 16, 322, items[i], C_VALUE, bg);
            char pct[12];
            if (i == 2) {
                (void)text_format(pct, sizeof(pct), s->brightness_saved, 0);
                unsigned len = (unsigned)strlen(pct); pct[len++] = '%'; pct[len] = 0;
            }
            text_draw(&font_small, x + 16, 366, i == 2 ? pct : details[i], C_TITLE, bg);
        }
    } else if (s->mode == UI_MODE_BRIGHTNESS) {
        render_brightness_editor();
    } else if (s->mode == UI_MODE_CAL_SELECT) {
        text_draw_centred(&font_small, LCD_WIDTH / 2, 76,
                          "Press the knob for the dial you want to calibrate.", C_VALUE, C_BG);
        draw_cal_dial_map(-1, 1);
    } else if (s->mode == UI_MODE_CAL_RUN) {
        draw_cal_dial_map((s->cal_phase == 6u) ? -1 : (int)s->cal_pot, 0);
        if (s->cal_phase == 6u) {
            text_draw_centred(&font_value, LCD_WIDTH / 2, 166,
                              "CALIBRATION COMPLETE", C_TOGGLE_ON, C_BG);
            text_draw_centred(&font_ui, LCD_WIDTH / 2, 264,
                              "Calibration saved", C_VALUE, C_BG);
        } else {
            char dial[16] = { 'D','I','A','L',' ',(char)('1' + s->cal_pot),0 };
            text_draw_centred(&font_value, LCD_WIDTH / 2, 136, dial,
                              s->cal_phase == 7u ? C_ALERT : C_VALUE, C_BG);
            text_draw_centred(&font_ui, LCD_WIDTH / 2, 176, cal_stage(s->cal_phase),
                              s->cal_phase == 7u ? C_ALERT : C_TITLE, C_BG);
            text_draw_centred(&font_small, LCD_WIDTH / 2, 212,
                              s->cal_message ? s->cal_message : "Follow the instructions",
                              C_VALUE, C_BG);
            if (s->cal_phase == 1u || s->cal_phase == 5u) {
                char seconds[16];
                (void)text_format(seconds, sizeof(seconds),
                                  (double)s->cal_clear_ms / 1000.0, 1);
                text_draw_centred(&font_ui, LCD_WIDTH / 2, 300, seconds, C_ACCENT, C_BG);
            } else if (s->cal_phase != 7u) {
                if (s->cal_cycle >= 3u) {
                    text_draw_centred(&font_small, LCD_WIDTH / 2, 306,
                                      "FINAL CHECK", C_TITLE, C_BG);
                } else {
                    char pass[16] = { 'P','A','S','S',' ',(char)('1' + s->cal_cycle),
                                      ' ','/',' ','3',0 };
                    text_draw_centred(&font_small, LCD_WIDTH / 2, 306, pass, C_TITLE, C_BG);
                }
            }
        }
        if (s->cal_phase == 7u) {
            /* The two-line alert is the widest calibration status. Restore the surrounding
             * chrome afterwards so its background can never leave holes at the card edges. */
            draw_cal_dial_map((int)s->cal_pot, 0);
            text_draw(&font_ui, 24, 20, title, C_VALUE, C_BG);
            gfx_set_fg(C_OUTLINE);
            gfx_fill_rect(MARGIN_X, 58, LCD_WIDTH - 2 * MARGIN_X, 2);
        }
    } else {
        text_draw_centred(&font_ui, LCD_WIDTH / 2, 170,
                          "Discard all stored touch calibration?", C_VALUE, C_BG);
        text_draw_centred(&font_small, LCD_WIDTH / 2, 225,
                          "Button 1 cancels. Button 5 confirms.", C_TITLE, C_BG);
    }
    draw_system_footer(full);
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

/* Choice layout is decided from the entire field, not the options near the cursor. Otherwise a
 * long label makes the carousel jump between densities as the dial turns. Pooled labels are not
 * NUL-terminated, so measure their advances directly instead of copying them into a scratch
 * buffer (which would make the decision depend on that buffer's size). */
static int16_t choice_label_width(const font_t *face, const char *label, uint16_t len)
{
    int32_t width = 0;
    for (uint16_t i = 0; label && i < len; i++) {
        uint8_t c = (uint8_t)label[i];
        if (c < face->first || c > face->last) c = (uint8_t)'?';
        width += face->glyphs[c - face->first].adv;
        if (width > 32767) return 32767;
    }
    return (int16_t)width;
}

static void choice_label_text(const surf_field_t *f, uint16_t option,
                              char *buf, uint32_t cap)
{
    uint16_t len = 0;
    const char *label = surf_choice_label(f, option, &len);
    if (!label) {
        (void)text_format(buf, cap, (double)option, 0);
        return;
    }
    uint16_t n = len < cap - 1u ? len : (uint16_t)(cap - 1u);
    memcpy(buf, label, n);
    buf[n] = 0;
}

static int choice_labels_fit(const surf_field_t *f, uint8_t slots)
{
    int16_t available = (int16_t)(FC_W / slots - 4 * SP);
    uint16_t scan = f->choice_count;
    if (scan > SURF_MAX_CHOICES) scan = SURF_MAX_CHOICES;
    int missing = f->choice_count > scan;

    for (uint16_t option = 0; option < scan; option++) {
        uint16_t len = 0;
        const char *label = surf_choice_label(f, option, &len);
        if (!label) { missing = 1; continue; }
        if (choice_label_width(&font_ui, label, len) > available) return 0;
    }

    /* Missing labels render as their numeric index. The final index is the widest possible
     * fallback for the field, including options beyond the bounded label-table scan. */
    if (missing && f->choice_count) {
        char fallback[24];
        (void)text_format(fallback, sizeof(fallback), (double)(f->choice_count - 1u), 0);
        if (text_width(&font_ui, fallback) > available) return 0;
    }
    return 1;
}

static uint8_t choice_slot_count(const surf_field_t *f)
{
    if (choice_labels_fit(f, 7u)) return 7u;
    if (choice_labels_fit(f, 5u)) return 5u;
    return 3u;
}

static void render_choice_drilled(const surf_field_t *f, int16_t y)
{
    gfx_set_fg(C_BG);
    gfx_fill_rect(FC_X, y, FC_W, FC_DIGIT_H);
    uint16_t count = f->choice_count;
    if (!count) return;
    uint16_t selected = (uint16_t)(f->choice < count ? f->choice : count - 1u);
    uint8_t slots = choice_slot_count(f);
    int16_t pitch = (int16_t)(FC_W / slots);
    int8_t half = (int8_t)(slots / 2u);

    for (int8_t offset = (int8_t)-half; offset <= half; offset++) {
        int32_t option_i = (int32_t)selected + offset;
        if (option_i < 0 || option_i >= count) continue; /* deliberately blank, never wrapped */
        uint16_t option = (uint16_t)option_i;
        char buf[48];
        choice_label_text(f, option, buf, sizeof(buf));
        const font_t *face = (slots == 3u && option == selected) ? &font_value : &font_ui;
        fit_text(face, buf, (int16_t)(pitch - 4 * SP));
        int16_t cx = (int16_t)(LCD_WIDTH / 2 + offset * pitch);
        text_draw_centred(face, cx,
                          (int16_t)(y + (FC_DIGIT_H - text_height(face)) / 2 - 4),
                          buf, option == selected ? C_VALUE : C_TITLE, C_BG);
    }

    /* The marker makes selection unambiguous when every label uses the compact proportional
     * face. Its centre is also invariant: exactly LCD_WIDTH/2 for every option and density. */
    gfx_set_fg(C_ACCENT);
    gfx_fill_rect((int16_t)(LCD_WIDTH / 2 - 15),
                     (int16_t)(y + FC_DIGIT_H - 13), 30, 3);

    if (selected > (uint16_t)half)
        text_draw(&font_ui, FC_X, (int16_t)(y + 42), "<", C_NAV, C_BG);
    if ((uint32_t)selected + (uint32_t)half + 1u < count)
        text_draw_right(&font_ui, (int16_t)(FC_X + FC_W), (int16_t)(y + 42),
                        ">", C_NAV, C_BG);
}

static void render_color_drilled(const surf_field_t *f, int16_t y)
{
    static const char *const name[4] = { "R", "G", "B", "A" };
    gfx_set_fg(C_BG);
    gfx_fill_rect(FC_X, y, FC_W, FC_DIGIT_H);
    for (uint8_t i = 0; i < f->color_count; i++) {
        char value[24];
        (void)text_format(value, sizeof(value), f->color[i],
                          (f->present & EMP_PRESENT_PRECISION) ? f->precision : 2u);
        static const uint16_t channel[4]={0xF800,0x07E0,0x001F,0xFFFF};
        gfx_rect_t ramp={KNOB_CX(i)-64,y+72,128,8};
        gfx_paint(ramp,(gfx_paint_t){C_TRACK,channel[i],ramp,1},2);
        text_draw_centred(&font_small, KNOB_CX(i), y, name[i], C_TITLE, C_BG);
        text_draw_centred(&font_compact, KNOB_CX(i), (int16_t)(y + 32), value, C_VALUE, C_BG);
    }
    int16_t sw = 180, sh = 30;
    int16_t sx = (int16_t)((LCD_WIDTH - sw) / 2);
    int16_t sy = (int16_t)(y + FC_DIGIT_H - sh - 2);
    gfx_checker((gfx_rect_t){sx,sy,sw,sh},color565(f),
                f->color_count==4 ? (uint8_t)(255*color_normal(f,f->color[3])) : 255u);
    frame(sx, sy, sw, sh, 2, C_OUTLINE);
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
    const int16_t reg_y    = top ? ROW_TOP_Y : ROW_BOT_Y;
    const int16_t reg_h    = ROW_H;

    if (full) {
        /* Clear only the half the readout occupies. The band and the other row of cells belong
         * to both views and are drawn by the shared paths below, so clearing the whole panel
         * here would make them flash on every entry into a field. */
        gfx_set_fg(C_BG);
        gfx_fill_rect(FC_X, reg_y, FC_W, reg_h);
    }

    draw_legend(full);

    if (f->kind == EMP_KIND_CHOICE) {
        render_choice_drilled(f, digit_y);
        return;
    }
    if (f->kind == EMP_KIND_COLOR && f->color_count >= 3u) {
        render_color_drilled(f, digit_y);
        return;
    }

    /* Anything that is not a plain number has no digit window to show. Booleans use the same
     * switch as the overview, at a larger drilled-view scale. */
    if (f->value_tag == EMP_VAL_BOOL || f->kind != EMP_KIND_NUMBER) {
        gfx_set_fg(C_BG);
        gfx_fill_rect(FC_X, digit_y, FC_W, FC_DIGIT_H);
        if (f->value_tag == EMP_VAL_BOOL) {
            const int16_t sw = 300, sh = 96;
            draw_bool_switch((int16_t)((LCD_WIDTH - sw) / 2),
                           (int16_t)(digit_y + (FC_DIGIT_H - sh) / 2),
                           sw, sh, f->boolean, &font_value);
            return;
        }
        value_text(f, buf, sizeof(buf));
        fit_text(&font_value, buf, FC_W - 2 * MARGIN_X);
        text_draw_centred(&font_value, LCD_WIDTH / 2, (int16_t)(digit_y + 30),
                          buf, ui_value_color(f, 0), C_BG);
        return;
    }

    digit_t d[MAX_DIGITS];
    int16_t w = 0;
    unsigned n = layout_digits(f, d, &w);
    int16_t x0 = (int16_t)((LCD_WIDTH - w) / 2);

    /* Captions and link lines repaint only when the window or the held knob changes, never on a
     * value change: clearing this band on each detent made the top third of the panel flash. */
    static int8_t  links_ws = -99, links_held = -99, links_top = -1, links_zoom = -1;
    static int16_t links_x0 = -1;

    if (full || links_ws != st->ws || links_held != st->held_digit || links_x0 != x0
             || links_top != (int8_t)top || links_zoom != (int8_t)st->zoom_held) {
        int16_t clr_y = top ? cap_y : link_nr;
        int16_t clr_h = top ? (int16_t)(link_nr - cap_y)
                            : (int16_t)(cap_y + 28 - link_nr);
        gfx_set_fg(C_BG);
        gfx_fill_rect(FC_X, clr_y, FC_W, clr_h);

        for (unsigned k = 0; k < UI_DIGITS; k++) {
            int8_t e = (int8_t)(st->ws - (int8_t)k);
            int held = (st->held_digit == (int8_t)k);

            double place = 1.0;
            if (e >= 0) { for (int8_t i = 0; i < e; i++) place *= 10.0; }
            else        { for (int8_t i = 0; i > e; i--) place /= 10.0; }
            (void)text_format(buf, sizeof(buf), place, (uint8_t)(e < 0 ? -e : 0));

            text_draw_centred(&font_small, KNOB_CX(k), cap_y, buf,
                              held || st->zoom_held ? C_ACCENT : C_TITLE, C_BG);

            /* The line from the knob down to the digit it edits. Without it, the correspondence
             * between four identical dials and four positions in a number has to be memorised. */
            for (unsigned i = 0; i < n; i++) {
                if (d[i].knob != (int8_t)k) continue;
                int16_t dx = (int16_t)(x0 + d[i].x + text_width(&font_display, "0") / 2);

                /* Stub out from the knob, diagonal across, stub in to the digit. The shape is
                 * identical either way up; only the direction of travel flips. */
                int16_t knob_end  = top ? link_far : (int16_t)(link_far - 10);
                int16_t digit_end = top ? (int16_t)(link_nr - 10) : link_nr;

                gfx_set_fg(held || st->zoom_held ? C_ACCENT : C_LINK);
                gfx_fill_rect((int16_t)(KNOB_CX(k) - 1), knob_end, 2, 10);
                gfx_line((int16_t)KNOB_CX(k), (int16_t)(top ? knob_end + 10 : knob_end),
                            dx, (int16_t)(top ? digit_end : digit_end + 10));
                gfx_fill_rect((int16_t)(dx - 1), digit_end, 2, 10);
                break;
            }
        }
        links_ws   = st->ws;
        links_held = st->held_digit;
        links_x0   = x0;
        links_top  = (int8_t)top;
        links_zoom = (int8_t)st->zoom_held;
    }

    /* The number, one digit at a time. Repainting the whole band per change cost about 50 ms of
     * SPI and showed as the value flashing black on every detent; a knob changes one digit. */
    static char     last_ch[MAX_DIGITS];
    static uint16_t last_col[MAX_DIGITS];
    static uint8_t last_selected[MAX_DIGITS];
    static unsigned last_n_digits;
    static int16_t  last_digit_x0 = -1;
    static int16_t  last_digit_y  = -1;

    uint8_t other = (uint8_t)(st->held_digit >= 0 ? 88u : 100u);

    int relayout = full || n != last_n_digits || x0 != last_digit_x0 || digit_y != last_digit_y;
    if (relayout) {
        gfx_set_fg(C_BG);
        gfx_fill_rect(FC_X, digit_y, FC_W, FC_DIGIT_H);
        for (unsigned i = 0; i < MAX_DIGITS; i++) last_ch[i] = 0;
    }
    last_digit_y = digit_y;

    int16_t adv = text_width(&font_display, "0");

    for (unsigned i = 0; i < n; i++) {
        uint16_t c;
        if (d[i].grey)                                          c = C_DIM;
        else if (st->zoom_held && d[i].knob >= 0)               c = C_ACCENT;
        else if (d[i].knob >= 0 && d[i].knob == st->held_digit) c = C_VALUE;
        else                                                    c = dim(C_VALUE, other);

        uint8_t selected=(uint8_t)(d[i].knob >= 0 && d[i].knob == st->held_digit);
        if (!relayout && last_ch[i] == d[i].ch && last_col[i] == c && last_selected[i]==selected) continue;
        last_selected[i]=selected;

        int16_t gx = (int16_t)(x0 + d[i].x);
        if (!relayout) {
            gfx_set_fg(C_BG);
            gfx_fill_rect(gx, digit_y, adv, FC_DIGIT_H);
        }
        if (d[i].knob >= 0 && d[i].knob == st->held_digit) {
            gfx_rect_t plate={gx,digit_y,adv,FC_DIGIT_H};
            gfx_card(plate,C_CELL_HOT);
            gfx_set_fg(C_ACCENT);gfx_fill_rect(gx+4,digit_y+FC_DIGIT_H-3,adv-8,3);
        }
        char one[2] = { d[i].ch, 0 };
        text_draw(&font_display, gx, digit_y, one, c,
                  d[i].knob >= 0 && d[i].knob == st->held_digit ? C_CELL_HOT : C_BG);

        last_ch[i]  = d[i].ch;
        last_col[i] = c;
    }
    last_n_digits = n;
    last_digit_x0 = x0;

    if (f->present & EMP_PRESENT_UNIT) {
        str_of(buf, sizeof(buf), f->unit_off, f->unit_len);
        text_draw(&font_small, (int16_t)(x0 + w + SP),
                  (int16_t)(digit_y + FC_DIGIT_H - text_height(&font_small)), buf, C_UNIT, C_BG);
    }

    if ((f->present & (EMP_PRESENT_MIN | EMP_PRESENT_MAX)) ==
        (EMP_PRESENT_MIN | EMP_PRESENT_MAX) && f->max > f->min) {
        int16_t bx = (int16_t)(LCD_WIDTH - 2 * MARGIN_X - 12), by = digit_y;
        int16_t bh = FC_DIGIT_H;
        gfx_set_fg(C_TRACK);
        gfx_fill_rect(bx, by, 12, bh);

        double t = (f->number - f->min) / (f->max - f->min);
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        int16_t fh = (int16_t)(t * (double)bh);
        gfx_set_fg(C_ACCENT);
        gfx_fill_rect(bx, (int16_t)(by + bh - fh), 12, fh);
    }
}

/* ------------------------------------------------------------------ power-on */

/* Boot fills in canonical reading order, the same order used by fields and inputs. */
static const uint8_t boot_order[UI_BOOT_CELL_FRAMES] = { 0, 1, 2, 3, 4, 5, 6, 7 };

/* The accent rule lives at the bottom of the band -- the one region no knob points at -- so it
 * reads as the surface as a whole coming up rather than as anything about a control. */
#define BOOT_RULE_H   4
#define BOOT_RULE_Y   ((int16_t)(BAND_Y + BAND_H - 2 * SP))
#define BOOT_RULE_X   ((int16_t)MARGIN_X)
#define BOOT_RULE_W   ((int16_t)(LCD_WIDTH - 2 * MARGIN_X))

/* One frame of the power-on sequence.
 *
 * Frame-indexed rather than timed, for two reasons that both matter. The simulator has no
 * millisecond clock -- bsp_millis() is the device's SysTick -- so a self-timed animation could
 * not be judged without hardware, which is exactly what tools/sim exists to avoid. And the
 * caller has to own the pacing anyway: it must service USB between frames, because the debug
 * channel must never be blocked by the thing being debugged (see the enumeration wait in
 * main.c, and the hang that comment records).
 *
 * Every frame is INCREMENTAL. Full-screen repaint time contributes to the frame budget; the cells and the rule are hardware fills, and the one piece of text is drawn
 * once. So a frame cannot be rendered on its own -- run 0..n to see frame n, which is also
 * exactly what the device does.
 *
 * It deliberately says nothing about a host. The device is an instrument that a computer may or
 * may not be driving; "READY" is a claim about the panel, the knobs and the surface layer, all
 * of which are true with nothing plugged in. */
static void boot_ready_frame_impl(unsigned step)
{
    if (step == 0) {
        clear_screen();
        return;
    }

    if (step <= UI_BOOT_CELL_FRAMES) {
        unsigned i = step - 1u;
        int16_t x, y;
        cell_geometry(boot_order[i], &x, &y);
        gfx_set_fg(C_CELL);
        gfx_fill_rect(x, y, CELL_W, ROW_H);
        frame(x, y, CELL_W, ROW_H, 2, C_OUTLINE);

        /* The rule advances WITH the cells rather than on a clock of its own, so the two read as
         * one gesture and the sweep cannot finish early because a frame ran long. */
        int16_t from = (int16_t)((int32_t)BOOT_RULE_W * (int32_t)i / (int32_t)UI_BOOT_CELL_FRAMES);
        int16_t to   = (int16_t)((int32_t)BOOT_RULE_W * (int32_t)(i + 1u)
                                 / (int32_t)UI_BOOT_CELL_FRAMES);
        gfx_set_fg(C_ACCENT);
        gfx_fill_rect((int16_t)(BOOT_RULE_X + from), BOOT_RULE_Y,
                         (int16_t)(to - from), BOOT_RULE_H);
        return;
    }

    if (step == UI_BOOT_READY_FRAME) {
        /* Every cell drawn, so lift all eight frames together and say so once. The frames go
         * back to C_OUTLINE when the real surface paints over this, which is the point: the
         * accent is the moment, not a state. */
        for (unsigned k = 0; k < UI_BOOT_CELL_FRAMES; k++) {
            int16_t x, y;
            cell_geometry(k, &x, &y);
            frame(x, y, CELL_W, ROW_H, 2, C_ACCENT);
        }
        text_draw_centred(&font_value, LCD_WIDTH / 2,
                          (int16_t)(BAND_Y + (BAND_H - 2 * SP - text_height(&font_value)) / 2),
                          "READY", C_VALUE, C_BG);
    }

    /* Anything after that is the hold, and there is nothing to draw for it. The caller is
     * pacing; adding ink here would only cost SPI time for no change on the panel. */
}

/* ------------------------------------------------------------------ entry point */

static void render_impl(void)
{
    const ui_state_t *st = ui_state();
    int view = st->mode == UI_MODE_SURFACE ? ((st->focused >= 0) ? 1 : 0)
                                           : (2 + (int)st->mode);
    static uint64_t previous_revision = UINT64_MAX;
    int full = (view != last_view) || previous_revision != surf_applied_revision();
    previous_revision = surf_applied_revision();

    if (st->mode != UI_MODE_SURFACE) {
        static uint8_t old_phase = 0xFFu, old_pot = 0xFFu, old_cycle = 0xFFu;
        static uint8_t old_sel = 0xFFu, old_valid = 0xFFu, old_custom = 0xFFu;
        static uint8_t old_brightness = 0xFFu, old_save_error = 0xFFu;
        static uint16_t old_clear_ds = 0xFFFFu;
        uint16_t clear_ds = (uint16_t)((st->cal_clear_ms + 99u) / 100u);
        int changed = full || old_phase != st->cal_phase || old_pot != st->cal_pot
                    || old_cycle != st->cal_cycle || old_sel != st->system_selection
                    || old_valid != st->cal_valid_mask || old_custom != st->cal_custom
                    || old_clear_ds != clear_ds
                    || old_brightness != st->brightness_preview
                    || old_save_error != st->brightness_save_error;
        if (changed) render_system_view(1); else draw_system_footer(0);
        old_phase = st->cal_phase; old_pot = st->cal_pot; old_cycle = st->cal_cycle;
        old_sel = st->system_selection; old_valid = st->cal_valid_mask; old_custom = st->cal_custom;
        old_brightness = st->brightness_preview;
        old_save_error = st->brightness_save_error;
        old_clear_ds = clear_ds;
    } else if (view == 0) {
        render_overview(full);
    } else {
        const surf_field_t *f = surf_field((uint16_t)st->focused);
        if (!f) { render_overview(full); view = 0; }
        else {
            int changed = full
                       || st->focused    != last_focus
                       || st->ws         != last_ws
                       || st->held_digit != last_held
                       || st->zoom_held  != last_zoom
                       || f->number      != last_value
                       || f->choice      != last_choice
                       || f->boolean     != last_boolean
                       || f->value_tag   != last_value_tag
                       || f->tone        != last_tone
                       || strcmp(f->text, last_drill_text) != 0
                       || f->color_count != last_color_count
                       || memcmp(f->color, last_color, sizeof(last_color)) != 0
                       || st->page       != last_page;

            if (changed) {
                int rebuild = full || st->focused != last_focus;
                render_drilled(rebuild, f);
                if (rebuild || st->zoom_held != last_zoom) {
                    if (st->zoom_held) draw_zoom_banner(PAGE_Y);
                    else               draw_page_strip(PAGE_Y);
                }
                if (!rebuild) draw_legend(0);      /* undo/redo availability moves as you edit */
                render_cells(rebuild, ui_cell_row_first(),
                             ui_cell_row_first() + UI_DIGITS);

                last_focus = st->focused;
                last_ws    = st->ws;
                last_held  = st->held_digit;
                last_zoom  = (int8_t)st->zoom_held;
                last_value = f->number;
                last_choice = f->choice;
                last_boolean = f->boolean;
                last_value_tag = f->value_tag;
                last_tone = f->tone;
                memcpy(last_drill_text, f->text, sizeof(last_drill_text));
                last_color_count = f->color_count;
                memcpy(last_color, f->color, sizeof(last_color));
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
        memset(last_cell, 0, sizeof(last_cell));
    }
}

void ui_render(void)
{
    gfx_begin();
    render_impl();
    gfx_end();
}
void ui_boot_ready_frame(unsigned step)
{
    gfx_begin();
    boot_ready_frame_impl(step);
    gfx_end();
}

/* ------------------------------------------------------------------ demo scenes */

/* What the device actually shows when nothing is driving it: eight empty cells and no page map.
 *
 * This is now the resting state at both ends of a session -- at power-on, and again when the
 * host clears the surface -- so it is worth being able to look at without hardware. It used not
 * to exist on the device at all, because boot loaded a demo descriptor over it. */
void ui_render_empty(void)
{
    surf_init();
    ui_state_init();
    ui_reset();
    ui_render();
}

/* The power-on sequence up to and including frame `step`.
 *
 * Frames are incremental, so this replays from 0 rather than jumping -- which is also exactly
 * what the device does, and means a scene cannot accidentally show a state the panel never
 * reaches. */
void ui_render_boot(unsigned step)
{
    surf_init();
    ui_state_init();
    for (unsigned i = 0; i <= step && i < UI_BOOT_FRAMES; i++) ui_boot_ready_frame(i);
}

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
    ui_state_push(UI_ROW_SPLIT, 1, 1000);       /* press the first bottom-row knob: drill in */
    ui_state_push(UI_ROW_SPLIT, 0, 1010);
    ui_state_touch(0x02, 1020);      /* a finger resting on a top-row digit knob */
    ui_render();
}

/* Declare `def` as field `slot`'s default through the real DEFAULTS handler. */
static void demo_default(uint16_t slot, double def)
{
    uint8_t m[24]; unsigned n = 0;
    uint64_t rev = surf_applied_revision();
    for (unsigned i = 0; i < 8; i++) m[n++] = (uint8_t)(rev >> (8u * i));
    m[n++] = 1; m[n++] = 0;
    m[n++] = (uint8_t)slot; m[n++] = (uint8_t)(slot >> 8);
    m[n++] = 1; m[n++] = EMP_VAL_NUMBER;
    uint64_t bits; memcpy(&bits, &def, sizeof(bits));
    for (unsigned i = 0; i < 8; i++) m[n++] = (uint8_t)(bits >> (8u * i));
    surf_handle(EMP_OP_DEFAULTS, m, n);
}

/* Two of the first page's values away from their defaults, one at it. */
static void demo_modified_fixture(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    demo_default(0, 0.5);       /* Cutoff 0.53: modified */
    demo_default(1, 0.21);      /* Resonance 0.21: at its default */
    demo_default(4, 1.0);       /* Drive -6.5: modified */
}

void ui_render_demo_modified(void)
{
    demo_modified_fixture();
    ui_render();
}

void ui_render_demo_modified_focused(void)
{
    demo_modified_fixture();
    ui_state_push(UI_ROW_SPLIT, 1, 1000);       /* drill into Drive */
    ui_state_push(UI_ROW_SPLIT, 0, 1010);
    ui_render();
}

void ui_render_demo_focused_zoom(void)
{
    demo_modified_fixture();
    ui_state_push(UI_ROW_SPLIT, 1, 1000);
    ui_state_push(UI_ROW_SPLIT, 0, 1010);
    surf_set_number(4, 12.345);                 /* wider than the window, so zoom can move */
    ui_state_button(UI_BTN_ZOOM, 1, 1100);      /* held: the strip and window say so */
    (void)ui_state_rotate(0, -UI_ZOOM_STEP, 1110);
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
    ui_state_push(0, 1, 1000);      /* press the first top-row knob */
    ui_state_push(0, 0, 1010);
    ui_state_touch(0x20, 1020);                /* a finger on bottom knob 1, now a digit knob */
    ui_render();
}

static void demo_focus_on_page(uint8_t page, uint8_t knob)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    if (page) ui_state_button(UI_BTN_PAGE_NEXT, 1, 1000);
    knob = (uint8_t)ui_state_knob_for((uint16_t)(page * UI_FIELDS_PER_PAGE + knob));
    ui_state_push(knob, 1, 1010);
    ui_state_push(knob, 0, 1020);
    ui_render();
}

void ui_render_demo_focused_toggle(void)   { demo_focus_on_page(0, 2); }
void ui_render_demo_focused_negative(void) { demo_focus_on_page(0, 3); }
void ui_render_demo_focused_choice(void)   { demo_focus_on_page(1, 0); }
void ui_render_demo_focused_fine(void)     { demo_focus_on_page(1, 1); }
void ui_render_demo_focused_color(void)    { demo_focus_on_page(1, 3); }

void ui_render_demo_choice_waveform(uint16_t selected)
{
    surf_demo_descriptor();
    surf_set_choice(8u, selected);
    ui_state_init();
    ui_reset();
    ui_state_button(UI_BTN_PAGE_NEXT, 1, 1000);
    ui_state_push((unsigned)ui_state_knob_for(8u), 1, 1010);
    ui_state_push((unsigned)ui_state_knob_for(8u), 0, 1020);
    ui_render();
}

static void demo_choice_labels(const char *const *labels, uint16_t count,
                               uint16_t selected, uint16_t slot)
{
    surf_demo_choice_descriptor(slot, "Choice Width", labels, count, selected);
    ui_state_init();
    ui_reset();
    unsigned knob = (unsigned)ui_state_knob_for(slot);
    ui_state_push(knob, 1, 1000);
    ui_state_push(knob, 0, 1010);
    ui_render();
}

void ui_render_demo_choice_short(void)
{
    static const char *const labels[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I" };
    demo_choice_labels(labels, 9u, 4u, UI_ROW_SPLIT);
}

void ui_render_demo_choice_medium(void)
{
    static const char *const labels[] = {
        "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf"
    };
    demo_choice_labels(labels, 7u, 3u, UI_ROW_SPLIT);
}

void ui_render_demo_choice_long_top(void)
{
    static const char *const labels[] = {
        "Resonant Low Pass", "Pulse Width Mod", "Envelope Follower", "Stereo Crossfeed", "Granular Position"
    };
    demo_choice_labels(labels, 5u, 2u, 0u);
}

void ui_render_demo_page2(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_state_button(UI_BTN_PAGE_NEXT, 1, 1000);
    ui_render();
}

void ui_render_demo_system(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_state_calibration_status(0, 0, 0, 0x25u, 1, 0, "Ready");
    ui_state_system_status(3723000u, 0);
    ui_state_button(UI_BTN_SYSTEM, 1, 1000);
    ui_render();
}

static void demo_brightness_enter(uint8_t saved)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_state_brightness_status(saved);
    ui_state_button(UI_BTN_SYSTEM, 1, 1000);
    (void)ui_state_rotate(0, 1, 1010);
    (void)ui_state_rotate(0, 1, 1020);
    ui_state_push(0, 1, 1030);
    ui_state_push(0, 0, 1040);
}

void ui_render_demo_brightness_full(void)
{
    demo_brightness_enter(100u);
    ui_render();
}

void ui_render_demo_brightness_mid(void)
{
    demo_brightness_enter(60u);
    ui_render();
}

void ui_render_demo_brightness_min(void)
{
    demo_brightness_enter(10u);
    ui_render();
}

void ui_render_demo_brightness_error(void)
{
    demo_brightness_enter(60u);
    ui_state_brightness_save_result(0);
    ui_render();
}

void ui_render_demo_reboot(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_state_button(UI_BTN_SYSTEM, 1, 1000);  /* overview -> settings */
    ui_state_button(UI_BTN_SYSTEM, 0, 1010);
    ui_state_button(UI_BTN_SYSTEM, 1, 1020);  /* held: show release-gated restart */
    ui_render();
}

void ui_render_demo_calibration(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_state_button(UI_BTN_SYSTEM, 1, 1000);
    ui_state_push(0, 1, 1010);                 /* calibration selection */
    ui_state_push(2, 1, 1020);                 /* individual dial */
    ui_state_calibration_status(3, 2, 1, 0x03u, 1, 0, "Keep touching and turn the dial");
    ui_render();
}

static void demo_calibration_enter(void)
{
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_state_button(UI_BTN_SYSTEM, 1, 1000);
    ui_state_push(0, 1, 1010);
}

void ui_render_demo_calibration_select(void)
{
    demo_calibration_enter();
    ui_state_calibration_status(0, 0, 0, 0x25u, 1, 0, "Ready");
    ui_render();
}

void ui_render_demo_calibration_baseline(void)
{
    demo_calibration_enter();
    ui_state_push(2, 1, 1020);
    ui_state_calibration_status(1, 2, 0, 0x03u, 1, 1300,
                                "Keep hands clear - starts automatically");
    ui_render();
}

void ui_render_demo_calibration_failed(void)
{
    demo_calibration_enter();
    ui_state_push(2, 1, 1020);
    ui_state_calibration_status(7, 2, 1, 0x03u, 1, 0,
                                "A different dial was touched");
    ui_render();
}

void ui_render_demo_calibration_complete(void)
{
    demo_calibration_enter();
    ui_state_push(2, 1, 1020);
    ui_state_calibration_status(6, 2, 3, 0x07u, 1, 0, "Calibration saved");
    ui_render();
}
