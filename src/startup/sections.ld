/* Section layout shared by the bootloader and the application.
 *
 * The including script must define FLASH_REGION before INCLUDEing this, e.g.
 *   REGION_ALIAS("FLASH", EMB)
 *
 * Everything else — vector table first, .data with a separate load region, stack and heap —
 * is identical between the two images, so it lives here rather than being copy-pasted and
 * allowed to diverge.
 */

__stack_size = DEFINED(__stack_size) ? __stack_size : 16K;
__heap_size  = DEFINED(__heap_size)  ? __heap_size  : 16M;

ENTRY(Reset_Handler)

SECTIONS
{
    /* Vector table first. For the bootloader this is where the stock bootloader hands off;
       for the application it is what our bootloader points VTOR at before jumping. */
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
        *(.text) *(.text.*)
        *(.glue_7) *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata) *(.rodata.*)
        . = ALIGN(4);
    } > FLASH

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

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > FLASH

    .data :
    {
        . = ALIGN(4);
        __data_start = .;
        *(.data) *(.data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH
    __data_load = LOADADDR(.data);

    /* Code that must not execute from flash while flash is being programmed. The FACI
       cannot serve read requests during an erase or write, so anything on that path is
       copied to RAM at startup and run from there.

       No longer empty in the application image: the app now writes the persistent boot
       record, so it links the same flash driver the bootloader uses. */
    .ramfunc :
    {
        . = ALIGN(4);
        __ramfunc_start = .;
        *(.ramfunc) *(.ramfunc.*)
        . = ALIGN(4);
        __ramfunc_end = .;
    } > RAM AT > FLASH
    __ramfunc_load = LOADADDR(.ramfunc);

    /* Where the image actually ends in FLASH, as opposed to where its sections end in RAM.
       .ramfunc is the last thing loaded, so this is the high-water mark of everything the
       flasher wrote.

       It exists so an image can checksum ITSELF over exactly its own extent. Checksumming the
       whole 2.9 MB region instead was measured at 1194 ms, which is far too slow to pay on
       every boot; over a real 220 KB image the same work is about a tenth of that. */
    __image_end = __ramfunc_load + (__ramfunc_end - __ramfunc_start);

    /* ORDER MATTERS: the stack is placed BEFORE .bss, deliberately.

       With .bss first, every byte added to .bss moved the stack, and the application began
       dying with a stack-monitor NMI once .bss grew by about 12 KB. A layout in which adding an
       unrelated static array relocates the stack makes the stack's address an accident of other
       code — a bad property regardless of what went wrong here. Placing the stack directly
       above .data pins it, so it stays put however much the image grows.

       16-byte alignment is separate and also required: MSPMPUSA/MSPMPUEA ignore their low
       address bits, so a boundary that is only 8-aligned describes a window that is not quite
       the stack. arm_stack_monitor() masks as well, so neither end can get it wrong.

       See docs/hardware-notes.md for the full investigation, including the theory that looked
       right and was disproved. */
    .stack (NOLOAD) :
    {
        . = ALIGN(16);
        __stack_bottom = .;
        . += __stack_size;
        . = ALIGN(16);
        __stack_top = .;
    } > RAM

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss) *(.bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /* Heap in SDRAM, not SRAM. Not usable until the SDRAM controller is up, so nothing
       may allocate before bsp_sdram_init(). */
    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        __heap_start = .;
        . += __heap_size;
        __heap_end = .;
    } > SDRAM

    /DISCARD/ : { *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }
}
