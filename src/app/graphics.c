#include "graphics.h"
#include "graphics_backend.h"
#include "text.h"
#include "theme.h"
#include <string.h>
#include <limits.h>

#define SURFACES 32u
#define TEXT_ENTRIES 96u
#define TEXT_LENGTH 96u
#define DIRTY_MAX 32u
static const gfx_rect_t screen={0,0,LCD_WIDTH,LCD_HEIGHT};
static gfx_rect_t clip={0,0,LCD_WIDTH,LCD_HEIGHT};
static uint16_t foreground;
static uint8_t started, initialized;
static gfx_rect_t dirty[DIRTY_MAX];
static unsigned dirty_n;
static gfx_stats_t counters;
static uint16_t scanline[LCD_WIDTH];
static struct { gfx_rect_t rect; gfx_paint_t paint; } surfaces[SURFACES];
static unsigned surface_n;
static struct { gfx_paint_t paint; int16_t w,h; uint8_t radius,valid; } gradients[GFX_GRADIENT_COUNT];
static unsigned gradient_next;
static struct {
    const font_t *font; gfx_paint_t paint; uint16_t fg; int16_t x,y,w,h,top;
    uint32_t page; int16_t sx,sy; uint8_t valid; char text[TEXT_LENGTH];
} texts[TEXT_ENTRIES];
static unsigned text_next, shelf_page;
static int16_t shelf_x,shelf_y,shelf_h;

#if defined(_MSC_VER)
#define GFX_WEAK
#else
#define GFX_WEAK __attribute__((weak))
#endif
GFX_WEAK void gfx_yield(void) { }
GFX_WEAK uint32_t gfx_clock_cycles(void) { return 0; }
uint64_t gfx_gpu_cycles,gfx_spi_cycles;
uint32_t gfx_accel_errors;
const gfx_stats_t *gfx_stats(void) { return &counters; }
int gfx_intersect(gfx_rect_t a,gfx_rect_t b,gfx_rect_t *out)
{
    int32_t x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int32_t right=(int32_t)a.x+a.w<(int32_t)b.x+b.w?(int32_t)a.x+a.w:(int32_t)b.x+b.w;
    int32_t bottom=(int32_t)a.y+a.h<(int32_t)b.y+b.h?(int32_t)a.y+a.h:(int32_t)b.y+b.h;
    if(a.w<=0||a.h<=0||b.w<=0||b.h<=0||right<=x||bottom<=y) return 0;
    *out=(gfx_rect_t){(int16_t)x,(int16_t)y,(int16_t)(right-x),(int16_t)(bottom-y)}; return 1;
}
static int contains(gfx_rect_t a,gfx_rect_t b)
{
    return b.x>=a.x&&b.y>=a.y&&(int32_t)b.x+b.w<=(int32_t)a.x+a.w&&(int32_t)b.y+b.h<=(int32_t)a.y+a.h;
}
void gfx_clip(gfx_rect_t r) { if(!gfx_intersect(r,screen,&clip)) clip=(gfx_rect_t){0,0,0,0}; }
void gfx_unclip(void) { clip=screen; }
uint16_t gfx_blend(uint16_t bg,uint16_t fg,uint8_t a)
{
    int32_t r=(((bg>>11)&31)*(255-a)+((fg>>11)&31)*a+127)/255;
    int32_t g=(((bg>>5)&63)*(255-a)+((fg>>5)&63)*a+127)/255;
    int32_t b=((bg&31)*(255-a)+(fg&31)*a+127)/255;
    return (uint16_t)((r<<11)|(g<<5)|b);
}
gfx_paint_t gfx_solid(uint16_t c,gfx_rect_t r) { return (gfx_paint_t){c,c,r,0}; }
uint16_t gfx_sample(gfx_paint_t p,int16_t x,int16_t y)
{
    int32_t n=p.horizontal?p.bounds.w:p.bounds.h;
    int32_t i=p.horizontal?(int32_t)x-p.bounds.x:(int32_t)y-p.bounds.y;
    if(n<=1||i<=0) return p.first;
    if(i>=n-1) return p.last;
    return gfx_blend(p.first,p.last,(uint8_t)((i*255+(n-1)/2)/(n-1)));
}
gfx_paint_t gfx_background(gfx_rect_t r,uint16_t bg)
{
    for(unsigned i=surface_n;i>0;i--)
        if(surfaces[i-1].paint.first==bg&&contains(surfaces[i-1].rect,r)) return surfaces[i-1].paint;
    return gfx_solid(bg,r);
}
static void damage(gfx_rect_t r)
{
    gfx_rect_t q;
    if(!gfx_intersect(r,screen,&q)) return;
    /* Merge touching damage: a border is one card, not hundreds of scanline copies. */
    for(unsigned i=0;i<dirty_n;) {
        gfx_rect_t d=dirty[i], overlap;
        gfx_rect_t expanded={d.x-1,d.y-1,d.w+2,d.h+2};
        if(gfx_intersect(expanded,q,&overlap)) {
            int16_t left=d.x<q.x?d.x:q.x,top=d.y<q.y?d.y:q.y;
            int16_t right=d.x+d.w>q.x+q.w?d.x+d.w:q.x+q.w;
            int16_t bottom=d.y+d.h>q.y+q.h?d.y+d.h:q.y+q.h;
            q=(gfx_rect_t){left,top,right-left,bottom-top};
            dirty[i]=dirty[--dirty_n]; i=0;
        } else i++;
    }
    if(dirty_n<DIRTY_MAX) dirty[dirty_n++]=q;
    else { dirty[0]=screen; dirty_n=1; }
}
void gfx_begin(void)
{
    if(started) return;
    if(!initialized) {
        for(int16_t y=0;y<LCD_HEIGHT;y+=32) {
            gb_bte_copy(RA_PAGE(0),0,y,GFX_WORK,0,y,LCD_WIDTH,32);gfx_yield();
        }
        initialized=1;
    }
    started=1; dirty_n=0; gb_set_canvas(GFX_WORK);
}
void gfx_end(void)
{
    if(!started) return;
    for(unsigned i=0;i<dirty_n;i++) {
        gfx_rect_t r=dirty[i];
        /* Limit copy height so servicing continues even during full page changes. */
        for(int16_t y=0;y<r.h;y+=32) {
            int16_t h=r.h-y<32?r.h-y:32;
            gb_bte_copy(GFX_WORK,r.x,r.y+y,RA_PAGE(0),r.x,r.y+y,r.w,h);
            counters.copies++; gfx_yield();
        }
    }
    gb_set_canvas(RA_PAGE(0)); started=0; dirty_n=0;
}
void gfx_cache_reset(void)
{ memset(gradients,0,sizeof(gradients)); memset(texts,0,sizeof(texts));
    text_next=gradient_next=shelf_page=0; shelf_x=shelf_y=shelf_h=0;
}
void gfx_invalidate(void) { surface_n=0;gfx_cache_reset(); }
void gfx_copy(uint32_t src,gfx_rect_t r,uint32_t dst,int16_t x,int16_t y)
{
    gfx_rect_t source,destination={x,y,r.w,r.h},q;
    if(src>RA_VRAM_SIZE-RA_PAGE_BYTES||dst>RA_VRAM_SIZE-RA_PAGE_BYTES) return;
    if(!gfx_intersect(r,screen,&source)) return;
    destination.x+=(int16_t)(source.x-r.x); destination.y+=(int16_t)(source.y-r.y);
    destination.w=source.w;destination.h=source.h;
    if(!gfx_intersect(destination,dst==GFX_WORK?clip:screen,&q)) return;
    source.x+=(int16_t)(q.x-destination.x);source.y+=(int16_t)(q.y-destination.y);
    gb_bte_copy(src,source.x,source.y,dst,q.x,q.y,q.w,q.h); counters.copies++;
    if(dst==GFX_WORK) damage(q);
}
static int inset(int16_t row,int16_t h,uint8_t radius)
{
    int i=row<h-1-row?row:h-1-row;
    if(i>=radius) return 0;
    /* Integer circle edges; radius is capped at 8 pixels. */
    int dy=radius-1-i,dx=radius;
    while(dx>0&&dx*dx+dy*dy>radius*radius) dx--;
    return radius-dx;
}
static void raster(gfx_rect_t r,gfx_paint_t p,uint8_t radius)
{
    if(radius>8) radius=8;
    if(radius>r.w/2) radius=(uint8_t)(r.w/2);
    if(radius>r.h/2) radius=(uint8_t)(r.h/2);
    for(int16_t row=0;row<r.h;) {
        int cut=inset(row,r.h,radius),run=1;
        if(!p.horizontal||p.first==p.last) {
            uint16_t colour=gfx_sample(p,r.x,r.y+row);
            /* RGB565 gradients contain repeated rows. Submit equal rows as one rectangle,
               keeping rounded cutouts exact and each operation at most 32 rows tall. */
            while(run<32&&row+run<r.h&&inset(row+run,r.h,radius)==cut&&
                  gfx_sample(p,r.x,r.y+row+run)==colour)run++;
            gb_set_fg(colour);
            gb_fill_rect(r.x+cut,r.y+row,r.w-2*cut,(int16_t)run); counters.fills++;
        } else {
            for(int16_t x=0;x<r.w-2*cut;x++) scanline[x]=gfx_sample(p,r.x+cut+x,r.y+row);
            gb_blit_begin(r.x+cut,r.y+row,r.w-2*cut,1);
            gb_blit_pixels(scanline,(uint32_t)(r.w-2*cut));gb_blit_end();
            counters.spi_pixels+=(uint32_t)(r.w-2*cut);
        }
        row+=(int16_t)run;
        if(run>1||(row&15)==0)gfx_yield();
    }
}
static int paint_equal(gfx_paint_t a,gfx_paint_t b)
{
    return a.first==b.first&&a.last==b.last&&a.horizontal==b.horizontal&&
        (a.first==a.last||(a.bounds.x==b.bounds.x&&a.bounds.y==b.bounds.y&&a.bounds.w==b.bounds.w&&a.bounds.h==b.bounds.h));
}
void gfx_paint(gfx_rect_t r,gfx_paint_t p,uint8_t radius)
{
    gfx_rect_t q; if(!gfx_intersect(r,clip,&q)) return;
    gfx_begin();
    if(p.first==p.last&&!radius) {
        gb_set_fg(p.first);
        for(int16_t y=0;y<q.h;y+=32) {
            gb_fill_rect(q.x,q.y+y,q.w,q.h-y<32?q.h-y:32); counters.fills++;gfx_yield();
        }
        damage(q);return;
    }
    if(r.w>LCD_WIDTH||r.h>LCD_HEIGHT||r.w<8||r.h<8) {
        raster(q,p,0);damage(q);return;
    }
    /* Cache complete gradient tiles in fixed pages, preserving clipping and source origin. */
    gfx_paint_t local=p;
    local.bounds.x-=r.x;local.bounds.y-=r.y;
    unsigned entry;
    for(entry=0;entry<GFX_GRADIENT_COUNT;entry++)
        if(gradients[entry].valid&&gradients[entry].w==r.w&&gradients[entry].h==r.h&&
           gradients[entry].radius==radius&&paint_equal(gradients[entry].paint,local)) break;
    if(entry==GFX_GRADIENT_COUNT) {
        entry=gradient_next++%GFX_GRADIENT_COUNT; counters.gradient_misses++;
        gb_set_canvas(GFX_GRADIENT_FIRST+entry*RA_PAGE_BYTES);
        /* Cutout corners match the ground, and are included in the cached tile. */
        gb_set_fg(C_BG); gb_fill_rect(0,0,r.w,r.h);
        raster((gfx_rect_t){0,0,r.w,r.h},local,radius);
        gradients[entry].paint=local;gradients[entry].w=r.w;gradients[entry].h=r.h;
        gradients[entry].radius=radius;gradients[entry].valid=1;
        gb_set_canvas(GFX_WORK);
    } else counters.gradient_hits++;
    gfx_copy(GFX_GRADIENT_FIRST+entry*RA_PAGE_BYTES,(gfx_rect_t){q.x-r.x,q.y-r.y,q.w,q.h},GFX_WORK,q.x,q.y);
}
void gfx_card(gfx_rect_t r,uint16_t c)
{
    gfx_paint_t p={c,gfx_blend(C_BG,c,212),r,0};
    for(unsigned i=0;i<surface_n;i++) if(surfaces[i].rect.x==r.x&&surfaces[i].rect.y==r.y) {
        surfaces[i].rect=r;surfaces[i].paint=p;gfx_paint(r,p,5);return;
    }
    if(surface_n==SURFACES) { memmove(surfaces,surfaces+1,(SURFACES-1)*sizeof(surfaces[0]));surface_n--; }
    surfaces[surface_n].rect=r;surfaces[surface_n++].paint=p;gfx_paint(r,p,5);
}
void gfx_restore(gfx_rect_t r,uint16_t bg) { gfx_paint(r,gfx_background(r,bg),0); }
void gfx_set_fg(uint16_t c) { foreground=c; }
void gfx_fill_rect(int16_t x,int16_t y,int16_t w,int16_t h)
{
    gfx_rect_t r={x,y,w,h},q;
    if(!gfx_intersect(r,clip,&q)) return;
    if(foreground==C_BG&&x==0&&y==0&&w==LCD_WIDTH) surface_n=0;
    int card=foreground==C_CELL||foreground==C_CELL_HOT||foreground==C_CELL_DOWN||foreground==C_STOP;
    gfx_paint_t p=gfx_background(r,foreground);
    if(card&&p.first==p.last&&w>=60&&h>=45) gfx_card(r,foreground);
    else gfx_paint(r,p,0);
}
void gfx_border(gfx_rect_t r,uint8_t radius,uint8_t t,uint16_t c)
{
    if(!t) return;
    if(t>8) t=8;
    unsigned rad=radius>8?8:radius;
    int16_t middle_start=(int16_t)(rad>t?rad:t);
    int16_t middle_end=(int16_t)(r.h-middle_start);
    for(int16_t row=0;row<r.h;) {
        /* Only rounded corners need individual rows. Straight sides are hardware rectangles. */
        if(middle_end>middle_start&&row==middle_start) {
            gfx_paint((gfx_rect_t){r.x,r.y+row,t,middle_end-row},gfx_solid(c,r),0);
            gfx_paint((gfx_rect_t){r.x+r.w-t,r.y+row,t,middle_end-row},gfx_solid(c,r),0);
            row=middle_end;continue;
        }
        int cut=inset(row,r.h,(uint8_t)rad);
        int top=row<t||row>=r.h-t;
        gfx_paint((gfx_rect_t){r.x+cut,r.y+row,top?r.w-2*cut:t,1},gfx_solid(c,r),0);
        if(!top) gfx_paint((gfx_rect_t){r.x+r.w-cut-t,r.y+row,t,1},gfx_solid(c,r),0);
        row++;
    }
}
void gfx_line(int16_t x0,int16_t y0,int16_t x1,int16_t y1)
{
    /* Clip the segment before sending coordinates to the unsigned controller registers. */
    int dx=x1>x0?x1-x0:x0-x1,dy=y1>y0?y1-y0:y0-y1;
    int sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy;
    int have=0;int16_t ax=0,ay=0,bx=0,by=0;
    for(;;) {
        if(x0>=clip.x&&y0>=clip.y&&x0<clip.x+clip.w&&y0<clip.y+clip.h) {
            if(!have){ax=x0;ay=y0;have=1;}bx=x0;by=y0;
        }
        if(x0==x1&&y0==y1) break;
        int twice=2*err;if(twice>-dy){err-=dy;x0+=sx;}if(twice<dx){err+=dx;y0+=sy;}
    }
    if(!have) return;
    gfx_begin();gb_set_fg(foreground);gb_line(ax,ay,bx,by);
    damage((gfx_rect_t){ax<bx?ax:bx,ay<by?ay:by,(int16_t)(bx>ax?bx-ax+1:ax-bx+1),(int16_t)(by>ay?by-ay+1:ay-by+1)});
}
void gfx_meter(gfx_rect_t r,int16_t extent,int horizontal,uint16_t c)
{
    gfx_paint(r,gfx_solid(C_TRACK,r),3);
    gfx_rect_t inner={r.x+2,r.y+2,r.w-4,r.h-4};
    int16_t span=horizontal?inner.w:inner.h;
    if(extent>span) extent=span;
    if(extent<=0) return;
    if(horizontal) inner.w=extent;else {inner.y+=inner.h-extent;inner.h=extent;}
    gfx_paint(inner,(gfx_paint_t){c,C_LIVE,inner,(uint8_t)horizontal},2);
}
void gfx_checker(gfx_rect_t r,uint16_t c,uint8_t alpha)
{
    for(int16_t y=0;y<r.h;y+=8) for(int16_t x=0;x<r.w;x+=8) {
        uint16_t bg=((x/8+y/8)&1)?C_TITLE:C_INDICATOR;
        gfx_rect_t tile={r.x+x,r.y+y,r.w-x<8?r.w-x:8,r.h-y<8?r.h-y:8};
        gfx_paint(tile,gfx_solid(gfx_blend(bg,c,alpha),tile),0);
    }
}
static const font_glyph_t *glyph(const font_t *f,unsigned char c)
{
    if(c<f->first||c>f->last) c=('?'>=f->first&&'?'<=f->last)?'?':f->first;
    return f->glyphs+c-f->first;
}
int16_t gfx_text(const font_t *f,int16_t x,int16_t y,const char *s,uint16_t fg,uint16_t bg)
{
    int16_t width=text_width(f,s),top=f->line_height,bottom=0;
    if(width<=0) return 0;
    for(const char *t=s;*t;t++) {
        const font_glyph_t *g=glyph(f,(unsigned char)*t);if(!g->w) continue;
        if(g->by<top) top=g->by;
        if(g->by+g->h>bottom) bottom=g->by+g->h;
    }
    if(top<0) top=0;
    if(bottom>f->line_height) bottom=f->line_height;
    gfx_rect_t r={x,y+top,width,bottom-top},q;
    if(!gfx_intersect(r,clip,&q)) return width;
    gfx_begin();gfx_paint_t paint=gfx_background(r,bg);
    /* Cache key includes the sampled gradient coordinates, so page/layout changes cannot reuse
       a glyph blended against a different plate. Solid backgrounds are position-independent. */
    gfx_paint_t key=paint;key.bounds.x-=x;key.bounds.y-=y;
    unsigned entry;
    for(entry=0;entry<TEXT_ENTRIES;entry++)
        if(texts[entry].valid&&texts[entry].font==f&&texts[entry].fg==fg&&
           paint_equal(texts[entry].paint,key)&&strcmp(texts[entry].text,s)==0) break;
    if(entry==TEXT_ENTRIES) {
        counters.text_misses++;
        /* Oversized strings are rendered clipped without caching. */
        int cache=strlen(s)<TEXT_LENGTH&&width<=LCD_WIDTH&&r.h<=LCD_HEIGHT;
        int16_t sx=q.x,sy=q.y;uint32_t page=GFX_WORK;
        if(cache) {
            if(shelf_x+width>LCD_WIDTH){shelf_x=0;shelf_y+=shelf_h;shelf_h=0;}
            if(shelf_y+r.h>LCD_HEIGHT){shelf_page=(shelf_page+1)%GFX_TEXT_PAGES;shelf_x=shelf_y=shelf_h=0;
                for(unsigned i=0;i<TEXT_ENTRIES;i++) if(texts[i].page==GFX_TEXT_FIRST+shelf_page*RA_PAGE_BYTES) texts[i].valid=0;}
            sx=shelf_x;sy=shelf_y;page=GFX_TEXT_FIRST+shelf_page*RA_PAGE_BYTES;
            shelf_x+=width;if(r.h>shelf_h)shelf_h=r.h;
            gb_set_canvas(page);
        }
        int16_t draw_w=cache?width:q.w,draw_h=cache?r.h:q.h;
        int16_t source_x=cache?x:q.x,source_y=cache?r.y:q.y;
        gb_blit_begin(sx,sy,draw_w,draw_h);
        for(int16_t row=0;row<draw_h;row++) {
            int16_t logical_y=source_y+row,logical_row=logical_y-y;
            for(int16_t col=0;col<draw_w;col++) scanline[col]=gfx_sample(paint,source_x+col,logical_y);
            int32_t pen=x;
            for(const char *t=s;*t;t++) {
                const font_glyph_t *g=glyph(f,(unsigned char)*t);
                if(g->w&&logical_row>=g->by&&logical_row<g->by+g->h) {
                    const uint8_t *cov=f->pixels+g->off+(uint32_t)(logical_row-g->by)*g->w;
                    for(unsigned col=0;col<g->w;col++) {
                        int32_t dest=pen+g->bx+col-source_x;
                        if(dest>=0&&dest<draw_w) scanline[dest]=gfx_blend(scanline[dest],fg,cov[col]);
                    }
                }
                pen+=g->adv;
            }
            gb_blit_pixels(scanline,(uint32_t)draw_w);counters.spi_pixels+=(uint32_t)draw_w;
            /* End the active window before servicing callbacks. Reopen at the next row. */
            if((row&7)==7&&row+1<draw_h) {
                gb_blit_end();gfx_yield();gb_set_canvas(page);
                gb_blit_begin(sx,sy+row+1,draw_w,draw_h-row-1);
            }
        }
        gb_blit_end();gb_set_canvas(GFX_WORK);
        if(!cache){damage(q);return width;}
        entry=text_next++%TEXT_ENTRIES;texts[entry].font=f;texts[entry].fg=fg;texts[entry].paint=key;
        texts[entry].w=width;texts[entry].h=r.h;texts[entry].top=top;
        texts[entry].sx=sx;texts[entry].sy=sy;texts[entry].page=page;
        strcpy(texts[entry].text,s);texts[entry].valid=1;
    } else counters.text_hits++;
    gfx_copy(texts[entry].page,(gfx_rect_t){texts[entry].sx+q.x-r.x,texts[entry].sy+q.y-r.y,q.w,q.h},GFX_WORK,q.x,q.y);
    return width;
}
