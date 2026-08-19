/* The bootloader's USB service: identify, receive a new application, launch it.
 *
 * This is what makes the device iterable. Once this image is in flash, replacing the
 * application never again requires the button chord, the SD card, or a human — the host
 * opens the COM port, says `erase`, streams the image, says `run`.
 *
 * Flash programming itself lives in flash_faci.c; this file is the transport and the
 * command surface. Keeping them apart matters because the FACI sequence is the dangerous
 * part and deserves to be readable on its own.
 */

#include "s7g2.h"
#include "bsp.h"
#include "usb_fs.h"
#include "console.h"
#include "boot_screen.h"
#include "boot_handshake.h"
#include "app_launch.h"
#include "flash_faci.h"

extern volatile uint32_t g_boot_reason;

/* Streaming state for `wr`. The host sends hex lines; we buffer a program-unit's worth and
 * commit it, because the FACI writes fixed-size units and a partial unit is not a thing. */
static uint32_t stream_addr;
static uint8_t  stream_buf[FLASH_WRITE_UNIT];
static uint32_t stream_fill;
static uint32_t stream_written;
static int      stream_open;

/* Button 5 is mux channel 4. Keep this tiny scanner local to the bootloader instead of linking
 * the application's ADC/rotation stack into the recovery image. It requires a stable release
 * before accepting a press, so the same hold that requested update mode cannot immediately
 * bounce straight back into the application. */
static int button5_pressed(void)
{
    static uint8_t last_raw;
    static uint8_t stable;
    static uint8_t count;
    static uint8_t primed;
    static uint8_t armed;

    PORT_CLR(9, 0);                  /* mux A0 = 0 */
    PORT_CLR(9, 1);                  /* mux A1 = 0 */
    PORT_SET(9, 5);                  /* mux A2 = 1: channel 4 */
    bsp_delay_us(2);
    uint8_t raw = (uint8_t)!PORT_GET(0, 4);   /* front buttons are active low */

    if (!primed) {
        last_raw = stable = raw;
        count = 1;
        primed = 1;
        armed = (uint8_t)!raw;       /* already released: the first press must act */
        return 0;
    }

    if (raw != last_raw) {
        last_raw = raw;
        count = 1;
        return 0;
    }
    if (count < 32u) count++;
    if (count != 32u || raw == stable) return 0;

    stable = raw;
    if (!stable) {
        armed = 1;                   /* a deliberate press must follow a stable release */
        return 0;
    }
    if (!armed) return 0;
    armed = 0;
    return 1;
}

static uint32_t parse_hex(const char **p)
{
    uint32_t v = 0;
    const char *s = *p;
    while (*s == ' ') s++;
    while (1) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
        s++;
    }
    *p = s;
    return v;
}

static void cmd_id(const char *a)
{
    (void)a;
    console_write("electra-mini-fw BOOTLOADER @0x00100000\r\n");
    console_hex("boot_reason", g_boot_reason);
    console_write("  1 app asked  2 app invalid  3 crash loop  4 host claimed\r\n");
    console_hex("app_valid",  (uint32_t)app_image_valid());
    console_hex("app_base",   APP_BASE_ADDR);
    console_hex("app_max",    APP_MAX_BYTES);
    if (boot_handshake_valid()) {
        console_hex("boot_count", BOOT_HANDSHAKE.boot_count);
        console_hex("app_faults", BOOT_HANDSHAKE.app_faults);
    }
}

static void cmd_erase(const char *a)
{
    (void)a;
    console_write("erasing application region...\r\n");
    int rc = flash_erase_app();
    console_hex("erase rc", (uint32_t)rc);
    stream_open = 0;
    if (rc == 0) console_write("ok\r\n");
}

/* `wr <hexaddr> <hexbytes...>` — offset is relative to the application base, so the host
 * never has to know where we put it. */
static void cmd_wr(const char *a)
{
    const char *p = a;
    uint32_t off = parse_hex(&p);

    if (!stream_open || off != stream_addr + stream_fill) {
        /* New or non-contiguous run: flush whatever is pending first. */
        if (stream_open && stream_fill) {
            while (stream_fill < FLASH_WRITE_UNIT) stream_buf[stream_fill++] = 0xFF;
            (void)flash_write_unit(APP_BASE_ADDR + stream_addr, stream_buf);
        }
        stream_addr = off;
        stream_fill = 0;
        stream_open = 1;
    }

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *q = p;
        uint32_t byte = parse_hex(&p);
        if (p == q) break;
        stream_buf[stream_fill++] = (uint8_t)byte;
        stream_written++;
        if (stream_fill == FLASH_WRITE_UNIT) {
            int rc = flash_write_unit(APP_BASE_ADDR + stream_addr, stream_buf);
            if (rc != 0) { console_hex("write FAILED at", stream_addr); stream_open = 0; return; }
            stream_addr += FLASH_WRITE_UNIT;
            stream_fill = 0;
        }
    }
    console_write("ok\r\n");
}

static void cmd_flush(const char *a)
{
    (void)a;
    if (stream_open && stream_fill) {
        while (stream_fill < FLASH_WRITE_UNIT) stream_buf[stream_fill++] = 0xFF;
        int rc = flash_write_unit(APP_BASE_ADDR + stream_addr, stream_buf);
        console_hex("flush rc", (uint32_t)rc);
        stream_addr += FLASH_WRITE_UNIT;
        stream_fill = 0;
    }
    console_hex("bytes written", stream_written);
}

/* Read back a CRC over the flashed region so the host can verify without trusting us. */
static void cmd_crc(const char *a)
{
    const char *p = a;
    uint32_t len = parse_hex(&p);
    if (!len) len = stream_written;
    console_hex("crc32", flash_crc32(APP_BASE_ADDR, len));
    console_hex("over_len", len);
}

static void cmd_run(const char *a)
{
    (void)a;
    if (!app_image_valid()) { console_write("no valid application image; refusing\r\n"); return; }
    console_write("launching application...\r\n");
    for (int i = 0; i < 64; i++) usb_poll();
    app_launch();
}

static void cmd_reset(const char *a)
{
    (void)a;
    console_write("resetting...\r\n");
    for (int i = 0; i < 64; i++) usb_poll();
    bsp_system_reset();
}

static const console_cmd_t boot_cmds[] = {
    { "help",  "list commands",                     console_help },
    { "id",    "identify + why we are here",        cmd_id       },
    { "erase", "erase the application region",      cmd_erase    },
    { "wr",    "wr <off> <bytes..>  write hex",     cmd_wr       },
    { "flush", "commit a partial write unit",       cmd_flush    },
    { "crc",   "crc <len>  checksum the image",     cmd_crc      },
    { "run",   "launch the application",            cmd_run      },
    { "reset", "warm reset",                        cmd_reset    },
};

/* Give a host a brief chance to claim the device before the application is launched.
 * Returns 1 if a host attached AND sent anything at all — mere enumeration is not enough,
 * because a machine that simply has the cable plugged in should not stop the app running. */
int usb_wait_for_host(uint32_t timeout_ms)
{
    usb_init();
    console_init(boot_cmds, sizeof(boot_cmds) / sizeof(boot_cmds[0]));

    int announced = 0;

    for (uint32_t ms = 0; ms < timeout_ms; ms++) {
        for (int i = 0; i < 20; i++) usb_poll();

        /* Announce ourselves as soon as the host has configured us, so a human watching a
         * terminal can see the window and type into it, and so a tool has something to
         * match on. Without this the window is invisible and can only be hit by luck. */
        if (usb_configured() && !announced) {
            announced = 1;
            console_write("\r\nEMB: press any key within the window to stay in the bootloader\r\n");
        }

        uint8_t probe[8];
        if (usb_configured() && usb_read(probe, sizeof(probe)) > 0) return 1;
        bsp_delay_us(1000);
    }
    return 0;
}

void usb_flash_service(void)
{
    /* usb_wait_for_host may not have run (e.g. we are here because the app image is
     * invalid), so make sure the stack is up either way. Re-initialising is harmless. */
    usb_init();
    console_init(boot_cmds, sizeof(boot_cmds) / sizeof(boot_cmds[0]));

    console_write("\r\nelectra-mini-fw bootloader ready\r\n> ");

    /* Last, deliberately. A device held in the bootloader used to show a black screen, which
     * looks exactly like a dead one -- see boot_screen.c. Drawn only once the console is
     * answering, so that a panel which fails to come up costs a caption and not the recovery. */
    boot_screen_show(g_boot_reason);

    for (;;) {
        usb_poll();
        console_poll();
        if (button5_pressed() && app_image_valid()) {
            console_write("button 5: launching application...\r\n");
            for (int i = 0; i < 64; i++) usb_poll();
            app_launch();
        }
    }
}
