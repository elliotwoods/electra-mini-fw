/* Antialiased text on the panel. See text.c for why the blending happens on the MCU rather
 * than in the controller's BTE. */

#ifndef ELECTRA_TEXT_H
#define ELECTRA_TEXT_H

#include <stdint.h>
#include "font.h"

int16_t text_width(const font_t *f, const char *s);
int16_t text_height(const font_t *f);

/* y is the TOP of the line box. Returns the width drawn.
 *
 * Not the baseline: the glyph metrics are generated top-relative, and having the two disagree
 * is precisely the bug that made the first version render slivers. Mixed sizes therefore align
 * at their tops; align baselines by offsetting y by the difference in ascent. */
int16_t text_draw(const font_t *f, int16_t x, int16_t y,
                  const char *s, uint16_t fg, uint16_t bg);
int16_t text_draw_right(const font_t *f, int16_t right, int16_t y,
                        const char *s, uint16_t fg, uint16_t bg);
int16_t text_draw_centred(const font_t *f, int16_t cx, int16_t y,
                          const char *s, uint16_t fg, uint16_t bg);

/* Fixed-point decimal, no libc and no float formatting. `precision` comes from the field
 * descriptor, so there is never a question of how many decimals to show. */
int text_format(char *out, uint32_t cap, double v, uint8_t precision);

#endif /* ELECTRA_TEXT_H */
