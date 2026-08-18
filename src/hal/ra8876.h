/* RAiO RA8876 display controller.
 *
 * 800x480 RGB565 panel with 16 MB of the controller's own SDRAM. The MCU never holds a
 * framebuffer: it issues drawing commands and the RA8876 scans out by itself. The MCU's own
 * GLCDC is unused on this board.
 */

#ifndef ELECTRA_RA8876_H
#define ELECTRA_RA8876_H

#include <stdint.h>

#define LCD_WIDTH   800
#define LCD_HEIGHT  480
#define LCD_BPP     2                   /* RGB565 */

/* VRAM layout. Pages are 0x12C000 = 1024*600*2 in the stock firmware — sized for the
 * full-size Electra One, not this panel. At 800x480 a page only needs 0xBB800, so we use
 * the tighter figure and get more pages out of the same 16 MB. */
#define RA_PAGE_BYTES   ((uint32_t)LCD_WIDTH * LCD_HEIGHT * LCD_BPP)   /* 0xBB800 */
#define RA_VRAM_SIZE    (16UL * 1024 * 1024)
#define RA_PAGE(n)      ((uint32_t)(n) * RA_PAGE_BYTES)

/* Always returns 0 -- bring-up is never aborted, so that the write path can still light
 * the panel when the read path is broken. Check g_ra_status for what did not confirm. */
int  ra8876_init(void);

/* Bitmask of stages whose status poll never confirmed. Non-fatal; inspect after init.
 * b0 reset poll, b1 pll poll, b2 sdram poll, b3 status register reads look stuck. */
extern uint32_t g_ra_status;

void ra8876_display_on(int on);
void ra8876_backlight(uint16_t brightness);   /* 0x0800 brightest .. 0x2000 off; BELOW
                                               * 0x0800 is also OFF. Clamped internally. */

/* Drawing. All coordinates are relative to the active window origin. */
void ra8876_set_canvas(uint32_t addr);        /* where drawing lands */
void ra8876_set_display_page(uint32_t addr);  /* what is scanned out */
void ra8876_set_fg(uint16_t rgb565);
void ra8876_set_bg(uint16_t rgb565);          /* also the chroma key for transparent blits */

void ra8876_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h);
void ra8876_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
void ra8876_bte_fill(uint32_t dst, int16_t x, int16_t y, int16_t w, int16_t h);
void ra8876_bte_copy(uint32_t src, int16_t sx, int16_t sy,
                     uint32_t dst, int16_t dx, int16_t dy,
                     int16_t w, int16_t h);
/* Stream pixels into a rectangle. begin, then any number of pixel runs, then end — the
 * controller wraps at the window edge, so a rectangle is one linear run. ALWAYS call end: the
 * active window stays clamped otherwise and every later fill is silently clipped. */
void ra8876_blit_begin(int16_t x, int16_t y, int16_t w, int16_t h);
void ra8876_blit_pixels(const uint16_t *px, uint32_t count);
void ra8876_blit_end(void);

void ra8876_bte_copy_chroma(uint32_t src, int16_t sx, int16_t sy,
                            uint32_t dst, int16_t dx, int16_t dy,
                            int16_t w, int16_t h);

#endif /* ELECTRA_RA8876_H */
