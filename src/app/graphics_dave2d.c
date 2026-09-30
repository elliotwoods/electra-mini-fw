/* Explicit benchmark backend: hardware geometry/copies in MCU SDRAM, dirty SPI upload to RA. */
#include "graphics.h"
#include "graphics_backend.h"
#include "dave_driver.h"
#include "diag.h"
#include "bsp.h"
#include <string.h>
static uint16_t memory[GFX_VRAM_END/2] __attribute__((section(".graphics_sdram"),aligned(8)));
static d2_device *device;
static int tried;
static d2_renderbuffer *commands;
static uint32_t canvas;
static uint16_t foreground;
static gfx_rect_t window;
static uint32_t cursor;
extern int electra_dave_failed(void);
static int ready(void)
{
    if(!tried) {
        tried=1;
        if(bsp_sdram_selftest()!=0) {emp_diag(EMP_SEV_ERROR,EMP_DIAG_GRAPHICS_FAULT,0,"D/AVE SDRAM selftest failed");gfx_accel_errors++;return 0;}
        device=d2_opendevice(d2_df_no_dlist|d2_df_no_irq|d2_df_no_fbcache|d2_df_no_texcache);
        if(!device||d2_inithw(device,0)!=D2_OK) {device=NULL;emp_diag(EMP_SEV_ERROR,EMP_DIAG_GRAPHICS_FAULT,0,"D/AVE initialization failed");gfx_accel_errors++;return 0;}
        commands=d2_getrenderbuffer(device,0);d2_selectrenderbuffer(device,commands);
        d2_setantialiasing(device,0);d2_setblendmode(device,d2_bm_one,d2_bm_zero);
        /* CPU coverage composition is identical across backends; caches are in bounded SDRAM. */
        memset(memory,0,sizeof(memory));
    }
    return device&&!electra_dave_failed();
}
static void check(d2_s32 rc)
{
    if(rc!=D2_OK) {gfx_accel_errors++;emp_diag(EMP_SEV_ERROR,EMP_DIAG_GRAPHICS_FAULT,(uint32_t)rc,"D/AVE drawing rejected");}
}
static void submit(void)
{
    check(d2_executerenderbuffer(device,commands,0));
    check(d2_flushframe(device));
    check(d2_selectrenderbuffer(device,commands));
}
void gb_set_canvas(uint32_t a)
{
    if(a>GFX_VRAM_END-RA_PAGE_BYTES)return;
    canvas=a;if(ready())check(d2_framebuffer(device,memory+a/2,LCD_WIDTH,LCD_WIDTH,LCD_HEIGHT,d2_mode_rgb565));
}
void gb_set_fg(uint16_t c)
{
    foreground=c;
    uint32_t rgb=(((c>>11)&31)*255/31)<<16|(((c>>5)&63)*255/63)<<8|(c&31)*255/31;
    if(ready())check(d2_setcolor(device,0,rgb));
}
void gb_fill_rect(int16_t x,int16_t y,int16_t w,int16_t h)
{
    if(!ready()||w<=0||h<=0)return;
    check(d2_renderbox(device,(d2_point)(x*16),(d2_point)(y*16),(d2_width)(w*16),(d2_width)(h*16)));
    submit();
}
void gb_line(int16_t x,int16_t y,int16_t xx,int16_t yy)
{
    /* Match the RA8876 integer line raster exactly, using hardware one-pixel geometry. */
    int dx=xx>x?xx-x:x-xx,dy=yy>y?yy-y:y-yy,sx=x<xx?1:-1,sy=y<yy?1:-1,err=dx-dy;
    for(;;){gb_fill_rect(x,y,1,1);if(x==xx&&y==yy)break;int twice=2*err;if(twice>-dy){err-=dy;x+=sx;}if(twice<dx){err+=dx;y+=sy;}}
}
void gb_bte_copy(uint32_t src,int16_t sx,int16_t sy,uint32_t dst,int16_t dx,int16_t dy,int16_t w,int16_t h)
{
    if(!ready()||src>GFX_VRAM_END-RA_PAGE_BYTES||dst>GFX_VRAM_END-RA_PAGE_BYTES)return;
    uint32_t saved=canvas;gb_set_canvas(dst);
    check(d2_setblitsrc(device,memory+src/2,LCD_WIDTH,LCD_WIDTH,LCD_HEIGHT,d2_mode_rgb565));
    check(d2_blitcopy(device,w,h,sx,sy,(d2_width)(w*16),(d2_width)(h*16),(d2_point)(dx*16),(d2_point)(dy*16),0));
    submit();
    if(dst==RA_PAGE(0)&&!electra_dave_failed()) {
        ra8876_set_canvas(RA_PAGE(0));ra8876_blit_begin(dx,dy,w,h);
        for(int16_t row=0;row<h;row++) {
            ra8876_blit_pixels(memory+dst/2+(dy+row)*LCD_WIDTH+dx,(uint32_t)w);
            if((row&7)==7&&row+1<h){ra8876_blit_end();gfx_yield();ra8876_blit_begin(dx,dy+row+1,w,h-row-1);}
        }
        ra8876_blit_end();
    }
    gb_set_canvas(saved);
}
void gb_blit_begin(int16_t x,int16_t y,int16_t w,int16_t h)
{
    window=(gfx_rect_t){x,y,w,h};cursor=0;if(ready())check(d2_flushframe(device));
}
void gb_blit_pixels(const uint16_t *p,uint32_t n)
{
    if(!ready()||window.w<=0)return;
    for(uint32_t i=0;i<n&&cursor<(uint32_t)window.w*window.h;i++,cursor++) {
        int x=window.x+(int)(cursor%(uint32_t)window.w),y=window.y+(int)(cursor/(uint32_t)window.w);
        if(x>=0&&x<LCD_WIDTH&&y>=0&&y<LCD_HEIGHT)memory[canvas/2+y*LCD_WIDTH+x]=p[i];
    }
}
void gb_blit_end(void) { }
