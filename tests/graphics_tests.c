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
#include "sim_wire.h"
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
static void switch_tests(void)
{
    clean();gfx_rect_t card={20,20,220,184},pill={40,70,148,54};base(card);
    uint16_t corner=sim_framebuffer()[70*800+40];
    gfx_begin();gfx_paint(pill,gfx_solid(C_TRACK,pill),27);gfx_end();
    CHECK(sim_framebuffer()[70*800+40]==corner);
    CHECK(sim_framebuffer()[97*800+114]==C_TRACK);
    memcpy(reference,sim_framebuffer(),sizeof(reference));
    gfx_begin();gfx_paint(pill,gfx_solid(C_TRACK,pill),27);gfx_end();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    clean();base(card);memcpy(reference,sim_framebuffer(),sizeof(reference));
    gfx_begin();gfx_clip((gfx_rect_t){100,80,20,30});gfx_paint(pill,gfx_solid(C_VALUE,pill),27);gfx_unclip();gfx_end();
    for(int y=70;y<124;y++)for(int x=40;x<188;x++)
        if(x<100||x>=120||y<80||y>=110) CHECK(sim_framebuffer()[y*800+x]==reference[y*800+x]);

    clean();fixture_toggles();ui_render();
    for(unsigned pass=0;pass<4;pass++) {
        for(unsigned k=0;k<8;k++)surf_set_bool((uint16_t)k,(uint8_t)((k+pass+1)%2));
        ui_render();memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
        CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    }
    for(unsigned knob=0;knob<=4;knob+=4) {
        clean();fixture_toggles();ui_state_push(knob,1,100);ui_state_push(knob,0,120);ui_render();
        for(unsigned state=0;state<2;state++) {
            surf_set_bool((uint16_t)knob,(uint8_t)state);ui_render();
            memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
            CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
        }
    }
    puts("  rounded switch overlays preserve corners/clipping; both states and editor orientations restore cleanly");
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
static void highlight_tests(void)
{
    for(unsigned layout=0;layout<3;layout++) {
        clean(); if(layout==0)fixture_toggles(); else {surf_demo_operator();ui_state_init();if(layout==2)ui_state_reveal(8);}
        ui_reset();ui_render();ui_render();
        for(unsigned k=0;k<8;k++) {
            uint16_t id=(uint16_t)(layout==2?8+k:k);
            if(!surf_field(id))continue;
            CHECK(ui_state_highlight(id,1500));ui_render();
            memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
            CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
            int32_t knob=ui_state_knob_for(id);ui_state_touch((uint16_t)(1u<<knob),10);ui_render();
            memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
            CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
            ui_state_touch(0,20);ui_state_highlight(-1,0);ui_render();
            memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
            CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
        }
    }
    for (unsigned layout=0;layout<3;layout++) {
        clean();if(layout==0)fixture_toggles();else{surf_demo_operator();ui_state_init();if(layout==2)ui_state_reveal(8);}
        ui_reset();ui_render();
        unsigned base=layout==2?8:0;
        uint64_t group=(UINT64_C(1)<<base)|(UINT64_C(1)<<(base+1));
        CHECK(ui_state_highlights(group,1500));ui_render();
        memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
        CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
        CHECK(ui_state_highlights(UINT64_C(1)<<(base+1),1500));ui_render();
        memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
        CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
        CHECK(ui_state_highlights(0,0));ui_render();
        memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
        CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    }
    puts("  single/group outline movement, local precedence and release match clean repaints in all layouts");
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
static unsigned count_colour(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t c)
{
    unsigned n=0;
    for(int16_t j=y;j<y+h;j++)for(int16_t i=x;i<x+w;i++) if(sim_framebuffer()[j*LCD_WIDTH+i]==c) n++;
    return n;
}
/* EMP/1.6: the estop plate is the operator stop card, full cell; tones are the status ink. */
static void status_tests(void)
{
    const int16_t cw=(LCD_WIDTH-24-18)/4, ch=184, x0=12, y0=6, x1=(int16_t)(12+cw+6);
    clean();fixture_status(EMP_TONE_OK,"ARMED");ui_render();
    gfx_paint_t stop={C_STOP,gfx_blend(C_BG,C_STOP,212),{x0,y0,cw,ch},0};
    gfx_paint_t cell={C_CELL,gfx_blend(C_BG,C_CELL,212),{x1,y0,cw,ch},0};
    CHECK(ui_state_field_for(0)==0&&surf_field_is_estop(surf_field(0)));
    CHECK(sim_framebuffer()[(y0+ch-20)*LCD_WIDTH+x0+20]==gfx_sample(stop,(int16_t)(x0+20),(int16_t)(y0+ch-20)));
    CHECK(sim_framebuffer()[(y0+ch-20)*LCD_WIDTH+x0+cw-20]==gfx_sample(stop,(int16_t)(x0+cw-20),(int16_t)(y0+ch-20)));
    CHECK(sim_framebuffer()[(y0+20)*LCD_WIDTH+x0+cw/2]==gfx_sample(stop,(int16_t)(x0+cw/2),(int16_t)(y0+20)));
    CHECK(sim_framebuffer()[(y0+ch-20)*LCD_WIDTH+x1+20]==gfx_sample(cell,(int16_t)(x1+20),(int16_t)(y0+ch-20)));
    CHECK(count_colour(x0,y0,cw,ch,C_VALUE)>50);                    /* the white label and PRESS */
    /* Status ink: green, then red after TONED_VALUES, and nothing left of the green. */
    CHECK(count_colour(x1,(int16_t)(y0+70),cw,ch-72,C_OK)>20);
    CHECK(count_colour(x1,(int16_t)(y0+70),cw,ch-72,C_DANGER)==0);
    fixture_toned_text(1,EMP_TONE_ALERT,"STOPPED");ui_render();
    CHECK(count_colour(x1,(int16_t)(y0+70),cw,ch-72,C_DANGER)>20);
    CHECK(count_colour(x1,(int16_t)(y0+70),cw,ch-72,C_OK)==0);
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    /* A tone-only change repaints too. */
    fixture_toned_text(1,EMP_TONE_WARN,"STOPPED");ui_render();
    CHECK(count_colour(x1,(int16_t)(y0+70),cw,ch-72,C_WARN)>20);
    CHECK(count_colour(x1,(int16_t)(y0+70),cw,ch-72,C_DANGER)==0);
    /* Pressing and releasing the stop puts back red, not grey, and matches a clean repaint. */
    ui_state_touch(1,100);ui_state_push(0,1,120);ui_render();
    CHECK(sim_framebuffer()[(y0+ch-20)*LCD_WIDTH+x0+20]==gfx_sample(stop,(int16_t)(x0+20),(int16_t)(y0+ch-20)));
    ui_state_push(0,0,140);ui_state_touch(0,160);ui_render();
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    CHECK(sim_framebuffer()[(y0+4)*LCD_WIDTH+x0+40]!=C_CELL);
    /* Drilled into a status readout, its large text carries the tone. */
    ui_state_push(4,1,200);ui_state_push(4,0,220);ui_render();
    CHECK(ui_state()->focused==4);
    CHECK(count_colour(0,0,LCD_WIDTH,190,C_DANGER)>20);
    puts("  estop plate is the stop card; status tones repaint and match clean repaints");
}
/* EMP/1.7: HOLD TO CLEAR is an ordinary cell, not a stop plate. Its fill grows amber while held,
 * turns green once sent, clears on release, and every step matches a clean repaint. */
static void hold_tests(void)
{
    const int16_t cw=(LCD_WIDTH-24-18)/4, ch=184, y0=6, x1=(int16_t)(12+cw+6);
    const int16_t fy=(int16_t)(y0+ch-12-16), fh=16;
    clean();fixture_status_hold();ui_state_tick(100);ui_render();
    CHECK(surf_field_is_hold(surf_field(1))&&ui_state_field_for(1)==1);
    CHECK(count_colour(x1,y0,cw,ch,C_STOP)==0);
    CHECK(count_colour(x1,fy,cw,fh,C_WARN)==0&&count_colour(x1,fy,cw,fh,C_OK)==0);
    ui_state_push(1,1,100);ui_state_tick(100+EMP_HOLD_MS/2);ui_render();
    unsigned half=count_colour(x1,fy,cw,fh,C_WARN);
    CHECK(half>200&&count_colour(x1,fy,cw,fh,C_OK)==0);
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    ui_state_tick(100+EMP_HOLD_MS*9/10);ui_render();
    CHECK(count_colour(x1,fy,cw,fh,C_WARN)>half);
    ui_state_tick(100+EMP_HOLD_MS);ui_render();
    CHECK(count_colour(x1,fy,cw,fh,C_WARN)==0&&count_colour(x1,fy,cw,fh,C_OK)>half);
    CHECK(count_colour(x1,(int16_t)(y0+70),cw,60,C_OK)>20);           /* DONE */
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    ui_state_push(1,0,2000);ui_state_tick(2000);ui_render();
    CHECK(count_colour(x1,fy,cw,fh,C_WARN)==0&&count_colour(x1,fy,cw,fh,C_OK)==0);
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    puts("  hold fill grows amber, turns green once sent, clears on release, matches clean repaints");
}
/* Read-only readouts used to carry the side fader with a centred pip, which read as a small
 * scrollbar beside a value that cannot scroll or turn. Only a ranged value has a fader now. */
static void readout_tests(void)
{
    const int16_t cw=(LCD_WIDTH-24-18)/4, ch=184, top=6, bottom=(int16_t)(6+184+6+88+6);
    clean();sim_wire_set_peer_minor(7);fixture_system_page_17(0);ui_render();
    for(unsigned k=1;k<8;k++) {
        const surf_field_t *f=surf_field((uint16_t)ui_state_field_for(k));
        int16_t x=(int16_t)(12+(k%4)*(cw+6)), y=k<4?top:bottom;
        int16_t bx=(int16_t)(x+cw-12-12), by=(int16_t)(y+70), bh=(int16_t)(ch-70-12);
        unsigned track=count_colour(bx,by,12,bh,C_TRACK);
        if(f->kind==EMP_KIND_READONLY) CHECK(track==0);             /* Fleet, Nodes, Watches, Dictation */
        if(f->kind==EMP_KIND_NUMBER) CHECK(track>100);              /* Volume, Brightness keep theirs */
    }
    /* Nothing of the old fader under READ ONLY either: the caption is whole. */
    sim_wire_set_peer_minor(EMP_VERSION_MINOR);
    puts("  read-only text and unbounded readouts have no fader; ranged values keep theirs");
}
/* EMP/1.8: the fleet panel and the System list. Every step matches a clean repaint, the stop
 * keeps its red plate, the clear's fill grows across its face, and a scroll repaints the list. */
static void panel_tests(void)
{
    const int16_t cw=(LCD_WIDTH-24-18)/4, y0=6, x0=12, x1=(int16_t)(12+cw+6), x3=(int16_t)(12+3*(cw+6));
    const int16_t fy=(int16_t)(y0+80), fh=(int16_t)(184-80-6);
    static const char *const list="Fleet\tE-STOPPED\nStop by\tcontrol surface\nReason\tcontrol surface E-STOP\n"
                                  "Stopped\t12 s ago\nNodes\t2/2\nWatches\t1 on duty";
    clean();fixture_system_page("E-STOPPED\ncontrol surface, 12 s ago",EMP_TONE_WARN,1,
                                "STOPPED\nclear E-stop first",EMP_TONE_WARN,0,list);
    ui_state_tick(100);ui_render();
    CHECK(count_colour(x0,fy,cw,fh,C_STOP)>2000);                /* the stop is still the stop */
    CHECK(count_colour(x1,fy,cw,fh,C_WARN)>40&&count_colour(x1,fy,cw,fh,C_WARN)<2000); /* edge only */
    CHECK(count_colour(x0,y0,2*cw+6,80,C_WARN)>40);               /* the lit E-STOPPED pill */
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    ui_state_push(1,1,100);ui_state_tick(100+EMP_HOLD_MS/2);ui_render();
    unsigned half=count_colour(x1,fy,cw,fh,C_WARN);
    CHECK(half>3000);
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    ui_state_tick(100+EMP_HOLD_MS);ui_render();
    CHECK(count_colour(x1,fy,cw,fh,C_OK)>half);
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    ui_state_push(1,0,2000);ui_state_tick(2000);ui_render();
    CHECK(count_colour(x1,fy,cw,fh,C_OK)==0);
    /* The status changes as a value: header and face repaint in place, as a clean repaint would. */
    fixture_toned_text(1,EMP_TONE_WARN,"E-STOPPED\nclearing...");ui_render();
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    /* Scrolling the list repaints it, and only it. */
    memcpy(reference,sim_framebuffer(),sizeof(reference));
    ui_state_rotate(3,UI_LIST_STEP*2,2100);ui_render();
    CHECK(ui_state_list_top(3)==2);
    unsigned changed=0;
    for(int y=0;y<LCD_HEIGHT;y++)for(int x=0;x<LCD_WIDTH;x++)
        if(sim_framebuffer()[y*LCD_WIDTH+x]!=reference[y*LCD_WIDTH+x]) {
            changed++; CHECK(x>=x3&&x<x3+cw&&y>=y0&&y<y0+184);
        }
    CHECK(changed>100);
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    /* Leaving the stop: the clear is disabled and has no fill to give, and the panel stays one. */
    fixture_system_page("DISARMED\nlasers dark",EMP_TONE_OK,0,"READY\narm checks met",EMP_TONE_OK,1,list);
    ui_state_tick(3000);ui_state_push(1,1,3000);ui_state_tick(3000+EMP_HOLD_MS);ui_render();
    CHECK(count_colour(x1,fy,cw,fh,C_WARN)==0&&count_colour(x1,fy,cw,fh,C_OK)==0);
    CHECK(count_colour(x0,y0,2*cw+6,80,C_OK)>40);                 /* the lit DISARMED pill */
    memcpy(reference,sim_framebuffer(),sizeof(reference));ui_reset();ui_render();
    CHECK(memcmp(reference,sim_framebuffer(),sizeof(reference))==0);
    puts("  fleet panel and list: stop plate kept, hold fill on the face, clean-repaint parity");
}
int main(void)
{
    primitive_tests();switch_tests();background_tests();incremental_tests();operator_tests();highlight_tests();eviction_tests();layout_tests();status_tests();hold_tests();readout_tests();panel_tests();
    printf("%u graphics checks passed\n",checks);return 0;
}
