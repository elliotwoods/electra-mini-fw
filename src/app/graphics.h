/* Shared, clipped RGB565 drawing operations. No allocation or protocol ownership. */
#ifndef ELECTRA_GRAPHICS_H
#define ELECTRA_GRAPHICS_H
#include <stdint.h>
#include "font.h"
#include "ra8876.h"
#define GFX_WORK RA_PAGE(1)
#define GFX_GRADIENT_FIRST RA_PAGE(2)
#define GFX_GRADIENT_COUNT 8u
#define GFX_TEXT_FIRST RA_PAGE(10)
#define GFX_TEXT_PAGES 4u
#define GFX_VRAM_END RA_PAGE(14)
_Static_assert(GFX_VRAM_END <= RA_VRAM_SIZE, "graphics VRAM exceeds device");
typedef struct { int16_t x,y,w,h; } gfx_rect_t;
typedef struct { uint16_t first,last; gfx_rect_t bounds; uint8_t horizontal; } gfx_paint_t;
typedef struct { uint32_t fills,copies,text_hits,text_misses,gradient_hits,gradient_misses,spi_pixels; } gfx_stats_t;
void gfx_begin(void);
void gfx_end(void);
void gfx_invalidate(void);
void gfx_cache_reset(void);
void gfx_clip(gfx_rect_t rect);
void gfx_unclip(void);
int gfx_intersect(gfx_rect_t a,gfx_rect_t b,gfx_rect_t *out);
uint16_t gfx_blend(uint16_t bg,uint16_t fg,uint8_t coverage);
uint16_t gfx_sample(gfx_paint_t paint,int16_t x,int16_t y);
gfx_paint_t gfx_solid(uint16_t c,gfx_rect_t rect);
gfx_paint_t gfx_background(gfx_rect_t rect,uint16_t fallback);
void gfx_paint(gfx_rect_t rect,gfx_paint_t paint,uint8_t radius);
void gfx_border(gfx_rect_t rect,uint8_t radius,uint8_t thickness,uint16_t c);
void gfx_restore(gfx_rect_t rect,uint16_t background);
void gfx_copy(uint32_t src,gfx_rect_t rect,uint32_t dst,int16_t x,int16_t y);
void gfx_set_fg(uint16_t c);
void gfx_fill_rect(int16_t x,int16_t y,int16_t w,int16_t h);
void gfx_line(int16_t x0,int16_t y0,int16_t x1,int16_t y1);
void gfx_card(gfx_rect_t rect,uint16_t c);
void gfx_meter(gfx_rect_t rect,int16_t extent,int horizontal,uint16_t colour);
void gfx_checker(gfx_rect_t rect,uint16_t colour,uint8_t alpha);
int16_t gfx_text(const font_t *f,int16_t x,int16_t y,const char *s,uint16_t fg,uint16_t bg);
const gfx_stats_t *gfx_stats(void);
/* Service USB/input between bounded operations; implementation must not re-enter rendering. */
void gfx_yield(void);
#endif
