/* Antialiased text, blended on the MCU and streamed to the panel.
 *
 * The RA8876 can composite glyphs from its own VRAM with the BTE, which is what the stock
 * firmware does. That is faster but it is 1-bit: type is either on or off, so it is aliased.
 * On this panel, at this size, aliased text is the single thing that most makes an instrument
 * look cheap — and better typography is one of the stated reasons this firmware exists.
 *
 * So coverage is blended here, against the caller's actual colours, and the finished pixels are
 * pushed. One atlas serves every colour pair, the type is properly antialiased, and the cost is
 * bounded by ink rather than by screen area: a 200x30 label is about 6 ms, where a full-screen
 * repaint through the same path would be 0.8 s.
 *
 * Rendering is row-at-a-time into a single scanline buffer. That keeps RAM flat regardless of
 * how long the string is, and matches how the controller consumes a windowed blit anyway.
 */

#include "text.h"
#include "ra8876.h"

/* One row of the widest string we will ever draw. 800 pixels is the whole panel. */
static uint16_t row_buf[LCD_WIDTH];

static const font_glyph_t *glyph_of(const font_t *f, char c)
{
    uint8_t u = (uint8_t)c;
    if (u < f->first || u > f->last) u = '?';
    return &f->glyphs[u - f->first];
}

int16_t text_width(const font_t *f, const char *s)
{
    int32_t w = 0;
    for (; *s; s++) w += glyph_of(f, *s)->adv;
    return (int16_t)w;
}

int16_t text_height(const font_t *f) { return (int16_t)f->line_height; }

/* Blend one coverage value between two RGB565 colours.
 *
 * Done per channel at 5/6/5 rather than by converting to 8-bit and back: the rounding is
 * indistinguishable at these sizes and it avoids three multiplies per pixel on a core with no
 * hardware divide worth using here. */
static inline uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a)
{
    if (a == 0)   return bg;
    if (a == 255) return fg;

    uint32_t br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    uint32_t fr = (fg >> 11) & 0x1F, fgc = (fg >> 5) & 0x3F, fb = fg & 0x1F;

    uint32_t r = br + (((fr - br) * a + 128) >> 8);
    uint32_t g = bgc + (((fgc - bgc) * a + 128) >> 8);
    uint32_t b = bb + (((fb - bb) * a + 128) >> 8);

    return (uint16_t)((r << 11) | (g << 5) | b);
}

int16_t text_draw(const font_t *f, int16_t x, int16_t y,
                  const char *s, uint16_t fg, uint16_t bg)
{
    int16_t w = text_width(f, s);
    int16_t h = (int16_t)f->line_height;

    if (w <= 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT) return 0;
    if (x + w > LCD_WIDTH)  w = (int16_t)(LCD_WIDTH - x);
    if (y + h > LCD_HEIGHT) h = (int16_t)(LCD_HEIGHT - y);

    /* Blit only the rows that actually contain ink.
     *
     * A 120 px face has a line box of 121 rows and digits whose ink is about 79 -- so a third of
     * every glyph pushed over SPI was blank background. At 640 KiB/s that third is real time:
     * repainting one big digit measured ~25 ms, which reads as a sluggish readout when a knob
     * is turning.
     *
     * The consequence is that THE CALLER OWNS THE BACKGROUND: rows outside the ink band are left
     * as they were. Every caller here already fills its region first, and a hardware fill costs
     * a handful of register writes regardless of area, so this moves work from the expensive
     * path to the free one. */
    int16_t ink_top = h, ink_bot = 0;
    for (const char *p = s; *p; p++) {
        const font_glyph_t *g = glyph_of(f, *p);
        if (!g->w) continue;
        if (g->by < ink_top) ink_top = g->by;
        if ((int16_t)(g->by + g->h) > ink_bot) ink_bot = (int16_t)(g->by + g->h);
    }
    if (ink_top < 0) ink_top = 0;
    if (ink_bot > h) ink_bot = h;
    if (ink_bot <= ink_top) return w;              /* nothing to draw, e.g. a run of spaces */

    ra8876_blit_begin(x, (int16_t)(y + ink_top), w, (int16_t)(ink_bot - ink_top));

    for (int16_t row = ink_top; row < ink_bot; row++) {
        for (int16_t i = 0; i < w; i++) row_buf[i] = bg;

        int32_t pen = 0;
        for (const char *p = s; *p; p++) {
            const font_glyph_t *g = glyph_of(f, *p);

            /* `by` is measured from the TOP of the line box, because that is where the
             * generator drew from — PIL's default anchor puts the text's top at the origin,
             * not its baseline. Adding `ascent` here, as if `by` were a baseline-relative
             * bearing, pushed every glyph down by a whole ascent so only its last row or two
             * landed inside the box. The simulator showed that as a thin sliver of digits and
             * no label text at all, in about a second. */
            int16_t top = g->by;
            if (g->w && row >= top && row < top + (int16_t)g->h) {
                const uint8_t *cov = f->pixels + g->off + (uint32_t)(row - top) * g->w;
                int32_t ox = pen + g->bx;

                for (uint8_t i = 0; i < g->w; i++) {
                    int32_t px = ox + i;
                    if (px >= 0 && px < w) {
                        row_buf[px] = blend(row_buf[px], fg, cov[i]);
                    }
                }
            }
            pen += g->adv;
            if (pen > w) break;
        }
        ra8876_blit_pixels(row_buf, (uint32_t)w);
    }

    ra8876_blit_end();
    return w;
}

int16_t text_draw_right(const font_t *f, int16_t right, int16_t y,
                        const char *s, uint16_t fg, uint16_t bg)
{
    int16_t w = text_width(f, s);
    return text_draw(f, (int16_t)(right - w), y, s, fg, bg);
}

int16_t text_draw_centred(const font_t *f, int16_t cx, int16_t y,
                          const char *s, uint16_t fg, uint16_t bg)
{
    int16_t w = text_width(f, s);
    return text_draw(f, (int16_t)(cx - w / 2), y, s, fg, bg);
}

/* ------------------------------------------------------------------ formatting */

/* Fixed-point decimal, no libc.
 *
 * The device formats values rather than the host sending strings, deliberately: pre-formatting
 * host-side would be lossy for a value the device echoes back, and it puts display policy on
 * the wrong side of the wire. Doing it here needs only integer work — `precision` from the
 * descriptor says how many decimals, so there is never a question of how to round. */
int text_format(char *out, uint32_t cap, double v, uint8_t precision)
{
    if (cap < 2) return 0;

    uint32_t i = 0;
    int neg = 0;

    if (v < 0) { neg = 1; v = -v; }

    /* Round at the requested precision before splitting, or 0.999 at 2 dp prints "0.99". */
    uint32_t scale = 1;
    for (uint8_t k = 0; k < precision; k++) scale *= 10u;

    double scaled = v * (double)scale + 0.5;
    if (scaled > 4294967000.0) scaled = 4294967000.0;      /* clamp rather than wrap */
    uint32_t fixed = (uint32_t)scaled;

    uint32_t whole = fixed / scale;
    uint32_t frac  = fixed % scale;

    char tmp[12];
    uint32_t n = 0;
    do { tmp[n++] = (char)('0' + whole % 10u); whole /= 10u; } while (whole && n < sizeof(tmp));

    if (neg && i < cap - 1) out[i++] = '-';
    while (n && i < cap - 1) out[i++] = tmp[--n];

    if (precision && i < cap - 1) {
        out[i++] = '.';
        for (uint8_t k = 0; k < precision && i < cap - 1; k++) {
            scale /= 10u;
            out[i++] = (char)('0' + (frac / scale) % 10u);
        }
    }
    out[i] = 0;
    return (int)i;
}
