/* EMB — the Electra Mini Bootloader.
 *
 * Sits at 0x00100000, where the stock Electra bootloader hands off. Owns the application
 * region at 0x00120000 and can replace it over USB with no physical intervention: no button
 * chord, no re-plugging, no card access. That is its whole reason for existing.
 *
 * How the device gets into flash mode, in order of preference:
 *
 *   1. The application asks.  Host sends "reboot for update" over USB; the app writes
 *      BOOT_REQ_UPDATE into the handshake and resets. Fully programmatic.
 *   2. The application has never proven itself.  It must write BOOT_HEALTHY into the
 *      handshake once the host has enumerated it; until some build has done so we hold here
 *      rather than hand over. This is the rule that keeps the device reachable no matter how
 *      early an application hangs.
 *   3. The application is not a valid image at all.  Erased or corrupt: we stay put.
 *   4. It is crash-looping — launched APP_FAULT_LIMIT times without ever reporting healthy.
 *   5. A host claims it during the brief window offered before a known-good app is launched.
 *
 * Only if none of those apply do we launch. The effect is that a bad application costs a
 * reboot, not a recovery cycle — and the SD-card route is reserved for the rare case of
 * replacing this bootloader itself.
 */

#include "s7g2.h"
#include "bsp.h"
#include "boot_handshake.h"
#include "app_launch.h"

#define APP_FAULT_LIMIT   3

/* How long a host gets to claim the device before we launch the application.
 *
 * Only offered to an application that has already proven itself, so this is a convenience
 * for interrupting a working system rather than the safety mechanism. The safety mechanism is
 * refusing to launch an unproven app at all — a window, however long, is a race, and we lost
 * that race repeatedly against an app that hung before USB came up. */
#define HOST_WINDOW_MS       1500

/* Reason we stayed, readable in a memory dump and reported over USB once it exists. */
volatile uint32_t g_boot_reason;

#define REASON_APP_REQUESTED  1
#define REASON_APP_INVALID    2
#define REASON_CRASH_LOOP     3
#define REASON_HOST_CLAIMED   4
#define REASON_UNPROVEN_APP   5
#define REASON_LAUNCHED       0

/* Placeholder until the USB device stack lands. Returning 0 means "no host claimed us",
 * so the bootloader behaves exactly as if the window had elapsed. */
__attribute__((weak)) int usb_wait_for_host(uint32_t timeout_ms)
{
    bsp_delay_ms(timeout_ms);
    return 0;
}

__attribute__((weak)) void usb_flash_service(void)
{
    /* The real implementation never returns: it services flash commands until the host
     * says RUN, then resets. Until it exists, fall through to the idle blink below. */
}

static void blink(unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        PORT_SET(7, 0); bsp_delay_ms(80);
        PORT_CLR(7, 0); bsp_delay_ms(140);
    }
    bsp_delay_ms(500);
}

int main(void)
{
    if (!boot_handshake_valid()) {
        boot_handshake_init();               /* cold boot: SRAM was undefined */
    }

    volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
    uint32_t request = h->request;

    h->boot_count++;
    h->request  = BOOT_REQ_NONE;             /* consume it, so it cannot latch us in forever */
    h->checksum = boot_handshake_checksum(h);

    /* 1 — the application asked to be updated. */
    if (request == BOOT_REQ_UPDATE) {
        g_boot_reason = REASON_APP_REQUESTED;
    }
    /* 2 — no launchable image. */
    else if (!app_image_valid()) {
        g_boot_reason = REASON_APP_INVALID;
    }
    /* 3 — the app has been launched repeatedly without ever reporting itself healthy. */
    else if (h->app_faults >= APP_FAULT_LIMIT) {
        g_boot_reason = REASON_CRASH_LOOP;
    }
    /* 4 — the application has never proven itself. Do NOT hand over: hold here and wait.
     *
     * This is the rule that makes the device always reachable. An app that hangs before it
     * can report health is exactly the app we must be able to displace, and any scheme that
     * launches it anyway — however long the window — is a race we can lose. Holding is not a
     * race. Once a healthy app has run, `health` persists across warm resets and normal boots
     * proceed at full speed. */
    else if (h->health != BOOT_HEALTHY) {
        g_boot_reason = REASON_UNPROVEN_APP;
    }
    /* 5 — known-good app: brief window for a host to intervene, then launch. */
    else if (usb_wait_for_host(HOST_WINDOW_MS)) {
        g_boot_reason = REASON_HOST_CLAIMED;
    }
    else {
        /* Count the launch. The application clears this once it is up; if it never does,
         * case 3 catches it on the third try. */
        h->app_faults++;
        h->checksum = boot_handshake_checksum(h);
        g_boot_reason = REASON_LAUNCHED;
        app_launch();                         /* never returns */
    }

    usb_flash_service();                      /* never returns, once implemented */

    /* Idle: blink the reason so the state is observable with nothing but a scope, and stay
     * reachable. Deliberately not a reset loop — a device that keeps resetting is much
     * harder to catch with a host than one that sits still and waits. */
    for (;;) {
        blink(g_boot_reason ? g_boot_reason : 1);
    }
}
