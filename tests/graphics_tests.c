#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "graphics.h"
#include "text.h"
#include "theme.h"
#include "sim_hal.h"
#include "ui.h"
#include "ui_state.h"
#include "surface.h"
#include "fixtures.h"
static uint16_t reference[LCD_WIDTH*LCD_HEIGHT];
static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static void clean(void) { gfx_end();sim_reset();gfx_invalidate();gfx_unclip(); }
static void base(gfx_rect_t r) { gfx_begin();gfx_card(r,C_CELL);gfx_end(); }
static void primitive_tests(void)
{
    gfx_rect_t r={-20,-20,40,40};clean();gfx_begin();gfx_clip((gfx_rect_t){5,5,10,10});
    gfx_paint(r,gfx_solid(0xffff,r),0);gfx_end();
    for(int y=0;y<40;y++)for(int x=0;x<40;x++) CHECK(sim_framebuffer()[y*800+x]==(x>=5&&x<15&&y>=5&&y<15?0xffff:0));
    CHECK(gfx_blend(0xffff,0,0)==0xffff);CHECK(gfx_blend(0xffff,0,255)==0);
    CHECK(gfx_blend(0xffff,0,128)==0x7bef);
    clean();gfx_begin();r=(gfx_rect_t){20,20,100,30};gfx_paint_t p={0xf800,0x001f,r,1};
    gfx_paint(r,p,0);CHECK(sim_framebuffer()[20*800+20]==0);gfx_end();
    CHECK(sim_framebuffer()[20*800+20]==0xf800);CHECK(sim_framebuffer()[20*800+119]==0x001f);
    gfx_begin();gfx_clip((gfx_rect_t){40,20,20,30});gfx_paint(r,p,0);gfx_end();
    CHECK(sim_framebuffer()[20*800+40]==gfx_sample(p,40,20));
    clean();gfx_begin();r=(gfx_rect_t){-100,-100,1200,800};p=(gfx_paint_t){0,0xffff,r,1};gfx_paint(r,p,0);gfx_end();
    CHECK(sim_framebuffer()[0]==gfx_sample(p,0,0));CHECK(sim_framebuffer()[799]==gfx_sample(p,799,0));
    clean();gfx_begin();r=(gfx_rect_t){40,40,60,60};p=(gfx_paint_t){0,0xffff,r,0};gfx_paint(r,p,5);gfx_end();
    CHECK(sim_framebuffer()[40*800+40]==C_BG);CHECK(sim_framebuffer()[99*800+70]==0xffff);
    gfx_begin();gfx_copy(RA_PAGE(0),(gfx_rect_t){40,40,60,60},GFX_WORK,140,40);gfx_end();
    for(int y=0;y<60;y++)for(int x=0;x<60;x++)CHECK(sim_framebuffer()[(40+y)*800+40+x]==sim_framebuffer()[(40+y)*800+140+x]);
    puts("  clipping, endpoint quantization, rounded edges, staging and VRAM copies passed");
}
static void background_tests(void)
{
    clean();gfx_rect_t card={20,20,200,184};base(card);memcpy(reference,sim_framebuffer(),sizeof(reference));
    gfx_begin();text_draw(&font_value,32,90,"-1.25",C_VALUE,C_CELL);gfx_end();
    gfx_rect_t value={22,90,196,100};gfx_begin();gfx_restore(value,C_CELL);gfx_end();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    gfx_begin();text_draw(&font_value,32,90,"-1.25",C_VALUE,C_CELL);gfx_end();
    memcpy(reference,sim_framebuffer(),sizeof(reference));uint32_t hit=gfx_stats()->text_hits;
    gfx_begin();gfx_restore(value,C_CELL);text_draw(&font_value,32,90,"-1.25",C_VALUE,C_CELL);gfx_end();
    CHECK(gfx_stats()->text_hits>hit);CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    gfx_invalidate();base(card);gfx_begin();text_draw(&font_value,32,90,"-1.25",C_VALUE,C_CELL);gfx_end();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    gfx_begin();text_draw(&font_display,-40,-40,"-999.99",C_VALUE,C_BG);gfx_end();
    gfx_begin();text_draw(&font_ui,780,470,"clipped",C_VALUE,C_BG);gfx_end();
    CHECK(text_width(&font_value,"11111111")==text_width(&font_value,"88888888"));
    puts("  actual gradient background, restoration, cache hit/invalidation and clipped glyphs passed");
}
static void incremental_tests(void)
{
    clean();ui_render_demo();
    ui_state_touch(1,0);ui_render();ui_state_touch(0,1000);ui_render();
    ui_state_push(0,1,1020);ui_state_push(0,0,1040);ui_render();
    ui_state_touch(1u<<4,1060);ui_render();ui_state_touch(0,1080);ui_render();
    ui_state_push(0,1,1100);ui_state_push(0,0,1120);ui_render();
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);ui_render();
    unsigned diff=0;for(unsigned i=0;i<LCD_WIDTH*LCD_HEIGHT;i++) if(reference[i]!=sim_framebuffer()[i]) diff++;
    CHECK(diff==0);
    puts("  focus/touch release and editor orientation match a clean repaint");
}
static void operator_tests(void)
{
    clean();surf_demo_operator();ui_state_init();ui_reset();ui_render();
    memcpy(reference,sim_framebuffer(),sizeof(reference));
    ui_state_touch(1,100);ui_state_push(0,1,120);ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))!=0);
    ui_state_push(0,0,140);ui_state_touch(0,160);ui_render();
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    ui_state_reveal(8);ui_render();ui_state_touch(1,180);ui_state_push(0,1,200);ui_render();
    ui_state_push(0,0,220);ui_state_touch(0,240);ui_render();
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    puts("  both operator pages restore touched/pressed cues to a clean repaint");
}
static void eviction_tests(void)
{
    clean();gfx_rect_t card={20,20,200,184},value={24,80,180,95};base(card);
    char text[32];
    for(unsigned i=0;i<500;i++){text_format(text,sizeof(text),(double)i/100,2);gfx_begin();gfx_restore(value,C_CELL);text_draw(&font_value,32,90,text,C_VALUE,C_CELL);gfx_end();}
    memcpy(reference,sim_framebuffer(),sizeof(reference));gfx_invalidate();base(card);gfx_begin();text_draw(&font_value,32,90,text,C_VALUE,C_CELL);gfx_end();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    CHECK(gfx_stats()->text_misses>=500);
    puts("  500 changed values remain identical after cache shelf/entry eviction");
}
static void layout_tests(void)
{
    for(unsigned n=1;n<=4;n++){clean();fixture_sparse(n,0);ui_reset();ui_render();
        for(unsigned k=0;k<4;k++)CHECK(ui_state_field_for(k)==-1);
        for(unsigned k=0;k<n;k++){if(ui_state_field_for(k+4)!=(int)k)fprintf(stderr,"n=%u k=%u actual=%d count=%u rev=%llu\n",n,k,(int)ui_state_field_for(k+4),surf_field_count(),(unsigned long long)surf_applied_revision());CHECK(ui_state_field_for(k+4)==(int)k);}
        memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    }
    clean();fixture_sparse(4,1);ui_reset();ui_render();
    CHECK(surf_field(1)==NULL);CHECK(surf_field(6)!=NULL);CHECK(ui_state_reveal(9));ui_render();
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    CHECK(text_width(&font_compact,"111111")==text_width(&font_compact,"888888"));
    puts("  sparse pages, lower-row placement, long suffixes, negative and read-only fixtures passed");
}
int main(void)
{
    primitive_tests();background_tests();incremental_tests();operator_tests();eviction_tests();layout_tests();
    printf("%u graphics checks passed\n",checks);return 0;
}
