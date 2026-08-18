/* External SDRAM bring-up: 32 MB at 0x90000000, 16-bit bus.
 *
 * Transcribed from the stock firmware. The refresh figures are worth stating because they are the
 * one place the recovered values can be sanity-checked independently:
 *
 *   SDRFCR = 0x73A8  ->  REFW = 7, RFC = 0x3A8 = 936
 *   936 / 120 MHz (BCLK) = 7.8 us  ~=  64 ms / 8192 rows
 *
 * That agreement is why we are confident both in SDRFCR and in BCLK actually being 120 MHz.
 * Two earlier analysis passes disagreed here (0x3A8 vs 0x73A8) because the firmware
 * writes the register twice — once masking the top nibble, once the bottom.
 */

#include "s7g2.h"
#include "bsp.h"

#define SPIN_LIMIT 0x100000U

static int wait_sdsr_zero(void)
{
    for (unsigned i = 0; i < SPIN_LIMIT; i++) {
        if (REG32(SDSR) == 0) return 0;
    }
    return -1;
}

int bsp_sdram_init(void)
{
    /* The controller needs the bus clock settled before it is poked. */
    bsp_delay_us(100);

    /* Init-sequence parameters, written as three separate fields. Final SDIR = 0x0025. */
    REG16(SDIR) = (uint16_t)(REG16(SDIR) & 0xF8FFU);                  /* ARFI = 0 */
    if (wait_sdsr_zero() != 0) return -1;
    REG16(SDIR) = (uint16_t)((REG16(SDIR) & 0xFF0FU) | 0x0020U);      /* ARFC = 2 */
    if (wait_sdsr_zero() != 0) return -1;
    REG16(SDIR) = (uint16_t)((REG16(SDIR) & 0xFFF0U) | 0x0005U);      /* PRC  = 5 */
    if (wait_sdsr_zero() != 0) return -1;

    /* Run the JEDEC init sequence and wait for INIST to clear. */
    REG8(SDICR) = (uint8_t)(REG8(SDICR) | 0x01U);
    for (unsigned i = 0; i < SPIN_LIMIT; i++) {
        if (((REG8(SDSR) & 0x0FU) >> 3) == 0) break;
    }

    REG8(SDCCR)  = (uint8_t)(REG8(SDCCR) & 0xCFU);   /* BSIZE = 0 -> 16-bit bus */
    REG8(SDAMOD) = (uint8_t)(REG8(SDAMOD) | 0x01U);  /* BE = 1, continuous access */
    REG8(SDCMOD) = (uint8_t)(REG8(SDCMOD) & 0xFEU);  /* EMODE = 0, little endian */
    if (wait_sdsr_zero() != 0) return -1;

    /* Mode register: BL = 1, CAS latency 3, write burst single. Direct store, not RMW. */
    REG16(SDMOD) = 0x0230U;
    for (unsigned i = 0; i < SPIN_LIMIT; i++) {
        if ((REG32(SDSR) & 1U) == 0) break;
    }

    /* Timing, five fields -> 0x00052503: RAS 5, RCD 2, RP 2, WR 1, CL 3. */
    REG32(SDTR) = (REG32(SDTR) & 0xFFF8FFFFUL) | 0x00050000UL;
    REG32(SDTR) = (REG32(SDTR) & 0xFFFFCFFFUL) | 0x00002000UL;
    REG32(SDTR) = (REG32(SDTR) & 0xFFFFF1FFUL) | 0x00000400UL;
    REG32(SDTR) =  REG32(SDTR) | 0x00000100UL;
    REG32(SDTR) = (REG32(SDTR) & 0xFFFFFFF8UL) | 0x00000003UL;

    REG8(SDADR) = (uint8_t)((REG8(SDADR) & 0xFCU) | 0x01U);           /* 9-bit column */

    /* Two writes, one per field. Together: 0x73A8. */
    REG16(SDRFCR) = (uint16_t)((REG16(SDRFCR) & 0x0FFFU) | 0x7000U);  /* REFW = 7 */
    REG16(SDRFCR) = (uint16_t)((REG16(SDRFCR) & 0xF000U) | 0x03A8U);  /* RFC  = 936 */

    REG8(SDRFEN) = (uint8_t)(REG8(SDRFEN) | 0x01U);                   /* refresh on */
    REG8(SDCCR)  = (uint8_t)(REG8(SDCCR) | 0x01U);                    /* EXENB -> live */

    return 0;
}

/* Non-destructive smoke test: walk a few addresses across the range, restoring each.
 * Returns 0 on success. Useful as an early liveness check before anything relies on SDRAM. */
int bsp_sdram_selftest(void)
{
    static const uint32_t offsets[] = {
        0x00000000UL, 0x00000004UL, 0x00010000UL, 0x00100000UL,
        0x00400000UL, 0x01000000UL, 0x01FFFFF0UL,
    };
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        volatile uint32_t *p = (volatile uint32_t *)(SDRAM_BASE + offsets[i]);
        uint32_t saved = *p;
        *p = 0xA55A0000UL | i;
        if (*p != (0xA55A0000UL | i)) { *p = saved; return -(int)(i + 1); }
        *p = ~(0xA55A0000UL | i);
        if (*p != ~(0xA55A0000UL | i)) { *p = saved; return -(int)(i + 1); }
        *p = saved;
    }
    return 0;
}
