/* Coverage text delegates to shared graphics for background-aware, cached compositing. */

#include "text.h"
#include "graphics.h"

/* One row of the widest string we will ever draw. 800 pixels is the whole panel. */


static const font_glyph_t *glyph_of(const font_t *f, char c)
{
    uint8_t u = (uint8_t)c;
    if (u < f->first || u > f->last) u = ('?' >= f->first && '?' <= f->last) ? '?' : f->first;
    return &f->glyphs[u - f->first];
}

int16_t text_width(const font_t *f, const char *s)
{
    int32_t w = 0;
    for (; *s; s++) w += glyph_of(f, *s)->adv;
    return (int16_t)(w > 32767 ? 32767 : w);
}

int16_t text_height(const font_t *f) { return (int16_t)f->line_height; }

int16_t text_draw(const font_t *f, int16_t x, int16_t y,
                  const char *s, uint16_t fg, uint16_t bg)
{
    return gfx_text(f, x, y, s, fg, bg);
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
