/* Boot handshake: 32 bytes at the bottom of SRAM that survive a warm reset.
 *
 * This is how the application asks to be reflashed without anyone touching the device. The
 * host sends a "reboot for update" command over USB; the application writes REQUEST_UPDATE
 * here and resets; our bootloader sees it, stays in flash mode instead of launching the
 * application, and clears it. The host then pushes a new image and commands RUN.
 *
 * The region is carved out of SRAM by memory.ld and is never touched by .data or .bss
 * initialisation in either image, which is what lets it carry information across the reset.
 *
 * It is deliberately paranoid about validity: a cold boot leaves SRAM in an undefined state,
 * and a random word matching the magic would strand the device in the bootloader. Hence a
 * magic, an inverted copy, and a checksum — all three must agree.
 */

#ifndef ELECTRA_BOOT_HANDSHAKE_H
#define ELECTRA_BOOT_HANDSHAKE_H

#include <stdint.h>

#define BOOT_HANDSHAKE_ADDR   0x1FFE0000UL

#define BOOT_MAGIC            0x454D4231UL   /* "EMB1" */

/* Reasons, in the `request` field. */
#define BOOT_REQ_NONE         0x00000000UL
#define BOOT_REQ_UPDATE       0x55504454UL   /* "UPDT" — stay in the bootloader */
#define BOOT_REQ_RUN_APP      0x52554E41UL   /* "RUNA" — launch the app immediately */

/* Written by an application only once it is demonstrably working — host enumerated, console
 * available. The bootloader will not hand over to an app that has never set it.
 *
 * This exists because clearing app_faults turned out to be worthless as a health signal: the
 * app that hung during display bring-up cleared it on its first line, so every boot looked
 * healthy and the bootloader relaunched the hang forever. A distinct magic that older images
 * do not know about cannot be set by accident, and cannot be set early by mistake. */
#define BOOT_HEALTHY          0x4F4B4159UL   /* "OKAY" */

typedef struct {
    uint32_t magic;         /* BOOT_MAGIC */
    uint32_t magic_inv;     /* ~BOOT_MAGIC — guards against a half-written struct */
    uint32_t request;       /* BOOT_REQ_* */
    uint32_t detail;        /* free-form; e.g. the app's build id when it asked */
    uint32_t boot_count;    /* incremented by the bootloader every launch */
    uint32_t app_faults;    /* incremented when an app is launched, cleared when it checks in */
    uint32_t health;        /* BOOT_HEALTHY once an app has proven itself; see below */
    uint32_t checksum;      /* sum of the preceding 7 words, negated */
} boot_handshake_t;

#define BOOT_HANDSHAKE (*(volatile boot_handshake_t *)BOOT_HANDSHAKE_ADDR)

/* Breadcrumb channel.
 *
 * An application that hangs before USB is up cannot say anything, and every hypothesis we
 * formed about *where* it hung was wrong. So the application stamps its progress into a word
 * that survives a warm reset and that the already-flashed bootloader prints unmodified.
 *
 * `app_faults` is that word. It is a deliberate re-use: the bootloader's `id` command prints
 * it, and the bootloader cannot be replaced without the SD card, so the reporting side is
 * fixed and we must fit the channel to it. Press the rear RESET button after a hang — SRAM
 * keeps its contents across a reset while power is held, and the bootloader will read out the
 * last stage the application reached.
 *
 * A no-op in the bootloader (weak, in startup.c), so the bootloader's own startup cannot
 * overwrite the crumbs it exists to report.
 */
void boot_stage(uint32_t code);

/* Always recorded — used by the fault paths.
 *
 * boot_stage() goes quiet once an application has reported itself healthy, because the crumb
 * shares a word with the crash counter and a proven app must not look like it is crash-looping.
 * But a FAULT must always be recorded: the moment the crumb is most needed is after an app that
 * used to work stops working, which is precisely when the quiet rule was hiding it. */
void boot_stage_force(uint32_t code);

/* Startup, in order. */
#define STAGE_RESET         0x11
#define STAGE_CLOCKS        0x12
#define STAGE_PINS          0x13
#define STAGE_SDRAM         0x14
#define STAGE_CRT           0x15
#define STAGE_STACKMON      0x16
#define STAGE_ICU           0x17
#define STAGE_PREMAIN       0x18
/* Application main, in order. */
#define STAGE_MAIN          0x20
#define STAGE_USB_INIT      0x21
#define STAGE_CONSOLE       0x22
#define STAGE_ENUM_WAIT     0x23
#define STAGE_ENUMERATED    0x24
#define STAGE_HEALTHY       0x25
#define STAGE_LCD_INIT      0x26
#define STAGE_LCD_ON        0x27
#define STAGE_BACKLIGHT     0x28
#define STAGE_LOOP          0x29
/* An exception fired: 0xF0 | (exception number << 8) | (stage it happened in << 16). */
#define STAGE_FAULT(ipsr, last)  (0x000000F0UL | (((ipsr) & 0xFFUL) << 8) | (((last) & 0xFFUL) << 16))

static inline uint32_t boot_handshake_checksum(const volatile boot_handshake_t *h)
{
    uint32_t sum = h->magic + h->magic_inv + h->request + h->detail
                 + h->boot_count + h->app_faults + h->health;
    return ~sum;
}

static inline int boot_handshake_valid(void)
{
    const volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
    return h->magic == BOOT_MAGIC
        && h->magic_inv == ~BOOT_MAGIC
        && h->checksum == boot_handshake_checksum(h);
}

static inline void boot_handshake_init(void)
{
    volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
    h->magic      = BOOT_MAGIC;
    h->magic_inv  = ~BOOT_MAGIC;
    h->request    = BOOT_REQ_NONE;
    h->detail     = 0;
    h->boot_count = 0;
    h->app_faults = 0;
    h->health = 0;
    h->checksum   = boot_handshake_checksum(h);
}

static inline void boot_handshake_set_request(uint32_t request, uint32_t detail)
{
    volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
    if (!boot_handshake_valid()) boot_handshake_init();
    h->request  = request;
    h->detail   = detail;
    h->checksum = boot_handshake_checksum(h);
}

#endif /* ELECTRA_BOOT_HANDSHAKE_H */
