/* Board support entry points for the Electra One Mini. */

#ifndef ELECTRA_BSP_H
#define ELECTRA_BSP_H

#include <stdint.h>
#include <stddef.h>

/* PRCR is reference-counted; unlock/lock must be balanced.
 * Group 0 gates the clock registers, group 1 the operating power mode. */
void prcr_unlock(unsigned group);
void prcr_lock(unsigned group);

/* Must be called before bsp_clock_init: the refcounts live in .bss, which is not cleared
 * until after the clocks are up. */
void prcr_reset(void);

/* Reconfigure one pin after boot (display reset, mux address lines). */
void bsp_pin_cfg(uint32_t port, uint32_t pin, uint32_t cfg);

/* Non-destructive SDRAM walk. 0 = pass, negative = index of the offset that failed. */
int  bsp_sdram_selftest(void);

/* 24 MHz crystal -> PLL -> 240 MHz, with wait states raised first.
 * Returns 0, -1 if the main oscillator never stabilised, -2 if the PLL did not. */
int  bsp_clock_init(void);

/* External SDRAM at 0x90000000. Must run after the clocks, because the refresh interval
 * is expressed in BCLK cycles. */
int  bsp_sdram_init(void);

/* Applies the 100-pin table recovered from stock firmware. Must run before any peripheral
 * that needs its pins muxed — which is all of them. */
void bsp_pins_init(void);

/* Writes the 25 ICU event-link slots. On this part an NVIC enable alone routes nothing,
 * so skipping this produces peripherals that appear dead with no error. */
void bsp_icu_init(void);

void bsp_delay_us(uint32_t us);
void bsp_delay_ms(uint32_t ms);

/* The recovered pin table, defined in electra_pin_cfg.c. The layout matches SSP's
 * ioport_pin_cfg_t, which happens to be the PmnPFS register layout directly. */
typedef struct {
    uint32_t pin_cfg;
    uint32_t pin;          /* (port << 8) | pin_number */
} electra_pin_cfg_t;

extern const electra_pin_cfg_t g_electra_pin_cfg_data[];
extern const uint32_t          g_electra_pin_cfg_count;

#endif /* ELECTRA_BSP_H */
