/* Pin muxing.
 *
 * The application configures all 100 pins itself — nothing is inherited from the bootloader
 * except SCB->VTOR. The table in electra_pin_cfg.c was recovered verbatim from the stock
 * firmware (ioport_cfg_t at 0x0031E3CC -> 100 entries at 0x0031E0AC).
 *
 * SSP's ioport_cfg_options_t bitfield turns out to be the PmnPFS register layout, so each
 * recovered pin_cfg word is written straight through. Verified against the decode:
 *   0x00004000 -> ISEL      (IRQ input)
 *   0x00008000 -> ASEL      (analog)
 *   0x00000005 -> PODR|PDR  (output, driven high)
 *   0x00004010 -> ISEL|PCR  (IRQ input with pull-up)
 *   0x0B010C00 -> PSEL 0xB | PMR | DSCR high-drive  (external bus)
 */

#include "s7g2.h"
#include "bsp.h"

/* PWPR guards every PmnPFS write.
 *   unlock: clear B0WI (bit7), then set PFSWE (bit6)
 *   lock:   clear PFSWE, then set B0WI
 * The order is not optional — B0WI must be clear before PFSWE can be written. */
static void pfs_unlock(void)
{
    REG8(PWPR) = 0x00U;
    REG8(PWPR) = 0x40U;
}

static void pfs_lock(void)
{
    REG8(PWPR) = 0x00U;
    REG8(PWPR) = 0x80U;
}

void bsp_pins_init(void)
{
    pfs_unlock();
    for (uint32_t i = 0; i < g_electra_pin_cfg_count; i++) {
        uint32_t pin  = g_electra_pin_cfg_data[i].pin;
        uint32_t port = (pin >> 8) & 0xFFU;
        uint32_t num  =  pin       & 0xFFU;
        REG32(PFS(port, num)) = g_electra_pin_cfg_data[i].pin_cfg;
    }
    pfs_lock();
}

/* Reconfigure a single pin at runtime. Needed for the display reset line and the analog
 * multiplexer address lines, which are driven as plain GPIO after boot. */
void bsp_pin_cfg(uint32_t port, uint32_t pin, uint32_t cfg)
{
    pfs_unlock();
    REG32(PFS(port, pin)) = cfg;
    pfs_lock();
}
