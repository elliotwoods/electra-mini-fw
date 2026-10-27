/* Host-only descriptor fixtures, encoded independently of the firmware decoder. */
#ifndef SIM_FIXTURES_H
#define SIM_FIXTURES_H
#include "surface.h"
#include "frame.h"
#include "ui_state.h"
#include <string.h>
typedef struct { uint8_t bytes[1024];unsigned n; } fixture_buf;
static void fixture_int(fixture_buf*b,uint64_t v,unsigned bytes){while(bytes--){b->bytes[b->n++]=(uint8_t)v;v>>=8;}}
static void fixture_string(fixture_buf*b,const char*s){unsigned n=(unsigned)strlen(s);fixture_int(b,n,2);memcpy(b->bytes+b->n,s,n);b->n+=n;}
static void fixture_double(fixture_buf*b,double v){memcpy(b->bytes+b->n,&v,8);b->n+=8;}
static void fixture_sparse(unsigned count,int holes)
{
    static uint64_t revision=0x100000000ULL;revision++;
    fixture_buf b={{0},0};fixture_int(&b,revision,8);fixture_int(&b,count,2);fixture_int(&b,0,2);fixture_int(&b,0,4);
    surf_handle(EMP_OP_DESC_BEGIN,b.bytes,b.n);uint32_t crc=0xffffffff;
    for(unsigned i=0;i<count;i++) {
        b.n=0;fixture_int(&b,i,2);unsigned body=b.n;
        fixture_int(&b,0,2);fixture_int(&b,holes?i*3:i,2);fixture_int(&b,i==count-1?EMP_KIND_READONLY:EMP_KIND_NUMBER,1);fixture_int(&b,0,1);
        fixture_int(&b,EMP_PRESENT_MIN|EMP_PRESENT_MAX|EMP_PRESENT_PRECISION,2);
        fixture_double(&b,-10000);fixture_double(&b,10000);fixture_double(&b,.01);
        fixture_int(&b,2,1);fixture_int(&b,0,1);fixture_int(&b,0,2);fixture_int(&b,EMP_VAL_NUMBER,1);fixture_double(&b,-9999.25+i);
        fixture_string(&b,i==0?"VeryLongUnbrokenIdentifyingLabel_WithAdditionalUnbrokenSegmentsAndAnIdentifyingSuffix_Channel_24":"Negative Readout");
        crc=emp_crc32c_update(crc,b.bytes+body,b.n-body);surf_handle(EMP_OP_DESC_FIELD,b.bytes,b.n);
    }
    b.n=0;fixture_int(&b,revision,8);fixture_int(&b,count,2);fixture_int(&b,~crc,4);surf_handle(EMP_OP_DESC_END,b.bytes,b.n);ui_state_init();
}
static void fixture_toggles(void)
{
    static uint64_t revision=0x700000000ULL;revision++;
    static const char *const labels[8]={"Bypass","Mute","Gate","Monitor","Enable","Record","Solo","Output"};
    fixture_buf b={{0},0};fixture_int(&b,revision,8);fixture_int(&b,8,2);fixture_int(&b,0,2);fixture_int(&b,0,4);
    surf_handle(EMP_OP_DESC_BEGIN,b.bytes,b.n);uint32_t crc=0xffffffff;
    for(unsigned i=0;i<8;i++) {
        b.n=0;fixture_int(&b,i,2);unsigned body=b.n;
        fixture_int(&b,0,2);fixture_int(&b,i,2);fixture_int(&b,EMP_KIND_TOGGLE,1);fixture_int(&b,0,1);fixture_int(&b,0,2);
        fixture_double(&b,0);fixture_double(&b,1);fixture_double(&b,1);
        fixture_int(&b,0,1);fixture_int(&b,0,1);fixture_int(&b,0,2);fixture_int(&b,EMP_VAL_BOOL,1);fixture_int(&b,i%2,1);
        fixture_string(&b,labels[i]);crc=emp_crc32c_update(crc,b.bytes+body,b.n-body);surf_handle(EMP_OP_DESC_FIELD,b.bytes,b.n);
    }
    b.n=0;fixture_int(&b,revision,8);fixture_int(&b,8,2);fixture_int(&b,~crc,4);surf_handle(EMP_OP_DESC_END,b.bytes,b.n);ui_state_init();ui_reset();
}
/* EMP/1.6 status page, as the av-fleet daemon sends it: a stop on the top-left knob (six fields,
 * so the generic grid keeps reading order), a toned status readout beside it. Records carry the
 * trailing style + tone members after the label. */
typedef struct { uint8_t kind,style,tone; const char*label,*text; } fixture_styled_field;
static void fixture_styled(const fixture_styled_field*f,unsigned n)
{
    static uint64_t revision=0x1600000000ULL;revision++;
    fixture_buf b={{0},0};fixture_int(&b,revision,8);fixture_int(&b,n,2);fixture_int(&b,0,2);fixture_int(&b,0,4);
    surf_handle(EMP_OP_DESC_BEGIN,b.bytes,b.n);uint32_t crc=0xffffffff;
    for(unsigned i=0;i<n;i++) {
        b.n=0;fixture_int(&b,i,2);unsigned body=b.n;
        fixture_int(&b,0,2);fixture_int(&b,i,2);fixture_int(&b,f[i].kind,1);fixture_int(&b,0,1);fixture_int(&b,EMP_PRESENT_STYLE,2);
        fixture_double(&b,0);fixture_double(&b,0);fixture_double(&b,0);fixture_int(&b,0,1);fixture_int(&b,0,1);fixture_int(&b,0,2);
        if(f[i].text){fixture_int(&b,EMP_VAL_TEXT,1);fixture_string(&b,f[i].text);} else {fixture_int(&b,EMP_VAL_NUMBER,1);fixture_double(&b,0);}
        fixture_string(&b,f[i].label);fixture_int(&b,f[i].style,1);fixture_int(&b,f[i].tone,1);
        crc=emp_crc32c_update(crc,b.bytes+body,b.n-body);surf_handle(EMP_OP_DESC_FIELD,b.bytes,b.n);
    }
    b.n=0;fixture_int(&b,revision,8);fixture_int(&b,n,2);fixture_int(&b,~crc,4);surf_handle(EMP_OP_DESC_END,b.bytes,b.n);ui_state_init();ui_reset();
}
static void fixture_status(uint8_t status_tone,const char*status)
{
    fixture_styled_field f[6]={
        {EMP_KIND_ACTION,EMP_STYLE_ESTOP,0,"E-STOP",0},{EMP_KIND_READONLY,0,0,"Orchestrator",0},
        {EMP_KIND_READONLY,0,EMP_TONE_OK,"Nodes","2/2 connected"},{EMP_KIND_READONLY,0,EMP_TONE_WARN,"Beacon","late"},
        {EMP_KIND_READONLY,0,EMP_TONE_ALERT,"Last stop","control surface"},{EMP_KIND_ACTION,0,0,"Identify",0}};
    f[1].tone=status_tone;f[1].text=status;
    fixture_styled(f,6);
}
/* EMP/1.7: the same page while STOPPED, as the host sends it: ID 1 becomes a HOLD Action. */
static void fixture_status_hold(void)
{
    static const fixture_styled_field f[6]={
        {EMP_KIND_ACTION,EMP_STYLE_ESTOP,0,"E-STOP",0},{EMP_KIND_ACTION,EMP_STYLE_HOLD,0,"HOLD TO CLEAR",0},
        {EMP_KIND_READONLY,0,EMP_TONE_ALERT,"Orchestrator","STOPPED"},{EMP_KIND_READONLY,0,EMP_TONE_WARN,"Beacon","late"},
        {EMP_KIND_READONLY,0,EMP_TONE_ALERT,"Last stop","control surface"},{EMP_KIND_ACTION,0,0,"Identify",0}};
    fixture_styled(f,6);
}
/* EMP/1.8: any field, flags and choices included -- enough to build the av-frameworks daemon's
 * system page exactly as its router sends it. `text` set means a Text value; otherwise Bool for
 * Actions and toggles, Number for the rest. */
typedef struct {
    uint8_t kind,flags,style,tone; const char*label,*text,*unit; double number,min,max;
    const char*const*choices; uint16_t choice_count;
} fixture_field;
static void fixture_fields(const fixture_field*f,unsigned n)
{
    static uint64_t revision=0x1800000000ULL;revision++;
    unsigned count=0; for(unsigned i=0;i<n;i++) if(f[i].label) count++;
    fixture_buf b={{0},0};fixture_int(&b,revision,8);fixture_int(&b,count,2);fixture_int(&b,0,2);fixture_int(&b,0,4);
    surf_handle(EMP_OP_DESC_BEGIN,b.bytes,b.n);uint32_t crc=0xffffffff;
    unsigned index=0;
    for(unsigned i=0;i<n;i++) {
        const fixture_field*x=&f[i];
        if(!x->label) continue;
        b.n=0;fixture_int(&b,index++,2);unsigned body=b.n;
        unsigned present=(x->style||x->tone)?EMP_PRESENT_STYLE:0u;
        if(x->unit) present|=EMP_PRESENT_UNIT;
        if(x->max>x->min) present|=EMP_PRESENT_MIN|EMP_PRESENT_MAX|EMP_PRESENT_STEP|EMP_PRESENT_PRECISION;
        if(x->choice_count) present|=EMP_PRESENT_CHOICES;
        fixture_int(&b,0,2);fixture_int(&b,i,2);fixture_int(&b,x->kind,1);
        fixture_int(&b,(x->kind==EMP_KIND_READONLY?EMP_FIELD_READONLY:0u)|x->flags,1);fixture_int(&b,present,2);
        fixture_double(&b,x->min);fixture_double(&b,x->max);fixture_double(&b,1);
        fixture_int(&b,0,1);fixture_int(&b,0,1);fixture_int(&b,x->choice_count,2);
        if(x->text){fixture_int(&b,EMP_VAL_TEXT,1);fixture_string(&b,x->text);}
        else if(x->kind==EMP_KIND_ACTION||x->kind==EMP_KIND_TOGGLE){fixture_int(&b,EMP_VAL_BOOL,1);fixture_int(&b,x->number!=0,1);}
        else {fixture_int(&b,EMP_VAL_NUMBER,1);fixture_double(&b,x->number);}
        fixture_string(&b,x->label);
        if(x->unit) fixture_string(&b,x->unit);
        for(unsigned c=0;c<x->choice_count;c++) fixture_string(&b,x->choices[c]);
        if(present&EMP_PRESENT_STYLE){fixture_int(&b,x->style,1);fixture_int(&b,x->tone,1);}
        crc=emp_crc32c_update(crc,b.bytes+body,b.n-body);surf_handle(EMP_OP_DESC_FIELD,b.bytes,b.n);
    }
    b.n=0;fixture_int(&b,revision,8);fixture_int(&b,count,2);fixture_int(&b,~crc,4);surf_handle(EMP_OP_DESC_END,b.bytes,b.n);ui_state_init();ui_reset();
}
/* The daemon's system page on an EMP/1.8 device: the fleet panel (E-STOP + CLEAR E-STOP), ARM,
 * the System readout, then Volume, Brightness, Dictate and Dictation. `stopped` enables the
 * clear; `armable` the arm. Strings are what control-surface-router's system page sends. */
static const char*const fixture_fleet_states[3]={"DISARMED","ARMED","E-STOPPED"};
/* EMP/1.9: `detail` set adds the fleet's detail page, as the daemon's detail.rs composes it: the
 * E-STOP panel PINNED at IDs 0..1, System OPENS, and six LIST cells at IDs 10..15 (8p+2+c, p=1)
 * titled Checks, Lease, Nodes, Boards, Stops, Orch. `opens` alone marks System OPENS. */
static const char*const fixture_detail_titles[6]={"Checks","Lease","Nodes","Boards","Stops","Orch"};
static void fixture_system_page_ex(const char*fleet,uint8_t fleet_tone,int stopped,
                                   const char*arm,uint8_t arm_tone,int armable,const char*readout,
                                   int opens,const char*const*detail)
{
    uint8_t pin=detail?EMP_FIELD_PINNED:0u;
    fixture_field f[16]={
        {EMP_KIND_ACTION,pin,EMP_STYLE_ESTOP,0,"E-STOP",0,0,0,0,0,0,0},
        {EMP_KIND_ACTION,(uint8_t)(pin|EMP_FIELD_JOINED|(stopped?0u:EMP_FIELD_DISABLED)),EMP_STYLE_HOLD,fleet_tone,
         "CLEAR E-STOP",fleet,0,0,0,0,fixture_fleet_states,3},
        {EMP_KIND_ACTION,(uint8_t)(armable?0u:EMP_FIELD_DISABLED),EMP_STYLE_HOLD,arm_tone,"ARM",arm,0,0,0,0,0,0},
        {EMP_KIND_READONLY,(uint8_t)(opens||detail?EMP_FIELD_OPENS:0u),EMP_STYLE_LIST,0,"System",readout,0,0,0,0,0,0},
        {EMP_KIND_NUMBER,0,0,0,"Volume",0,"%",40,0,100,0,0},
        {EMP_KIND_NUMBER,0,0,0,"Brightness",0,"%",75,0,100,0,0},
        {EMP_KIND_ACTION,0,0,0,"Dictate",0,0,0,0,0,0,0},
        {EMP_KIND_READONLY,0,0,0,"Dictation","Ready",0,0,0,0,0,0}};
    if(detail) for(unsigned c=0;c<6;c++)
        f[10+c]=(fixture_field){EMP_KIND_READONLY,0,EMP_STYLE_LIST,0,fixture_detail_titles[c],detail[c],0,0,0,0,0,0};
    fixture_fields(f,detail?16u:8u);
}
static void fixture_system_page(const char*fleet,uint8_t fleet_tone,int stopped,
                                const char*arm,uint8_t arm_tone,int armable,const char*readout)
{
    fixture_system_page_ex(fleet,fleet_tone,stopped,arm,arm_tone,armable,readout,0,0);
}
/* An application's page with the fleet's two slots reserved, as the daemon composes it on an
 * EMP/1.8 device while an orchestrator is known: E-STOP and CLEAR E-STOP PINNED at IDs 0 and 1,
 * the application's controls six to a page at IDs 2..7, 10..15 (8 and 9 stay empty). */
static void fixture_reserved_app_page(const char*fleet,uint8_t fleet_tone,int stopped)
{
    static const char*const labels[12]={"Cutoff","Resonance","Drive","Mix","Attack","Release",
                                        "Rate","Depth","Offset X","Offset Y","Bypass","Gain"};
    fixture_field f[16];
    memset(f,0,sizeof(f));
    f[0]=(fixture_field){EMP_KIND_ACTION,EMP_FIELD_PINNED,EMP_STYLE_ESTOP,0,"E-STOP",0,0,0,0,0,0,0};
    f[1]=(fixture_field){EMP_KIND_ACTION,(uint8_t)(EMP_FIELD_PINNED|EMP_FIELD_JOINED|(stopped?0u:EMP_FIELD_DISABLED)),
                         EMP_STYLE_HOLD,fleet_tone,"CLEAR E-STOP",fleet,0,0,0,0,fixture_fleet_states,3};
    for(unsigned i=0;i<12;i++) {
        unsigned id=i<6?2+i:10+(i-6);
        f[id]=(fixture_field){i==10?EMP_KIND_TOGGLE:EMP_KIND_NUMBER,0,0,0,labels[i],0,i<4?"%":0,
                              10.0+7.0*i,0,i==10?1:100,0,0};
    }
    fixture_fields(f,16);
}
/* The fleet's top row as av-frameworks' daemon sends it since av.daemon-extension minor 1
 * (control-surface-router/src/system/top.rs), every field PINNED: E-STOP; the state button (ARM or
 * CLEAR E-STOP a HOLD, DISARM a plain Action; DISABLED when not offered; its Text value the
 * status); POWER, a 0-100 % number; Fleet, a LIST that OPENS. `power` < 0 leaves ID 2 empty. */
typedef struct {
    const char*label; uint8_t style,tone; int enabled; const char*status;
} fixture_state_button;
static void fixture_top_row(fixture_field*f,const fixture_state_button*button,double power,
                            const char*fleet)
{
    f[0]=(fixture_field){EMP_KIND_ACTION,EMP_FIELD_PINNED,EMP_STYLE_ESTOP,0,"E-STOP",0,0,0,0,0,0,0};
    f[1]=(fixture_field){EMP_KIND_ACTION,(uint8_t)(EMP_FIELD_PINNED|(button->enabled?0u:EMP_FIELD_DISABLED)),
                         button->style,button->tone,button->label,button->status,0,0,0,0,0,0};
    if(power>=0)
        f[2]=(fixture_field){EMP_KIND_NUMBER,EMP_FIELD_PINNED,0,0,"POWER",0,"%",power,0,100,0,0};
    f[3]=(fixture_field){EMP_KIND_READONLY,(uint8_t)(EMP_FIELD_PINNED|EMP_FIELD_OPENS),EMP_STYLE_LIST,0,
                         "Fleet",fleet,0,0,0,0,0,0};
}
/* The daemon's system page with that top row; the bottom row as ever. */
static void fixture_top_page(const fixture_state_button*button,double power,const char*fleet)
{
    fixture_field f[8];
    memset(f,0,sizeof(f));
    fixture_top_row(f,button,power,fleet);
    f[4]=(fixture_field){EMP_KIND_NUMBER,0,0,0,"Volume",0,"%",40,0,100,0,0};
    f[5]=(fixture_field){EMP_KIND_NUMBER,0,0,0,"Brightness",0,"%",75,0,100,0,0};
    f[6]=(fixture_field){EMP_KIND_ACTION,0,0,0,"Dictate",0,0,0,0,0,0,0};
    f[7]=(fixture_field){EMP_KIND_READONLY,0,0,0,"Dictation","Ready",0,0,0,0,0,0};
    fixture_fields(f,8);
}
/* EMP/1.10: the daemon's system page since 2026-10-27: that top row, Volume and Brightness (IDs 6
 * and 7 empty), and previous / play-pause / next at IDs 8-10, bound to buttons 2-4. */
static void fixture_transport_page(const fixture_state_button*button,double power,const char*fleet)
{
    fixture_field f[11];
    memset(f,0,sizeof(f));
    fixture_top_row(f,button,power,fleet);
    f[4]=(fixture_field){EMP_KIND_NUMBER,0,0,0,"Volume",0,"%",40,0,100,0,0};
    f[5]=(fixture_field){EMP_KIND_NUMBER,0,0,0,"Brightness",0,"%",75,0,100,0,0};
    f[8]=(fixture_field){EMP_KIND_ACTION,0,EMP_STYLE_TRACK_PREV,0,"Previous",0,0,0,0,0,0,0};
    f[9]=(fixture_field){EMP_KIND_ACTION,0,EMP_STYLE_PLAY_PAUSE,0,"Play",0,0,0,0,0,0,0};
    f[10]=(fixture_field){EMP_KIND_ACTION,0,EMP_STYLE_TRACK_NEXT,0,"Next",0,0,0,0,0,0,0};
    fixture_fields(f,11);
}
/* An application's page with the top row reserved while the fleet plugin is active: the
 * application's controls four to a page in the bottom row, IDs 4..7, 12..15 (8..11 stay empty). */
static void fixture_top_app_page(const fixture_state_button*button,double power,const char*fleet)
{
    static const char*const labels[8]={"Cutoff","Resonance","Drive","Mix","Attack","Release",
                                       "Rate","Bypass"};
    fixture_field f[16];
    memset(f,0,sizeof(f));
    fixture_top_row(f,button,power,fleet);
    for(unsigned i=0;i<8;i++) {
        unsigned id=i<4?4+i:12+(i-4);
        f[id]=(fixture_field){i==7?EMP_KIND_TOGGLE:EMP_KIND_NUMBER,0,0,0,labels[i],0,i<4?"%":0,
                              10.0+7.0*i,0,i==7?1:100,0,0};
    }
    fixture_fields(f,16);
}
/* The same page as an EMP/1.7 device is sent it today (the fallback below minor 8). */
static void fixture_system_page_17(int stopped)
{
    fixture_field f[8]={
        {EMP_KIND_ACTION,0,EMP_STYLE_ESTOP,0,"E-STOP",0,0,0,0,0,0,0},
        stopped ? (fixture_field){EMP_KIND_ACTION,0,EMP_STYLE_HOLD,EMP_TONE_WARN,"CLEAR E-STOP",0,0,0,0,0,0,0}
                : (fixture_field){EMP_KIND_READONLY,0,0,EMP_TONE_OK,"Fleet","DISARMED",0,0,0,0,0,0},
        {EMP_KIND_READONLY,0,0,EMP_TONE_OK,"Nodes","2/2",0,0,0,0,0,0},
        {EMP_KIND_READONLY,0,0,EMP_TONE_OK,"Watches","1 on duty",0,0,0,0,0,0},
        {EMP_KIND_NUMBER,0,0,0,"Volume",0,"%",40,0,100,0,0},
        {EMP_KIND_NUMBER,0,0,0,"Brightness",0,"%",75,0,100,0,0},
        {EMP_KIND_ACTION,0,0,0,"Dictate",0,0,0,0,0,0,0},
        {EMP_KIND_READONLY,0,0,0,"Dictation","Ready",0,0,0,0,0,0}};
    fixture_fields(f,8);
}
/* EMP/1.6 TONED_VALUES for one text field at the live revision. */
static void fixture_toned_text(uint16_t id,uint8_t tone,const char*text)
{
    fixture_buf b={{0},0};fixture_int(&b,surf_applied_revision(),8);fixture_int(&b,1,2);
    fixture_int(&b,id,2);fixture_int(&b,0,4);fixture_int(&b,tone,1);fixture_int(&b,EMP_VAL_TEXT,1);fixture_string(&b,text);
    surf_handle(EMP_OP_TONED_VALUES,b.bytes,b.n);
}
#endif
