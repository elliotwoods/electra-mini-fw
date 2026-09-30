#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "ra8876.h"
static uint16_t vram[RA_VRAM_SIZE/2];
static uint32_t canvas, display;
static uint16_t *fb;
uint16_t *sim_framebuffer(void) { return vram+display/2; }
uint16_t *sim_vram(void) { return vram; }
void sim_reset(void) { memset(vram,0,sizeof(vram));canvas=display=0;fb=vram; }


/* Active-window state, mirroring the controller's: a blit wraps at the window edge, and the
 * renderer relies on that, so the simulator has to model it or it would flatter code that
 * would not work on the panel. */
static int16_t aw_x, aw_y, aw_w, aw_h;
static int32_t cursor;

static uint16_t fg_colour;

void ra8876_set_fg(uint16_t c) { fg_colour = c; }
void ra8876_set_bg(uint16_t c) { (void)c; }

void ra8876_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h)
{
    for (int16_t row = 0; row < h; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= LCD_HEIGHT) continue;
        for (int16_t col = 0; col < w; col++) {
            int32_t px = x + col;
            if (px < 0 || px >= LCD_WIDTH) continue;
            fb[py * LCD_WIDTH + px] = fg_colour;
        }
    }
}

void ra8876_blit_begin(int16_t x, int16_t y, int16_t w, int16_t h)
{
    aw_x = x; aw_y = y; aw_w = w; aw_h = h;
    cursor = 0;
}

void ra8876_blit_pixels(const uint16_t *px, uint32_t count)
{
    if (aw_w <= 0 || aw_h <= 0) return;
    for (uint32_t i = 0; i < count; i++) {
        int32_t cx = aw_x + (cursor % aw_w);
        int32_t cy = aw_y + (cursor / aw_w);
        if (cy >= aw_y + aw_h) break;
        if (cx >= 0 && cx < LCD_WIDTH && cy >= 0 && cy < LCD_HEIGHT) {
            fb[cy * LCD_WIDTH + cx] = px[i];
        }
        cursor++;
    }
}

void ra8876_blit_end(void)
{
    aw_x = 0; aw_y = 0; aw_w = LCD_WIDTH; aw_h = LCD_HEIGHT;
}

/* Unused by the drawing code, but referenced by the header. */
/* Bresenham, because the device's line comes from the RA8876's draw engine and the simulator
 * has no draw engine. Aliased on both sides, so what is judged here is what appears there. */
void ra8876_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 >= 0 && x0 < LCD_WIDTH && y0 >= 0 && y0 < LCD_HEIGHT)
            fb[(size_t)y0 * LCD_WIDTH + x0] = fg_colour;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 = (int16_t)(x0 + sx); }
        if (e2 <  dx) { err += dx; y0 = (int16_t)(y0 + sy); }
    }
}

void ra8876_set_canvas(uint32_t a) {
    assert(a <= RA_VRAM_SIZE - RA_PAGE_BYTES && !(a&1));canvas=a;fb=vram+a/2;
}
void ra8876_set_display_page(uint32_t a) {
    assert(a <= RA_VRAM_SIZE - RA_PAGE_BYTES && !(a&1));display=a;
}
void ra8876_bte_copy(uint32_t src,int16_t sx,int16_t sy,uint32_t dst,int16_t dx,int16_t dy,int16_t w,int16_t h)
{
    assert(src <= RA_VRAM_SIZE-RA_PAGE_BYTES && dst <= RA_VRAM_SIZE-RA_PAGE_BYTES);
    /* Hardware op 2 is forward copy. Reject overlapping reverse copies in tests. */
    for(int row=0;row<h;row++) for(int col=0;col<w;col++) {
        int x=sx+col,y=sy+row,xx=dx+col,yy=dy+row;
        if(x>=0&&x<LCD_WIDTH&&y>=0&&y<LCD_HEIGHT&&xx>=0&&xx<LCD_WIDTH&&yy>=0&&yy<LCD_HEIGHT)
            vram[dst/2+(unsigned)yy*LCD_WIDTH+xx]=vram[src/2+(unsigned)y*LCD_WIDTH+x];
    }
}
void ra8876_bte_fill(uint32_t dst,int16_t x,int16_t y,int16_t w,int16_t h)
{
    uint32_t saved=canvas;ra8876_set_canvas(dst);ra8876_fill_rect(x,y,w,h);ra8876_set_canvas(saved);
}

