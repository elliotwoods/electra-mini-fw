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

/* The application-requested case, split by WHY it asked. Only a deliberate `boot` command means
 * an update; the other three are the application reporting that it stopped, and saying "flash a
 * new one" to somebody holding a crashed device sends them to fix the wrong thing.
 *
 * Added above the codes above rather than replacing BOOT_SCREEN_APP_REQUESTED, which remains the
 * caption for a reason that cannot be classified. Which one applies is read out of the crumb the
 * application left; see reason_for_crumb in src/boot/main.c for the shapes and why they cannot
 * collide. */
#define BOOT_SCREEN_APP_UPDATE     6u   /* somebody typed `boot` */
#define BOOT_SCREEN_APP_FAULT      7u   /* hard/bus/usage fault */
#define BOOT_SCREEN_APP_NMI        8u   /* stack-monitor NMI storm */
#define BOOT_SCREEN_APP_HANDBACK   9u   /* no host could reach the application */

/* Where the application lives, for display only. */
#define BOOT_APP_BASE 0x00120000UL

/* Draw it. Call ONLY after USB and the console are up: the panel must never be able to cost us
 * the path by which a broken device is repaired. Silently does nothing if the controller does
 * not come up. */
void boot_screen_show(uint32_t reason);

#endif /* ELECTRA_BOOT_SCREEN_H */
