/* Shared memory map for both the second-stage bootloader and the application.
 * INCLUDEd by bootloader.ld and app.ld so the two can never drift apart.
 *
 *   0x00000000  1 MB      stock Electra bootloader.  NEVER WRITTEN BY US.
 *   0x00100000  128 KB    our bootloader ("EMB"). Flashed once, via the SD-card route.
 *   0x00120000  2.875 MB  application. Flashed over USB by our bootloader, as often as we like.
 *
 * The split is what removes the physical loop. The stock bootloader only ever has to place
 * our bootloader once; from then on our bootloader owns the application region and can
 * replace it over USB with no button presses, no re-plugging and no card access.
 *
 * The application region is deliberately the big one: our bootloader is small and static,
 * the application is what churns.
 */

MEMORY
{
    /* Declared only so a stray placement here fails the link loudly instead of quietly
       producing an image that would overwrite the stock bootloader. */
    STOCK_BOOT (rx)  : ORIGIN = 0x00000000, LENGTH = 1M

    EMB        (rx)  : ORIGIN = 0x00100000, LENGTH = 128K
    APP        (rx)  : ORIGIN = 0x00120000, LENGTH = 2944K    /* to the 4 MB device limit */

    /* SRAM.
     *
     * The part has 640 KB presented as one run from 0x1FFE0000, and that is what we use.
     *
     * An earlier note here claimed the low window behaves differently past roughly 0x1FFE8000,
     * on the evidence of two builds that failed once their stack or .bss reached beyond it.
     * That theory is contradicted by the stock bootloader, dumped on 2026-08-17, which runs its
     * own stack at 0x1FFECCE0 - well inside the window it supposedly could not use. The
     * 0x20000000 experiment the old comment described was tried and reverted; only the comment
     * survived, describing a layout that was never actually in force.
     *
     * The two original failures remain unexplained, so this is recorded rather than declared
     * solved. See docs/hardware-notes.md. The 32 bytes at the bottom are reserved for the boot
     * handshake, which is why this starts at 0x1FFE0020 rather than 0x1FFE0000. */
    RAM        (rwx) : ORIGIN = 0x1FFE0020, LENGTH = 640K - 32

    SDRAM      (rwx) : ORIGIN = 0x90000000, LENGTH = 32M
    QSPI       (r)   : ORIGIN = 0x60000000, LENGTH = 32M
    DATA_FLASH (r)   : ORIGIN = 0x40100000, LENGTH = 64K
}

/* Boot handshake: 32 bytes at the very bottom of SRAM that NEITHER image initialises.
 *
 * This is how the application asks to be reflashed. It writes a magic word here and resets;
 * our bootloader sees the magic, stays in USB flash mode instead of launching the app, and
 * clears it. That single mechanism is what makes "flash without touching the device" work,
 * because a running application can put itself into flash mode on request from the host.
 *
 * It must survive a warm reset, so it is outside both .data and .bss and is never cleared
 * by startup code. */
__boot_handshake = 0x1FFE0000;
