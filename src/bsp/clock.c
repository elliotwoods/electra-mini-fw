/* Clock and wait-state bring-up, reproducing the stock firmware's sequence exactly.
 *
 * The board runs from a 24 MHz crystal through the PLL to 240 MHz. Getting there has a
 * mandatory order: wait states must be raised BEFORE the dividers change, or the core starts
 * fetching from flash faster than the flash can answer. The stock firmware calls its
 * wait-state helper with phase=0 before the divider store and phase=1 after; the "after" pass
 * is a no-op at this frequency, so only the "before" pass is reproduced here.
 *
 * Values transcribed from the stock firmware and its callees. See docs/hardware-notes.md.
 */

#include "s7g2.h"
#include "bsp.h"

/* ------------------------------------------------------------------ PRCR */
/* Reference-counted so nested unlocks do not lock early.
 *
 * These live in .bss, which is NOT yet cleared when bsp_clock_init runs — the bulk .bss
 * clear is deliberately deferred until the core is at full speed, because it is 161 KB.
 * So the counters must be zeroed explicitly first. The stock firmware does exactly this,
 * zeroing its equivalents at 0x1FFE4E1C before touching the clocks; getting it wrong means
 * a garbage refcount leaves PRCR locked and every subsequent clock write is silently
 * dropped. Hence prcr_reset(), which Reset_Handler must call before anything else. */
static uint16_t prcr_depth[2];

void prcr_reset(void)
{
    prcr_depth[0] = 0;
    prcr_depth[1] = 0;
}

void prcr_unlock(unsigned group)
{
    if (prcr_depth[group]++ == 0) {
        uint16_t bit = (group == 0) ? PRCR_GROUP0 : PRCR_GROUP1;
        REG16(PRCR) = (uint16_t)(PRCR_KEY | REG16(PRCR) | bit);
    }
}

void prcr_lock(unsigned group)
{
    if (prcr_depth[group] > 0 && --prcr_depth[group] == 0) {
        uint16_t bit = (group == 0) ? PRCR_GROUP0 : PRCR_GROUP1;
        REG16(PRCR) = (uint16_t)((PRCR_KEY | REG16(PRCR)) & (uint16_t)~bit);
    }
}

/* ------------------------------------------------------------------ helpers */

/* Bounded spins. The stock code uses 0xFFFF iterations and traps on expiry; we return a
 * failure the caller can act on rather than wedging in a BKPT on a device with no debugger. */
#define SPIN_LIMIT 0xFFFFU

static int wait_clear8(uint32_t reg, uint8_t mask)
{
    for (unsigned i = 0; i < SPIN_LIMIT; i++) {
        if ((REG8(reg) & mask) == 0) return 0;
    }
    return -1;
}

static int wait_set8(uint32_t reg, uint8_t mask)
{
    for (unsigned i = 0; i < SPIN_LIMIT; i++) {
        if ((REG8(reg) & mask) != 0) return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ flash cache */

static void flash_cache_enable(void)
{
    REG8(FCACHEIV) = (uint8_t)(REG8(FCACHEIV) | 0x01U);
    (void)wait_clear8(FCACHEIV, 0x01U);
    REG8(FCACHEE) = 0x01U;
}

/* ------------------------------------------------------------------ wait states */

/* Order matters and is reproduced literally: SRAMWTSC bits are set 2, 3, 0, 1 in one
 * unlock window, then bit 4 in a second. Net result is 0x1F. */
static void sram_wait_states(void)
{
    REG8(SRAMPRCR) = 0xF1U;                                  /* unlock */
    REG8(SRAMWTSC) = (uint8_t)((REG8(SRAMWTSC) & 0xFBU) | (1U << 2));
    REG8(SRAMWTSC) = (uint8_t)((REG8(SRAMWTSC) & 0xF7U) | (1U << 3));
    REG8(SRAMWTSC) = (uint8_t)((REG8(SRAMWTSC) & 0xFEU) | (1U << 0));
    REG8(SRAMWTSC) = (uint8_t)((REG8(SRAMWTSC) & 0xFDU) | (1U << 1));
    REG8(SRAMPRCR) = 0xF0U;                                  /* lock */

    REG8(SRAMPRCR) = 0xF1U;
    REG8(SRAMWTSC) = (uint8_t)((REG8(SRAMWTSC) & 0xEFU) | (1U << 4));
    REG8(SRAMPRCR) = 0xF0U;
}

static void flash_wait_states(void)
{
    /* FLWT has no protection register of its own. 2 cycles is what 240 MHz needs. */
    REG8(FLWT) = (uint8_t)((REG8(FLWT) & 0xF8U) | 2U);
}

/* ------------------------------------------------------------------ oscillators */

static int main_osc_start(void)
{
    if ((REG8(MOSCCR) & 0x01U) == 0) {
        return 0;                                            /* already running */
    }

    prcr_unlock(0);
    REG8(MOMCR)    = (uint8_t)(REG8(MOMCR) & ~0x30U);        /* MODRV = 0 */
    REG8(MOMCR)    = (uint8_t)(REG8(MOMCR) & 0xBFU);         /* MOSEL = 0, resonator */
    REG8(MOSCWTCR) = (uint8_t)((REG8(MOSCWTCR) & 0xF0U) | 0x05U);
    REG8(MOSCCR)   = (uint8_t)(REG8(MOSCCR) & 0xFEU);        /* MOSTP = 0 -> start */
    prcr_lock(0);

    if (wait_clear8(MOSCCR, 0x01U) != 0) return -1;
    return wait_set8(OSCSF, OSCSF_MOSCSF);
}

/* PLLCCR ends up 0x2701: PLLMUL[13:8] = 0x27 (x20.0), PLLSRCSEL[4] = 0 (main osc),
 * PLIDIV[1:0] = 1 (/2). 24 MHz / 2 * 20 = 240 MHz. */
static int pll_start(void)
{
    if ((REG8(PLLCR) & 0x01U) == 0) {
        return 0;
    }

    prcr_unlock(0);
    REG16(PLLCCR) = (uint16_t)(REG16(PLLCCR) & 0xFFEFU);              /* source = main */
    REG16(PLLCCR) = (uint16_t)((REG16(PLLCCR) & 0xFFFCU) | 0x0001U);  /* PLIDIV = /2 */
    REG16(PLLCCR) = (uint16_t)((REG16(PLLCCR) & 0xC0FFU) | 0x2700U);  /* PLLMUL = x20 */
    prcr_lock(0);

    /* High-speed operating mode must be selected before the PLL is allowed to drive the
     * core. The stock code disables the flash cache across this window. */
    prcr_unlock(1);
    REG8(FCACHEE) = (uint8_t)(REG8(FCACHEE) & 0xFEU);
    REG8(SOPCCR)  = (uint8_t)(REG8(SOPCCR) & 0xFEU);
    REG8(OPCCR)   = (uint8_t)(REG8(OPCCR) & 0xFCU);                   /* OPCM = 0 */
    for (unsigned i = 0; i < SPIN_LIMIT; i++) {
        if (((REG8(SOPCCR) & 0x1FU) >> 4) == 0 && ((REG8(OPCCR) & 0x1FU) >> 4) == 0) break;
    }
    flash_cache_enable();
    prcr_lock(1);

    prcr_unlock(0);
    REG8(PLLCR) = (uint8_t)(REG8(PLLCR) & 0xFEU);                     /* PLLSTP = 0 */
    prcr_lock(0);

    if (wait_clear8(PLLCR, 0x01U) != 0) return -1;
    return wait_set8(OSCSF, OSCSF_PLLSF);
}

/* ------------------------------------------------------------------ entry point */

int bsp_clock_init(void)
{
    /* Sub-clock oscillator is unused on this board and is stopped explicitly, matching
     * stock. Leaving it running costs power and nothing else, but match the reference. */
    prcr_unlock(0);
    REG8(HOCOWTCR) = (uint8_t)((REG8(HOCOWTCR) & 0xF8U) | 0x06U);
    REG8(SOSCCR)   = (uint8_t)(REG8(SOSCCR) | 0x01U);
    prcr_lock(0);
    (void)wait_set8(SOSCCR, 0x01U);
    prcr_unlock(0);
    REG8(SOMCR) = (uint8_t)(REG8(SOMCR) & ~0x02U);
    prcr_lock(0);

    flash_cache_enable();

    if (main_osc_start() != 0) return -1;
    if (pll_start() != 0)      return -2;

    /* WAIT STATES BEFORE THE DIVIDER CHANGE. This ordering is the whole reason this
     * function is not simply "set SCKDIVCR". */
    sram_wait_states();
    flash_wait_states();

    prcr_unlock(0);
    REG8(SCKSCR)    = (uint8_t)((REG8(SCKSCR) & 0xF8U) | 0x05U);  /* source = PLL */
    REG32(SCKDIVCR) = 0x20011221UL;                               /* see header for fields */
    prcr_lock(0);

    prcr_unlock(0);
    REG8(SCKDIVCR2) = (uint8_t)((REG8(SCKDIVCR2) & 0x8FU) | (4U << 4)); /* UCK /5 = 48 MHz */
    REG8(BCKCR)     = (uint8_t)((REG8(BCKCR) & 0xFEU) | 0x01U);
    REG8(EBCKOCR)   = (uint8_t)(REG8(EBCKOCR) | 0x01U);           /* external bus clock */
    REG8(SDCKOCR)   = (uint8_t)(REG8(SDCKOCR) | 0x01U);           /* SDRAM clock */
    prcr_lock(0);

    return 0;
}

/* Crude blocking delay. Calibrated for 240 MHz ICLK; the loop is 4 cycles on Cortex-M4
 * when built at -O1 or above, which is what the stock firmware assumes too. */
void bsp_delay_us(uint32_t us)
{
    volatile uint32_t n = (F_ICLK_HZ / 4000000UL) * us;
    while (n--) {
        __asm__ volatile("");
    }
}

void bsp_delay_ms(uint32_t ms)
{
    while (ms--) bsp_delay_us(1000);
}
