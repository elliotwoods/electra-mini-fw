/* Cortex-M vector table.
 *
 * This must be the first thing at 0x00100000: the bootloader hands off via the SP/PC pair
 * that lives there. There is no header, magic, CRC or signature ahead of it — confirmed
 * byte-for-byte against the stock image.
 *
 * The stock firmware populates 41 entries (16 system + 25 IRQ) and leaves the rest erased.
 * We match that shape so the IRQ-to-IELSR mapping in icu.c lines up.
 */

#include <stdint.h>
#include "s7g2.h"
#include "boot_handshake.h"

extern uint32_t __stack_top;
void Reset_Handler(void);

/* Every handler is weak and aliased to a default, so a driver can claim one just by
 * defining a function with the matching name. */
void Default_Handler(void);
void fault_to_bootloader(uint32_t ipsr);
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));

/* SysTick and PendSV are NOT aliased to the spinning default, and that is the whole point.
 *
 * The stock bootloader runs ThreadX, so a SysTick can already be latched pending before any
 * of our code exists. Our bootloader inherits interrupts masked and so never sees it — but
 * the handoff to the application sets VTOR to the application's table and then unmasks, and
 * the tick lands here before the application's reset handler has run a single instruction.
 * Every application build hung identically at that point, which is exactly why none of the
 * changes we made inside the application ever mattered.
 *
 * A stray tick is not a fault. Absorb it, stop the timer so it cannot come back, and count
 * it so the console can confirm this really happened instead of us assuming it did. */

/* An NMI that keeps re-firing starves everything else, and the device drops off USB just as if
 * it had spun. Absorb a bounded number — enough for a one-off — then take the same route back
 * to the bootloader, where it can be diagnosed and replaced. */
__attribute__((weak)) void NMI_Handler(void)
{
    static uint32_t nmi_seen;
    if (++nmi_seen > 64u) fault_to_bootloader(2u);
}

__attribute__((weak)) void SysTick_Handler(void)
{
    REG32(SYST_CSR) = 0;

    volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
    if (boot_handshake_valid()) {
        h->detail   = h->detail + 1U;
        h->checksum = boot_handshake_checksum(h);
    }
}

__attribute__((weak)) void PendSV_Handler(void) { }

#define IRQ(n) void IRQ_##n##_Handler(void) __attribute__((weak, alias("Default_Handler")));
IRQ(00) IRQ(01) IRQ(02) IRQ(03) IRQ(04) IRQ(05) IRQ(06) IRQ(07)
IRQ(08) IRQ(09) IRQ(10) IRQ(11) IRQ(12) IRQ(13) IRQ(14) IRQ(15)
IRQ(16) IRQ(17) IRQ(18) IRQ(19) IRQ(20) IRQ(21) IRQ(22) IRQ(23)
IRQ(24)
#undef IRQ

typedef void (*vector_t)(void);

__attribute__((section(".vectors"), used))
const vector_t g_vectors[] = {
    (vector_t)&__stack_top,   /*  0  initial MSP */
    Reset_Handler,            /*  1  */
    NMI_Handler,              /*  2  */
    HardFault_Handler,        /*  3  */
    MemManage_Handler,        /*  4  */
    BusFault_Handler,         /*  5  */
    UsageFault_Handler,       /*  6  */
    0, 0, 0, 0,               /*  7-10 reserved */
    SVC_Handler,              /* 11  */
    DebugMon_Handler,         /* 12  */
    0,                        /* 13  reserved */
    PendSV_Handler,           /* 14  */
    SysTick_Handler,          /* 15  */

    /* IRQ 0..24 — see icu.c for what each is wired to. The mapping is the ICU's, not
     * fixed by silicon, so these two files must be kept in step. */
    IRQ_00_Handler, IRQ_01_Handler, IRQ_02_Handler, IRQ_03_Handler,
    IRQ_04_Handler, IRQ_05_Handler, IRQ_06_Handler, IRQ_07_Handler,
    IRQ_08_Handler, IRQ_09_Handler, IRQ_10_Handler, IRQ_11_Handler,
    IRQ_12_Handler, IRQ_13_Handler, IRQ_14_Handler, IRQ_15_Handler,
    IRQ_16_Handler, IRQ_17_Handler, IRQ_18_Handler, IRQ_19_Handler,
    IRQ_20_Handler, IRQ_21_Handler, IRQ_22_Handler, IRQ_23_Handler,
    IRQ_24_Handler,
};

/* A fault reboots into the bootloader instead of spinning.
 *
 * Spinning was the original choice, on the grounds that a reset loop looks like a boot failure.
 * That reasoning was wrong in an important way: a spinning device DISAPPEARS FROM USB, so it
 * costs a person walking over and unplugging the cable before anything can be learned or fixed.
 * Three of those in one afternoon is three too many.
 *
 * Rebooting with BOOT_REQ_UPDATE lands us in the bootloader, which holds in flash mode — so a
 * crashed application comes back by itself, ready to be replaced, with the breadcrumb intact
 * because a warm reset preserves SRAM. There is no reset loop to fear: the bootloader does not
 * relaunch a failing application, it waits.
 *
 * The crumb is written FIRST. If anything below misbehaves, the diagnosis still survives. */
void fault_to_bootloader(uint32_t ipsr)
{
    boot_stage_force(STAGE_FAULT(ipsr, BOOT_HANDSHAKE.app_faults));

    boot_handshake_set_request(BOOT_REQ_UPDATE, 0);

    __asm__ volatile("dsb" ::: "memory");
    REG32(0xE000ED0CUL) = (REG32(0xE000ED0CUL) & 0x0700UL) | 0x05FA0004UL;   /* SYSRESETREQ */
    __asm__ volatile("dsb" ::: "memory");

    for (;;) { __asm__ volatile("wfi"); }
}

void Default_Handler(void)
{
    uint32_t ipsr;
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    fault_to_bootloader(ipsr);
}
