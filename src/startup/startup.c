/* Reset path.
 *
 * Order is transcribed from the stock firmware's the stock firmware and matters at several points:
 *
 *   - the FPU must be on before any code the compiler may have vectorised runs
 *   - wait states must be raised before the clock does (handled inside bsp_clock_init)
 *   - .bss/.data must be settled before C++ static constructors run
 *   - SDRAM must be up before anything places data there, since our heap lives at 0x90000000
 *
 * We deliberately do NOT write SCB->VTOR. The stock application never does either — it is
 * the one thing the bootloader leaves configured for us, pointing at 0x00100000. Writing it
 * would work, but not writing it keeps us honest about the dependency.
 */

#include "s7g2.h"
#include "bsp.h"
#include "boot_handshake.h"

/* No-op unless the image overrides it. Only the application does: the bootloader must not
 * stamp over the crumbs it exists to read back. */
__attribute__((weak)) void boot_stage(uint32_t code) { (void)code; }
__attribute__((weak)) void boot_stage_force(uint32_t code) { (void)code; }

extern uint32_t __data_load, __data_start, __data_end;
extern uint32_t __bss_start, __bss_end;
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

extern int main(void);

static void copy_data(void)
{
    const uint32_t *src = &__data_load;
    uint32_t *dst = &__data_start;
    while (dst < &__data_end) *dst++ = *src++;
}

/* Code that must run from RAM rather than flash. The flash programming routines are the
 * reason this exists: while code flash is in program/erase mode it cannot serve instruction
 * fetches, so a routine that erases flash while executing from it locks the core. Copying
 * them here, before anything can call them, is what makes the bootloader's update path
 * possible at all. */
extern uint32_t __ramfunc_load, __ramfunc_start, __ramfunc_end;

static void copy_ramfunc(void)
{
    const uint32_t *src = &__ramfunc_load;
    uint32_t *dst = &__ramfunc_start;
    while (dst < &__ramfunc_end) *dst++ = *src++;
}

static void zero_bss(void)
{
    uint32_t *p = &__bss_start;
    while (p < &__bss_end) *p++ = 0;
}

static void run_init_array(void)
{
    size_t n = (size_t)(__init_array_end - __init_array_start);
    for (size_t i = 0; i < n; i++) __init_array_start[i]();
}

/* Renesas main-stack-pointer monitor. Not the ARM MPU — it watches the MSP against a
 * window and raises NMI on overrun. The stock firmware arms it over its 16 KB stack; we
 * arm it over ours, which the linker script provides. Cheap insurance on a device where a
 * stack overflow would otherwise present as arbitrary corruption. */
extern uint32_t __stack_bottom, __stack_top;

static void arm_stack_monitor(void)
{
    /* The monitor's address registers ignore their low bits, so an unaligned boundary
     * describes a window that is not the stack — and the monitor then faults on a perfectly
     * valid push. Round the start DOWN and the end UP so the programmed window always contains
     * the real stack: a window slightly larger than the stack is harmless, one slightly smaller
     * is a spurious NMI in the middle of normal operation.
     *
     * The linker script also aligns the section to 16. Both, because this failed once already
     * and the symptom — NMI in the main loop, on builds where .bss happened to reach a size
     * that left the stack 8-aligned — is a very long way from the cause. */
    uint32_t sa = (uint32_t)&__stack_bottom & ~0xFUL;
    uint32_t ea = (uint32_t)&__stack_top | 0xFUL;

    REG32(MSPMPUCTL) = 0;
    REG32(MSPMPUOAD) = 0xA500UL;                       /* key, NMI on detection */
    REG32(MSPMPUSA)  = sa;
    REG32(MSPMPUEA)  = ea;
    REG32(ICU_NMIER) = REG32(ICU_NMIER) | NMIER_SPEEN;
    REG32(MSPMPUCTL) = 1;
}

void Reset_Handler(void)
{
    /* 1. FPU on. CP10/CP11 full access. */
    REG32(CPACR) = REG32(CPACR) | 0x00F00000UL;
    __asm__ volatile("dsb");
    __asm__ volatile("isb");

    /* 2. Zero the handful of statics used before .bss is cleared. The bulk clear is
     *    deferred to step 5 because .bss is large and the core is still slow here. */
    prcr_reset();
    boot_stage(STAGE_RESET);

    /* 3. Clocks, including the wait states that must precede them. Nothing below this
     *    point is timing-correct until it has run. */
    if (bsp_clock_init() != 0) {
        boot_stage(0xC1);
        for (;;) { __asm__ volatile("wfi"); }
    }

    /* 4. Pins. Every peripheral needs its mux, and the display reset line is among them. */
    boot_stage(STAGE_CLOCKS);
    bsp_pins_init();

    /* 5. External SDRAM, before anything can place data in it. */
    boot_stage(STAGE_PINS);
    (void)bsp_sdram_init();

    /* 6. C runtime. */
    boot_stage(STAGE_SDRAM);
    zero_bss();
    copy_data();
    copy_ramfunc();

    /* 7. Stack overrun detection: NOT ARMED — see arm_stack_monitor() for the history.
     *
     * Kept as reachable-looking dead code rather than deleted, so the register sequence and the
     * reasoning survive for whoever revisits this with a datasheet in hand. */
    boot_stage(STAGE_CRT);
    if (0) arm_stack_monitor();

    /* 8. Interrupt routing. Must happen before any driver enables an NVIC slot. */
    boot_stage(STAGE_STACKMON);
    bsp_icu_init();

    /* 8b. Only now is taking an interrupt meaningful, so only now do we unmask. The handoff
     *     from the bootloader deliberately arrives with interrupts masked. */
    REG32(SCB_ICSR) = ICSR_PENDSTCLR | ICSR_PENDSVCLR;
    __asm__ volatile("cpsie i" ::: "memory");

    /* 9. Static constructors, after .data is populated. */
    boot_stage(STAGE_ICU);
    run_init_array();

    boot_stage(STAGE_PREMAIN);
    (void)main();

    for (;;) { __asm__ volatile("wfi"); }
}
