/* Bare-metal D1 port for the pinned Renesas TES D2 source. Direct-register mode, no RTOS.
 * Clock gate and IRQ3/0x1FD come from this board's existing BSP; register layout follows
 * the Renesas DRW D1 port. The hardware revision is checked before rendering. */
#include "dave_base.h"
#include "s7g2.h"
#include "bsp.h"
#include "graphics.h"
#include "graphics_backend.h"
#include "diag.h"
#include <string.h>
#include <stddef.h>
#define DRW_BASE 0x400E4000UL
#define DRW_STATUS 0u
#define DRW_IRQCTL 48u
#define ARENA_BYTES (128u*1024u)
static uint8_t arena[ARENA_BYTES] __attribute__((section(".graphics_sdram"),aligned(8)));
static unsigned arena_used;
static uint8_t opened,failed;
static uint32_t wait_start;
uint32_t electra_dave_fault_code,electra_dave_fault_context,electra_dave_revision;
void electra_dave_wait(d1_device *h,unsigned int mask);
int electra_dave_failed(void) { return failed; }
static void fault(uint16_t code,uint32_t context,const char *message)
{
    if(!failed) { gfx_accel_errors++; emp_diag(EMP_SEV_ERROR,code,context,message); }
    electra_dave_fault_code=code;electra_dave_fault_context=context;failed=1;
}
d1_device *d1_opendevice(long flags)
{
    (void)flags;prcr_unlock(1);REG32(MSTPCRC)&=~MSTPC_DRW;prcr_lock(1);
    REG32(DRW_BASE+DRW_IRQCTL*4)=0x2c; /* Clear all IRQs; polling direct mode owns completion. */
    opened=1;failed=0;electra_dave_revision=REG32(DRW_BASE+4);
    if(!electra_dave_revision||electra_dave_revision==0xffffffffu)fault(EMP_DIAG_GRAPHICS_FAULT,electra_dave_revision,"D/AVE revision invalid");return (d1_device *)&opened;
}
int d1_closedevice(d1_device *h) { (void)h;opened=0;return 1; }
void d1_setregister(d1_device *h,int deviceid,int index,long value)
{
    (void)h;if(failed||deviceid!=D1_DAVE2D||index<0||index>=128) return;
    __asm volatile("dmb" ::: "memory");REG32(DRW_BASE+(unsigned)index*4)=(uint32_t)value;
}
long d1_getregister(d1_device *h,int deviceid,int index)
{
    (void)h;if(failed||deviceid!=D1_DAVE2D||index<0||index>=128)return 0;
    return (long)REG32(DRW_BASE+(unsigned)index*4);
}
int d1_devicesupported(d1_device *h,int id) { (void)h;return id==D1_DAVE2D; }
int d1_queryarchitecture(d1_device *h) { (void)h;return d1_ma_unified; }
int d1_queryirq(d1_device *h,int mask,int timeout) { (void)mask;(void)timeout;electra_dave_wait(h,11u);return !failed; }
void electra_dave_wait(d1_device *h,unsigned int mask)
{
    (void)h;if(failed)return;wait_start=gfx_clock_cycles();
    for(unsigned polls=0;polls<1000000;polls++) {
        uint32_t status=REG32(DRW_BASE);
        if(status&(1u<<6)) { fault(EMP_DIAG_GRAPHICS_FAULT,status,"D/AVE bus error");break; }
        if(!(status&mask))break;
        if((uint32_t)(gfx_clock_cycles()-wait_start)>4800000u||polls==999999) {
            fault(EMP_DIAG_GRAPHICS_TIMEOUT,status,"D/AVE completion timeout");break;
        }
        if(!(polls&255))gfx_yield();
    }
    gfx_gpu_cycles+=(uint32_t)(gfx_clock_cycles()-wait_start);
    __asm volatile("dmb" ::: "memory");
}
void *d1_allocmem(unsigned n)
{
    if(n>ARENA_BYTES-8u||arena_used>ARENA_BYTES-8u)return NULL;
    n=(n+7u)&~7u;if(n>ARENA_BYTES-arena_used-8u)return NULL;
    unsigned *p=(unsigned *)(arena+arena_used);p[0]=n;arena_used+=n+8;return p+2;
}
void d1_freemem(void *p) { (void)p; /* Bounded lifetime arena; one driver instance per boot. */ }
unsigned d1_memsize(void *p) { return p?((unsigned *)p)[-2]:0; }
void *d1_allocvidmem(d1_device*h,int type,unsigned n){(void)h;(void)type;return d1_allocmem(n);}
void d1_freevidmem(d1_device*h,int type,void*p){(void)h;(void)type;d1_freemem(p);}
int d1_queryvidmem(d1_device*h,int type,int query){(void)h;(void)type;(void)query;return 0;}
void *d1_mapvidmem(d1_device*h,void*p,int flags){(void)h;(void)flags;return p;}
int d1_unmapvidmem(d1_device*h,void*p){(void)h;(void)p;return 1;}
void *d1_maptovidmem(d1_device*h,void*p){(void)h;return p;}
void *d1_mapfromvidmem(d1_device*h,void*p){(void)h;return p;}
int d1_copytovidmem(d1_device*h,void*dst,const void*src,unsigned n,int flags){(void)h;(void)flags;memcpy(dst,src,n);return 1;}
int d1_copyfromvidmem(d1_device*h,void*dst,const void*src,unsigned n,int flags){return d1_copytovidmem(h,dst,src,n,flags);}
int d1_cacheflush(d1_device*h,int type){(void)h;(void)type;__asm volatile("dmb" ::: "memory");return 1;}
int d1_cacheblockflush(d1_device*h,int type,const void*p,unsigned n){(void)p;(void)n;return d1_cacheflush(h,type);}
