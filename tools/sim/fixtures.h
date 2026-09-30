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
#endif
