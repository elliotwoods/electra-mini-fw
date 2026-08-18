/* Our second-stage bootloader: 128 KB at 0x00100000.
 *
 * The stock Electra bootloader hands off here, exactly as it did to the stock application —
 * plain Cortex-M vector table, no header, no CRC. This image is flashed once through the
 * SD-card route and then, ideally, never again: everything that changes afterwards lives in
 * the application region and goes over USB.
 *
 * Keep it small and keep it boring. It is the one component whose failure costs a physical
 * recovery cycle.
 */

INCLUDE memory.ld

REGION_ALIAS("FLASH", EMB)

/* The bootloader has no business owning a 16 MB heap; it never allocates. */
__heap_size = 0;

INCLUDE sections.ld

ASSERT(__vectors_start == 0x00100000,
       "bootloader vector table must start at 0x00100000 - the stock bootloader hands off there")
