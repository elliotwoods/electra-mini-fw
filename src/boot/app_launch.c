/* Validating and launching the application image.
 *
 * The bootloader must never hand control to something that is not a plausible Cortex-M
 * image, because a jump to erased flash (0xFFFFFFFF) faults immediately and the resulting
 * hang is indistinguishable from a dead device. Checking two words costs nothing and turns
 * "bricked application" into "boots into the bootloader and waits for a new one".
 */

#include "s7g2.h"
#include "bsp.h"
#include "app_launch.h"
#include "usb_fs.h"

/* Must match app.ld. */
#define APP_BASE   0x00120000UL
#define APP_LIMIT  0x00400000UL

/* SRAM window a valid initial stack pointer can live in. */
#define SRAM_LO    0x1FFE0000UL
#define SRAM_HI    0x20080000UL

int app_image_valid(void)
{
    uint32_t sp = REG32(APP_BASE);
    uint32_t pc = REG32(APP_BASE + 4);

    /* Erased flash reads 0xFFFFFFFF; a never-programmed region fails on both counts. */
    if (sp == 0xFFFFFFFFUL || sp == 0x00000000UL) return 0;
    if (pc == 0xFFFFFFFFUL || pc == 0x00000000UL) return 0;

    if (sp <= SRAM_LO || sp > SRAM_HI) return 0;     /* stack must point into SRAM */
    if ((pc & 1U) == 0) return 0;                    /* Thumb bit must be set */

    uint32_t entry = pc & ~1UL;
    if (entry < APP_BASE || entry >= APP_LIMIT) return 0;

    return 1;
}

/* Hand over. Everything the bootloader configured that could fire an interrupt into the
 * application's vector table has to be quiet first — an ICU event still linked to a
 * peripheral will happily deliver into whatever the application has at that slot before the
 * application has finished starting. */
void app_launch(void)
{
    uint32_t sp = REG32(APP_BASE);
    uint32_t pc = REG32(APP_BASE + 4);

    /* Leave the bus before we go. The application re-initialises the USB peripheral, which
     * desynchronises it from a host that never saw us disconnect — the port survives but
     * answers nothing. Detaching makes the host re-enumerate against the application. */
    usb_detach();

    __asm__ volatile("cpsid i" ::: "memory");

    /* Unlink every ICU event we may have set up, so nothing is pending or routed. */
    for (unsigned i = 0; i < ICU_IELSR_COUNT; i++) {
        REG32(ICU_IELSR + 4U * i) = 0;
    }

    /* Disable and clear every NVIC interrupt. 8 registers covers 256 lines; this part uses
     * far fewer, but clearing all of them is cheap and cannot be wrong. */
    for (unsigned i = 0; i < 8; i++) {
        REG32(0xE000E180UL + 4U * i) = 0xFFFFFFFFUL;   /* NVIC_ICER */
        REG32(0xE000E280UL + 4U * i) = 0xFFFFFFFFUL;   /* NVIC_ICPR */
    }

    /* SysTick off — the application will configure its own. */
    REG32(SYST_CSR) = 0;
    REG32(SYST_CVR) = 0;

    /* Clearing SysTick's enable bit stops the counter. It does NOT clear an exception the
     * counter already latched, and that distinction cost us several days.
     *
     * The stock bootloader runs ThreadX, so SysTick is live before we ever get control, and
     * it hands off to us with interrupts masked — which is why a pending SysTick never
     * troubles this image. The `cpsie i` that used to sit two lines below was the first
     * unmask since then, and by that point VTOR already pointed at the application. The
     * latched SysTick fired instantly, vectored through the application's table into its
     * default handler, and spun there forever. The application's reset handler never ran at
     * all, which is why nothing we changed inside the application made any difference: every
     * build failed identically, before its first instruction.
     *
     * So: clear what is pending, and do not unmask here. Reset_Handler unmasks once it has
     * routed interrupts, which is the only point at which taking one is meaningful. */
    REG32(SCB_ICSR) = ICSR_PENDSTCLR | ICSR_PENDSVCLR;

    __asm__ volatile("dsb" ::: "memory");

    /* Point the vector table at the application before enabling anything. */
    REG32(SCB_VTOR) = APP_BASE;
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* Load the application's stack pointer and branch to its reset handler. Never returns.
     * Interrupts stay masked across the handoff; the application unmasks them itself. */
    __asm__ volatile(
        "msr msp, %0   \n"
        "bx  %1        \n"
        :
        : "r"(sp), "r"(pc)
        : "memory");

    for (;;) { }
}

/* Warm reset, used to bounce back into the bootloader after the application has asked for
 * an update. The boot handshake in SRAM survives this. */
void bsp_system_reset(void)
{
    __asm__ volatile("dsb" ::: "memory");
    REG32(0xE000ED0CUL) = (REG32(0xE000ED0CUL) & 0x0700UL) | 0x05FA0004UL;  /* AIRCR SYSRESETREQ */
    __asm__ volatile("dsb" ::: "memory");
    for (;;) { }
}
