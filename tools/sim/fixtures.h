/* Host-only descriptor fixtures, encoded independently of the firmware decoder. */
#ifndef SIM_FIXTURES_H
#define SIM_FIXTURES_H
#include "surface.h"
#include "frame.h"
#include "ui_state.h"
#include <string.h>
typedef struct { uint8_t bytes[512];unsigned n; } fixture_buf;
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
/* EMP/1.6 TONED_VALUES for one text field at the live revision. */
static void fixture_toned_text(uint16_t id,uint8_t tone,const char*text)
{
    fixture_buf b={{0},0};fixture_int(&b,surf_applied_revision(),8);fixture_int(&b,1,2);
    fixture_int(&b,id,2);fixture_int(&b,0,4);fixture_int(&b,tone,1);fixture_int(&b,EMP_VAL_TEXT,1);fixture_string(&b,text);
    surf_handle(EMP_OP_TONED_VALUES,b.bytes,b.n);
}
#endif
