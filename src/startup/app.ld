/* The application: 2944 KB at 0x00120000.
 *
 * Launched by our bootloader, which sets SCB->VTOR to this image's base before jumping, so
 * the vector table works from here despite not being at 0x00100000.
 *
 * This is the image that churns. It is flashed over USB with no physical intervention, and
 * a bad one costs nothing worse than a reboot into the bootloader — which is the entire
 * point of the split.
 */

INCLUDE memory.ld

REGION_ALIAS("FLASH", APP)

INCLUDE sections.ld

ASSERT(__vectors_start == 0x00120000,
       "application vector table must start at 0x00120000 - the bootloader points VTOR there")
