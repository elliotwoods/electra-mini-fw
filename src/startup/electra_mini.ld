/* Linker script for a replacement Electra One Mini application.
 *
 * The bootloader owns 0x00000000-0x000FFFFF and chain-loads us at 0x00100000. It sets
 * SCB->VTOR (the stock application never writes it), then the core takes MSP from
 * vector[0] and PC from vector[1]. There is no header, magic, CRC or signature ahead of
 * the vector table -- the stock image starts with SP/PC immediately, confirmed byte for
 * byte -- so our image must do the same.
 *
 * Memory layout is taken from the stock firmware; see docs/hardware-notes.md.
 *
 * NOTE: an SSP project ships its own generated script. Use this as the reference for the
 * regions and the placement rules; the parts that matter are the FLASH origin, the vector
 * table coming first, and the SDRAM heap.
 */

MEMORY
{
    /* Owned by the bootloader. Declared only so a stray placement here fails loudly
       rather than silently producing an image that would overwrite it. */
    BOOTLOADER (rx)  : ORIGIN = 0x00000000, LENGTH = 1M

    /* Our application. 4 MB part, minus the bootloader's megabyte.
       Stock v4.1.4 occupied 0x00100000-0x003AE3DC (2,745 KiB). */
    FLASH (rx)       : ORIGIN = 0x00100000, LENGTH = 3M

    /* On-chip SRAM, 640 KB. Stock put its initial MSP at 0x20010360. */
    RAM (rwx)        : ORIGIN = 0x1FFE0000, LENGTH = 640K

    /* External SDRAM, initialised by our startup (controller at 0x40003C00).
       Stock uses >= 32 MB of it: a 320 KB D/AVE 2D pool at 0x90000000 and a
       16 MB C heap from 0x90050000. We drop D/AVE 2D, so all of it is ours. */
    SDRAM (rwx)      : ORIGIN = 0x90000000, LENGTH = 32M

    /* QSPI NOR, memory-mapped once the controller is brought up (regs at 0x64000000). */
    QSPI (r)         : ORIGIN = 0x60000000, LENGTH = 32M

    /* Data flash (E2 emulation). Holds the USB descriptor strings and serial. */
    DATA_FLASH (r)   : ORIGIN = 0x40100000, LENGTH = 64K
}

/* Stock used a 16 KB main stack. Heap lives in SDRAM, as stock did. */
__stack_size = DEFINED(__stack_size) ? __stack_size : 16K;
__heap_size  = DEFINED(__heap_size)  ? __heap_size  : 16M;

ENTRY(Reset_Handler)

SECTIONS
{
    /* The vector table MUST be the first thing at 0x00100000: the bootloader hands off
       via the SP/PC pair sitting there. */
    .vectors :
    {
        __vectors_start = .;
        KEEP(*(.vectors))
        KEEP(*(.vectors.*))
        . = ALIGN(4);
        __vectors_end = .;
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text.*)
        *(.glue_7) *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4);
    } > FLASH

    /* C++ static constructors. Stock ran 23 of these, after the .data copy. */
    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM.exidx   :
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > FLASH

    /* Initialised data: lives in RAM, image trails the code in flash.
       Startup copies __data_load .. (__data_load + size) to __data_start.
       `AT > FLASH` sets the load region; do not also give an explicit AT() address, or ld
       rejects the section as having both. */
    .data :
    {
        . = ALIGN(4);
        __data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /* Main stack, at the top of the region we use. */
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        __stack_bottom = .;
        . += __stack_size;
        . = ALIGN(8);
        __stack_top = .;
    } > RAM

    /* Heap in SDRAM, not SRAM -- SRAM is only 640 KB and the graphics work wants room.
       Not available until the SDRAM controller is up, so nothing may malloc before then. */
    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        __heap_start = .;
        . += __heap_size;
        __heap_end = .;
    } > SDRAM

    /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }

    /* Fail the link rather than produce an image that would land on the bootloader. */
    ASSERT(__vectors_start == 0x00100000,
           "vector table must start at 0x00100000 -- the bootloader hands off there")
}
