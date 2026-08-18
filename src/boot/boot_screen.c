/* What the bootloader puts on the panel while it waits.
 *
 * Until now it put nothing there. A device sitting in the bootloader showed a black screen,
 * which is indistinguishable from a device that is dead, a device whose backlight has failed,
 * and a device whose application crashed before drawing anything. Three very different problems
 * with one symptom, and the only way to tell them apart was to plug into a computer -- which is
 * exactly the step somebody staring at a black screen has not got to yet.
 *
 * So it says where it is and why, and what to do about it.
 *
 * THE DISPLAY IS NEVER ALLOWED TO COST US THE RECOVERY PATH. Everything here runs AFTER USB is
 * up and the console is answering, and nothing here is checked for success: if the panel does
 * not come up, the flashing path is untouched and the device is still reachable. That ordering
 * is the same rule the application learned the hard way -- an early version drew a test pattern
 * before polling USB, and because every status wait is an SPI transaction that a non-answering
 * controller runs to its limit, the device vanished from the bus entirely. A bootloader that
 * did that would be unrecoverable rather than merely quiet.
 */

#include <stdint.h>

#include "s7g2.h"
#include "bsp.h"
#include "ra8876.h"
#include "spi.h"
#include "text.h"
#include "font.h"
#include "boot_handshake.h"
#include "boot_screen.h"

#define BG       0x0000u        /* black */
#define FG       0xFFFFu        /* white */
#define DIM      0x8410u        /* mid grey, for labels */
#define ACCENT   0xFD20u        /* amber: this is a state that wants attention */
#define OK_GREEN 0x07E0u

#define W 800
#define H 480

static void put_hex(char *out, uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++) out[2 + i] = digits[(v >> (28 - 4 * i)) & 0xFu];
    out[10] = 0;
}

/* Why we are here, in the words somebody standing at the device needs. The reason codes are
 * the bootloader's own, from src/boot/main.c. */
static const char *reason_title(uint32_t reason)
{
    switch (reason) {
    case BOOT_SCREEN_APP_REQUESTED: return "UPDATE MODE";
    case BOOT_SCREEN_APP_INVALID:   return "NO APPLICATION";
    case BOOT_SCREEN_CRASH_LOOP:    return "APPLICATION KEEPS CRASHING";
    case BOOT_SCREEN_HOST_CLAIMED:  return "HELD BY HOST";
    case BOOT_SCREEN_UNPROVEN_APP:  return "WAITING FOR A HOST";
    default:                        return "BOOTLOADER";
    }
}

static const char *reason_detail(uint32_t reason)
{
    switch (reason) {
    case BOOT_SCREEN_APP_REQUESTED:
        return "The application asked to be replaced. Flash a new one and it will start.";
    case BOOT_SCREEN_APP_INVALID:
        return "There is no launchable image in the application slot.";
    case BOOT_SCREEN_CRASH_LOOP:
        return "It was launched repeatedly and never reported itself working.";
    case BOOT_SCREEN_HOST_CLAIMED:
        return "A host asked to keep control instead of letting the application start.";
    case BOOT_SCREEN_UNPROVEN_APP:
        return "This image has not run successfully yet, so it is not started on its own.";
    default:
        return "";
    }
}

static void label_value(int16_t x, int16_t y, const char *label, const char *value)
{
    int16_t w = text_draw(&font_small, x, y, label, DIM, BG);
    text_draw(&font_small, (int16_t)(x + w + 8), y, value, FG, BG);
}

void boot_screen_show(uint32_t reason)
{
    volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
    char buf[16];

    spi1_init();
    if (ra8876_init() != 0) return;          /* no panel: the console still works, which is what
                                              * actually matters here */
    ra8876_display_on(1);
    ra8876_backlight(0x0800);

    ra8876_set_bg(BG);
    ra8876_set_fg(BG);
    ra8876_fill_rect(0, 0, W, H);

    /* A band across the top, so the state is readable from across a room rather than only by
     * someone already leaning in to read the small print. */
    ra8876_set_fg(reason == BOOT_SCREEN_HOST_CLAIMED ? OK_GREEN : ACCENT);
    ra8876_fill_rect(0, 0, W, 6);

    text_draw(&font_ui,    40, 48, reason_title(reason),  FG,  BG);
    text_draw(&font_small, 40, 96, reason_detail(reason), DIM, BG);

    /* The facts, in the order somebody diagnosing would want them. */
    int16_t y = 160;
    const int16_t step = 26;

    put_hex(buf, (uint32_t)BOOT_APP_BASE);
    label_value(40, y, "application at", buf);           y = (int16_t)(y + step);

    label_value(40, y, "image", h->app_faults == 0 ? "present" : "present, see crumb below");
    y = (int16_t)(y + step);

    put_hex(buf, h->boot_count);
    label_value(40, y, "boots", buf);                    y = (int16_t)(y + step);

    put_hex(buf, h->app_faults);
    label_value(40, y, "last crumb", buf);               y = (int16_t)(y + step + 12);

    /* What to do. A status screen that describes a problem and not its remedy has done half a
     * job, and this is the half somebody at the device actually needs. */
    text_draw(&font_small, 40, y, "Connect USB. The bootloader is a serial port.", FG, BG);
    y = (int16_t)(y + step);
    text_draw(&font_small, 40, y, "python tools/deploy/flash_usb.py build/app.srec", DIM, BG);

    text_draw(&font_small, 40, H - 40, "electra-mini-fw bootloader", DIM, BG);
}
