#ifndef ELECTRA_GRAPHICS_BACKEND_H
#define ELECTRA_GRAPHICS_BACKEND_H
#include <stdint.h>
#include "ra8876.h"
/* These operations have identical RGB565 page/coordinate semantics in both builds. */
#if defined(GRAPHICS_DAVE2D) && GRAPHICS_DAVE2D
void gb_set_canvas(uint32_t a);
void gb_set_fg(uint16_t c);
void gb_fill_rect(int16_t x,int16_t y,int16_t w,int16_t h);
void gb_line(int16_t x,int16_t y,int16_t xx,int16_t yy);
void gb_bte_copy(uint32_t src,int16_t sx,int16_t sy,uint32_t dst,int16_t dx,int16_t dy,int16_t w,int16_t h);
void gb_blit_begin(int16_t x,int16_t y,int16_t w,int16_t h);
void gb_blit_pixels(const uint16_t *pixels,uint32_t n);
void gb_blit_end(void);
#else
#define gb_set_canvas ra8876_set_canvas
#define gb_set_fg ra8876_set_fg
#define gb_fill_rect ra8876_fill_rect
#define gb_line ra8876_line
#define gb_bte_copy ra8876_bte_copy
#define gb_blit_begin ra8876_blit_begin
#define gb_blit_pixels ra8876_blit_pixels
#define gb_blit_end ra8876_blit_end
#endif
uint32_t gfx_clock_cycles(void);
extern uint64_t gfx_gpu_cycles;
extern uint64_t gfx_spi_cycles;
extern uint32_t gfx_accel_errors;
#endif
