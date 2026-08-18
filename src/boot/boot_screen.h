/* The bootloader's status screen. See boot_screen.c for why it exists and why it runs last. */

#ifndef ELECTRA_BOOT_SCREEN_H
#define ELECTRA_BOOT_SCREEN_H

#include <stdint.h>

/* Mirrors the reason codes in src/boot/main.c. Named here so the screen does not have to
 * duplicate the numbers, and so a change to one is a compile error rather than a wrong caption
 * on a device somebody is trying to diagnose. */
#define BOOT_SCREEN_LAUNCHED       0u
#define BOOT_SCREEN_APP_REQUESTED  1u
#define BOOT_SCREEN_APP_INVALID    2u
#define BOOT_SCREEN_CRASH_LOOP     3u
#define BOOT_SCREEN_HOST_CLAIMED   4u
#define BOOT_SCREEN_UNPROVEN_APP   5u

/* Where the application lives, for display only. */
#define BOOT_APP_BASE 0x00120000UL

/* Draw it. Call ONLY after USB and the console are up: the panel must never be able to cost us
 * the path by which a broken device is repaired. Silently does nothing if the controller does
 * not come up. */
void boot_screen_show(uint32_t reason);

#endif /* ELECTRA_BOOT_SCREEN_H */
