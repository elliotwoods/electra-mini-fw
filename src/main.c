/* Application image. Runs at 0x00120000, launched by our bootloader.
 *
 * Two jobs right now:
 *   1. Bring up the display and prove it works (or report exactly how it does not).
 *   2. Speak USB, so we are never again reduced to guessing from a blank screen — and so the
 *      host can send us back to the bootloader without anyone touching the device.
 *
 * The `boot` command is the one that matters most. It writes BOOT_REQ_UPDATE into the
 * handshake region in SRAM and resets; the handshake survives the reset, our bootloader
 * reads it and stays in flash mode. That closes the loop: build, push, run, repeat, with the
 * device untouched on the bench.
 */

#include "s7g2.h"
#include "bsp.h"
#include "ra8876.h"
#include "spi.h"
#include "usb_fs.h"
#include "console.h"
#include "boot_handshake.h"
#include "app_launch.h"
#include "riic.h"
#include "qt2120.h"
#include "inputs.h"
#include "ui.h"
#include "ui_state.h"
#include "session.h"
#include "surface.h"
#include "text.h"

/* RGB565 */
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define YELLOW  0xFFE0
#define CYAN    0x07FF
#define MAGENTA 0xF81F

static volatile int32_t  g_sdram_status;
static volatile int32_t  g_lcd_init_rc;
static volatile int32_t  g_qt_init_rc;
static volatile uint32_t g_ticks;

/* Input and UI service cadence. 20 ms is fast enough that a touch feels immediate and a
 * release debounce measured in tens of milliseconds means what it says. */
#define SERVICE_PERIOD_MS 20u
static uint32_t g_next_service, g_service_count, g_service_hz, g_hz_mark;

/* Linker-provided stack bounds, reported by `id` so the monitor's window can be compared
 * against the real stack from the console instead of from a map file. */
extern uint32_t __stack_bottom_sym __asm__("__stack_bottom");
extern uint32_t __stack_top_sym    __asm__("__stack_top");
static uint32_t          g_unconfigured;   /* polls spent unconfigured; triggers re-attach */

/* Milliseconds since boot, from the Cortex-M4 cycle counter.
 *
 * The touch filter was being handed a hard-coded 50 ms per pass while the loop actually runs at
 * about 26 Hz, so every one of its timeouts was out by a quarter. A debounce measured in
 * guessed units is not a debounce. The counter wraps every ~18 s at 240 MHz, so the remainder
 * is carried rather than divided away. */
static volatile uint32_t g_systick_ms;

static uint32_t bsp_millis(void) { return g_systick_ms; }

/* Mirrors of what the UI is fed, for `uistate`. Diagnostics only. */
static uint16_t g_touch_filtered, g_moved_mask;
static uint32_t g_render_ms_max;      /* worst UI repaint seen; a view switch is the worst case */

/* ------------------------------------------------------------------ breadcrumbs */

/* The application's override of the weak no-op in startup.c. See boot_handshake.h for why
 * `app_faults` is the field: the bootloader is already in flash, cannot be replaced without
 * the SD card, and its `id` command prints that word verbatim. The channel had to be fitted
 * to the reporting side, not the other way round.
 *
 * Called before .bss is cleared and before .data is copied, so it must touch nothing but the
 * handshake itself — no statics, no library calls. */
void boot_stage(uint32_t code)
{
    volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
    if (!boot_handshake_valid()) return;      /* cold boot with no bootloader ahead of us */

    /* No health gate here any more.
     *
     * This used to fall silent once the app had reported healthy, on the theory that the crumb
     * and the crash counter share a word and a proven app must not be mistaken for a
     * crash-looping one. But the app ZEROES the counter the instant it proves healthy, so by
     * the time the bootloader reads it again the word is 0 unless the app died before getting
     * that far — which is precisely when the crumb is wanted. The gate bought nothing and cost
     * the diagnosis: a fault in a previously-good build reported a stale stage byte and pointed
     * nowhere at all. */

    h->app_faults = code;
    h->checksum   = boot_handshake_checksum(h);
}

/* A fault is recorded whatever the health flag says.
 *
 * The quiet rule above exists so a proven application is not mistaken for a crash-looping one.
 * But it was also silencing the crumb in the one situation where it matters most: an
 * application that used to work and has stopped. The stage byte then reported a stale value
 * and the diagnosis pointed nowhere. */
void boot_stage_force(uint32_t code)
{
    volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
    if (!boot_handshake_valid()) return;
    h->app_faults = code;
    h->checksum   = boot_handshake_checksum(h);
}


/* Ballast, to answer one question: does the failure follow the SIZE of .bss, or the test code?
 *
 * Adding the codec's test file (~12 KB of static buffers) reliably kills the application with an
 * NMI in its main loop, even though nothing ever calls it. This array reproduces the size with
 * no code attached at all. If it fails too, the test code is exonerated and the cause is purely
 * where things end up in memory; if it does not, the size is innocent and the code is not.
 *
 * Volatile, and touched once in main(), so the linker cannot garbage-collect the evidence. */
#ifndef BSS_BALLAST_BYTES
#define BSS_BALLAST_BYTES 0      /* set to 12288 to re-run the experiment */
#endif
#if BSS_BALLAST_BYTES > 0
volatile uint8_t g_bss_ballast[BSS_BALLAST_BYTES];
#endif

/* ------------------------------------------------------------------ watchdog */

/* A liveness watchdog on SysTick.
 *
 * The fault handlers cover exceptions, and the bounded NMI handler covers a storm — but
 * neither covers a plain infinite loop, and that is a real possibility in any driver poll. The
 * cost of missing it is a device that vanishes from USB and needs someone to unplug the cable.
 *
 * So the main loop bumps a counter, and SysTick checks that it is moving. If it stops for two
 * seconds, take the same route back to the bootloader as a fault does: crumb, update request,
 * reset. Any hang, of any kind, becomes a recoverable event that reports itself.
 *
 * SysTick is also the vehicle for the stray-tick absorber described in vectors.c, which must
 * keep working before the watchdog is armed — the stray tick arrives during the handoff, long
 * before this. Hence the armed flag rather than assuming ownership from reset. */
static volatile uint32_t g_loop_beat;
static volatile uint32_t g_clock_armed;      /* SysTick is ours and is counting milliseconds */
/* A MAGIC value, not a flag.
 *
 * A plain non-zero test let this reboot the device on a build where nothing had armed it: the
 * only assignment is in watchdog_arm(), reached only from `wdt on`, and the crumb still came
 * back saying the stall path had fired. Whatever put a stray value here -- stale SRAM read
 * before .bss is cleared, or a neighbour writing past its end -- a watchdog that can arm itself
 * by accident is worse than no watchdog, because it reboots a working device and takes the
 * evidence with it. This one fails closed. */
#define WATCHDOG_ARMED 0x5A5AC0DEu
static volatile uint32_t g_watchdog_armed;   /* AND a stalled main loop should reboot us */

/* The clock and the watchdog are separate, and that separation is the whole point.
 *
 * They used to be one flag. The liveness watchdog was disarmed at boot -- it had fired on a
 * build that otherwise looked healthy and the reason was never established -- and disarming it
 * silently stopped SysTick, so bsp_millis() returned zero forever.
 *
 * Everything timed then quietly stopped working while still appearing to run. The touch
 * filter's release counter advances by `dt_ms`, which was always zero, so a touch could never
 * be released: the panel latched into its drilled-down view and stayed there, digit highlights
 * worked exactly once each and never came back, and the release grace never expired. None of
 * that looked like a clock fault, which is why it survived several rounds of hunting through
 * the sensor and the UI instead.
 *
 * So: the clock starts at boot, unconditionally. The watchdog is a separate opt-in on top. */
void SysTick_Handler(void)
{
    static uint32_t last_beat, stalled_ms;

    if (!g_clock_armed) {
        /* Not ours yet: this is the tick inherited from the stock bootloader. */
        REG32(SYST_CSR) = 0;
        volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
        if (boot_handshake_valid()) {
            h->detail   = h->detail + 1U;
            h->checksum = boot_handshake_checksum(h);
        }
        return;
    }

    g_systick_ms++;              /* the firmware's only time base; see bsp_millis above */

    if (g_watchdog_armed != WATCHDOG_ARMED) return;

    if (g_loop_beat != last_beat) { last_beat = g_loop_beat; stalled_ms = 0; return; }

    extern void fault_to_bootloader(uint32_t ipsr);

    /* 0xFE, not 0xFF, and deliberately kept distinct.
     *
     * A stall reboot and a genuine exception land in the same crumb field, and 0xFF is a value
     * a confused IPSR read could plausibly produce -- so the two were indistinguishable. Moving
     * the sentinel one step is what identified this path as the culprit rather than a crash,
     * and turned an afternoon of guessing into one flash cycle. Any value outside the real
     * exception-number range works; the point is that it cannot be mistaken for one. */
    if (++stalled_ms > 2000u) fault_to_bootloader(0xFEu);
}

/* Long-running console commands must feed the watchdog.
 *
 * The screenshot reads three quarters of a megabyte out of the controller, which takes several
 * seconds inside a single command — during which the main loop is not turning, so the liveness
 * watchdog quite correctly concludes the firmware has hung and reboots into the bootloader.
 *
 * That is the watchdog doing exactly its job. Anything that legitimately blocks for longer than
 * the timeout has to say so, rather than the watchdog being loosened until it stops catching
 * the hangs it exists for. */
void watchdog_kick(void) { g_loop_beat++; }

/* Start the millisecond time base. Everything timed depends on it: the touch release debounce,
 * the focus release grace, the service cadence, the protocol's ping and heartbeat. */
static void clock_start(void)
{
    REG32(SYST_CSR) = 0;
    REG32(SYST_RVR) = 240000u - 1u;      /* 1 kHz at 240 MHz */
    REG32(SYST_CVR) = 0;
    g_clock_armed = 1;
    REG32(SYST_CSR) = 0x07u;             /* core clock, interrupt, enable */
}

static void watchdog_arm(void)
{
    if (!g_clock_armed) clock_start();
    g_watchdog_armed = WATCHDOG_ARMED;
}

/* ------------------------------------------------------------------ NMI */

/* An NMI must not be fatal, because a fatal one is unreportable.
 *
 * Adding ~12 KB of static test buffers made the application die with NMI in its main loop.
 * Stack alignment looked like the culprit — every working build had a 16-aligned stack and the
 * failing one did not — but fixing the alignment changed nothing, so that correlation was not
 * the cause. Rather than keep guessing at the reason from outside, this handler records what it
 * can see, disarms the source and returns, turning a device that vanishes from USB into one
 * that stays up and can be asked what happened.
 *
 * The Renesas stack-pointer monitor is the only NMI we arm ourselves, so disarming it is both
 * the way to stop an NMI storm and the test of whether it was responsible at all: if the app
 * then runs happily with a huge stack margin untouched, the monitor was firing spuriously. */
volatile uint32_t g_nmi_count;
volatile uint32_t g_nmi_msp;
volatile uint32_t g_nmi_mspmpu_ctl;
volatile uint32_t g_nmi_mspmpu_sa;
volatile uint32_t g_nmi_mspmpu_ea;

void NMI_Handler(void)
{
    /* Overrides the bounded absorber in vectors.c so the application can record more — but it
     * MUST keep the bound. The first version of this only recorded and returned, so when the
     * NMI turned out to re-fire regardless of disarming the monitor, the application stormed
     * forever and never reached the reboot path. It looked identical to the spin it replaced.
     * An override that drops a safety property is worse than no override. */
    uint32_t msp;
    __asm__ volatile("mrs %0, msp" : "=r"(msp));

    if (g_nmi_count == 0) {          /* keep the FIRST one; later ones are consequences */
        g_nmi_msp        = msp;
        g_nmi_mspmpu_ctl = REG32(MSPMPUCTL);
        g_nmi_mspmpu_sa  = REG32(MSPMPUSA);
        g_nmi_mspmpu_ea  = REG32(MSPMPUEA);
    }
    g_nmi_count++;

    /* Disarm, or we return straight back into it. */
    REG32(MSPMPUCTL) = 0;

    /* Report WHICH WAY the stack pointer left its window, in the spare byte of the breadcrumb.
     * "The stack monitor fired" is not a diagnosis: an overflow (below the start) and an
     * underflow (above the end) have entirely different causes, and that distinction is the
     * whole question here.
     *
     *   1 = MSP below the region start  — overflow, genuinely too much stack used
     *   2 = MSP above the region end    — MSP is not where the linker thinks it is
     *   3 = MSP inside the region       — the monitor fired without the pointer being outside
     */
    if (g_nmi_count > 8u) {
        uint32_t sa  = REG32(MSPMPUSA);
        uint32_t ea  = REG32(MSPMPUEA);
        uint32_t dir = (msp < sa) ? 1u : (msp > ea) ? 2u : 3u;

        uint32_t last = BOOT_HANDSHAKE.app_faults & 0xFFu;
        boot_stage(0x000000F0u | (2u << 8) | (last << 16) | (dir << 24));

        boot_handshake_set_request(BOOT_REQ_UPDATE, 0);
        __asm__ volatile("dsb" ::: "memory");
        REG32(0xE000ED0CUL) = (REG32(0xE000ED0CUL) & 0x0700UL) | 0x05FA0004UL;
        for (;;) { __asm__ volatile("wfi"); }
    }
}

/* ------------------------------------------------------------------ display */

static void test_pattern(void)
{
    static const uint16_t bars[8] = { WHITE, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, BLACK };

    ra8876_set_canvas(RA_PAGE(0));
    ra8876_set_display_page(RA_PAGE(0));

    ra8876_set_fg(BLACK);
    ra8876_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);

    const int16_t bar_w = LCD_WIDTH / 8;
    for (int i = 0; i < 8; i++) {
        ra8876_set_fg(bars[i]);
        ra8876_fill_rect((int16_t)(i * bar_w), 40, bar_w, 240);
    }

    /* One-pixel frame hard against the panel edge: a missing side means the active window
     * or the porch arithmetic is off by one. */
    ra8876_set_fg(WHITE);
    ra8876_fill_rect(0, 0, LCD_WIDTH, 1);
    ra8876_fill_rect(0, (int16_t)(LCD_HEIGHT - 1), LCD_WIDTH, 1);
    ra8876_fill_rect(0, 0, 1, LCD_HEIGHT);
    ra8876_fill_rect((int16_t)(LCD_WIDTH - 1), 0, 1, LCD_HEIGHT);

    /* Checker catches a stride error that solid fills would hide. */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            ra8876_set_fg(((x + y) & 1) ? WHITE : BLACK);
            ra8876_fill_rect((int16_t)(20 + x * 16), (int16_t)(320 + y * 16), 16, 16);
        }
    }

    /* Corner markers, so a rotated or mirrored panel is unmistakable. */
    ra8876_set_fg(RED);   ra8876_fill_rect(4, 4, 24, 8);
    ra8876_set_fg(GREEN); ra8876_fill_rect((int16_t)(LCD_WIDTH - 28), 4, 24, 8);
    ra8876_set_fg(BLUE);  ra8876_fill_rect(4, (int16_t)(LCD_HEIGHT - 12), 24, 8);
}

/* The display driver calls this from inside its wait loops. Servicing USB there is what
 * keeps the console alive even when the RA8876 is not answering — which is precisely the
 * situation we need the console for. */
void ra8876_yield(void) { usb_poll(); }

/* ------------------------------------------------------------------ commands */

static void cmd_id(const char *a)
{
    (void)a;
    console_write("electra-mini-fw APP @0x00120000\r\n");
    /* Non-zero proves the handoff really did release a stray SysTick into our vector table —
     * the thing that stopped every previous build before its first instruction. */
    console_hex("stray_systicks", boot_handshake_valid() ? BOOT_HANDSHAKE.detail : 0xFFFFFFFFu);
    console_hex("sdram_selftest", (uint32_t)g_sdram_status);
    console_hex("ra8876_init_rc", (uint32_t)g_lcd_init_rc);
    console_hex("ra_status_bits", g_ra_status);
    console_hex("qt2120_init_rc", (uint32_t)g_qt_init_rc);
    console_hex("usb_tx_dropped", g_usb_tx_dropped);
    console_hex("service_hz", g_service_hz);
    console_hex("render_ms_max", g_render_ms_max);
    console_hex("nmi_count", g_nmi_count);
    if (g_nmi_count) {
        console_hex("  nmi msp", g_nmi_msp);
        console_hex("  mspmpu ctl", g_nmi_mspmpu_ctl);
        console_hex("  mspmpu sa", g_nmi_mspmpu_sa);
        console_hex("  mspmpu ea", g_nmi_mspmpu_ea);
    }
    console_hex("stack_bottom", (uint32_t)&__stack_bottom_sym);
    console_hex("stack_top", (uint32_t)&__stack_top_sym);
    console_write("  b0 reset poll  b1 pll poll  b2 sdram poll  b3 status reads stuck\r\n");
}

/* The whole point: get back to the bootloader without touching the device. */
static void cmd_boot(const char *a)
{
    (void)a;
    console_write("rebooting into bootloader for update...\r\n");
    for (int i = 0; i < 64; i++) usb_poll();      /* let the reply drain before we reset */
    boot_handshake_set_request(BOOT_REQ_UPDATE, 0);
    bsp_system_reset();
}

static void cmd_lcd(const char *a)
{
    if (*a == 'o' && a[1] == 'n')  { ra8876_display_on(1); console_write("display on\r\n");  return; }
    if (*a == 'o' && a[1] == 'f')  { ra8876_display_on(0); console_write("display off\r\n"); return; }
    if (*a == 'r') { ra8876_set_fg(RED);   ra8876_fill_rect(0,0,LCD_WIDTH,LCD_HEIGHT); console_write("red\r\n");   return; }
    if (*a == 'g') { ra8876_set_fg(GREEN); ra8876_fill_rect(0,0,LCD_WIDTH,LCD_HEIGHT); console_write("green\r\n"); return; }
    if (*a == 'b') { ra8876_set_fg(BLUE);  ra8876_fill_rect(0,0,LCD_WIDTH,LCD_HEIGHT); console_write("blue\r\n");  return; }
    if (*a == 't') { test_pattern(); console_write("test pattern\r\n"); return; }
    console_write("usage: lcd on|off|r|g|b|t\r\n");
}

/* Read any RA8876 register back. This is the tool that will actually settle whether the SPI
 * read path works — the thing we could not determine with no comms. */
static void cmd_rd(const char *a)
{
    uint32_t reg = 0;
    for (const char *p = a; *p; p++) {
        if (*p >= '0' && *p <= '9') reg = reg * 16 + (uint32_t)(*p - '0');
        else if (*p >= 'a' && *p <= 'f') reg = reg * 16 + (uint32_t)(*p - 'a' + 10);
        else if (*p >= 'A' && *p <= 'F') reg = reg * 16 + (uint32_t)(*p - 'A' + 10);
        else break;
    }
    extern uint8_t ra_read_reg(uint8_t);
    extern uint8_t ra_read_status(void);
    console_hex("reg", reg);
    console_hex("  value", ra_read_reg((uint8_t)reg));
    console_hex("  status", ra_read_status());
}

static void cmd_bl(const char *a)
{
    uint32_t v = 0;
    for (const char *p = a; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
    if (!v) v = 0x0800;
    ra8876_backlight((uint16_t)v);
    console_hex("backlight", v);
}

/* Deliberately a command, not a startup action: it writes to external SDRAM, which is a bus
 * fault away from a silent hang if the external bus is not up. */
static void cmd_sdram(const char *a)
{
    (void)a;
    console_write("writing/reading 0x90000000 ... if this is the last thing you see, it faulted\r\n");
    for (int i = 0; i < 40; i++) usb_poll();
    g_sdram_status = bsp_sdram_selftest();
    console_hex("sdram_selftest", (uint32_t)g_sdram_status);
}

/* ------------------------------------------------------------------ memory access */

/* peek/poke exist because every display question so far has been answered by guessing, and
 * guessing has been wrong every time. Being able to read a PFS register, an SPI status word
 * or a port direction register directly turns "the display does not respond" into a question
 * with an answer, without a 22-second reflash between each hypothesis.
 *
 * Width matters: peripheral registers here are 8, 16 and 32 bits and several of them fault or
 * read rubbish if accessed at the wrong width, so the width is explicit rather than assumed. */
static uint32_t arg_hex(const char **p)
{
    uint32_t v = 0;
    const char *s = *p;
    while (*s == ' ') s++;
    while (1) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
        s++;
    }
    *p = s;
    return v;
}

/* peek <b|h|w> <hexaddr> [hexcount] */
static void cmd_peek(const char *a)
{
    const char *p = a;
    while (*p == ' ') p++;
    char width = *p ? *p++ : 'w';

    uint32_t addr = arg_hex(&p);
    uint32_t n    = arg_hex(&p);
    if (!n) n = 1;
    if (n > 32) n = 32;

    uint32_t step = (width == 'b') ? 1U : (width == 'h') ? 2U : 4U;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t at = addr + i * step;
        uint32_t v  = (width == 'b') ? (uint32_t)REG8(at)
                    : (width == 'h') ? (uint32_t)REG16(at)
                                     : REG32(at);
        console_hex("", at);
        console_hex("  =", v);
    }
}

/* poke <b|h|w> <hexaddr> <hexvalue> */
static void cmd_poke(const char *a)
{
    const char *p = a;
    while (*p == ' ') p++;
    char width = *p ? *p++ : 'w';

    uint32_t addr = arg_hex(&p);
    uint32_t val  = arg_hex(&p);

    if      (width == 'b') REG8(addr)  = (uint8_t)val;
    else if (width == 'h') REG16(addr) = (uint16_t)val;
    else                   REG32(addr) = val;

    console_hex("wrote", val);
    console_hex("  to", addr);
}

/* pfs <port> — dump all 16 PmnPFS registers for a port, so pin muxing can be checked against
 * docs/pinmap.txt rather than trusted. */
static void cmd_pfs(const char *a)
{
    const char *p = a;
    uint32_t port = arg_hex(&p);
    for (uint32_t i = 0; i < 16; i++) {
        console_hex("pin", i);
        console_hex("  pfs", REG32(PFS(port, i)));
    }
    console_hex("PCNTR1", REG32(PORT_PCNTR1(port)));
    console_hex("PIDR  ", REG32(PORT_PCNTR2(port)));
}

/* wr8 <reg> <val> — write any RA8876 register.
 *
 * The counterpart to `rd`. Between them the whole controller is reachable from the host, so a
 * timing or PLL theory can be tested in a second rather than by rebuilding and reflashing. */
static void cmd_wr8(const char *a)
{
    const char *p = a;
    uint32_t reg = arg_hex(&p);
    uint32_t val = arg_hex(&p);
    extern void ra_write_reg8(uint8_t, uint8_t);
    ra_write_reg8((uint8_t)reg, (uint8_t)val);
    console_hex("wrote reg", reg);
    console_hex("  value", val);
}

/* pix <x> <y> [count] — read pixels back out of the controller's VRAM.
 *
 * This is the question no register can answer: whether the draw engine is actually putting
 * colour into memory. A screen that is black because nothing was drawn and a screen that is
 * black because it is not being scanned out look identical from the outside, and they need
 * opposite fixes. Sets the graphic cursor, then reads MRWDP, which auto-increments. */
static void cmd_pix(const char *a)
{
    const char *p = a;
    uint32_t x = arg_hex(&p);
    uint32_t y = arg_hex(&p);
    uint32_t n = arg_hex(&p);
    if (!n) n = 4;
    if (n > 16) n = 16;

    extern void    ra_write_reg16(uint8_t, uint16_t);
    extern uint8_t ra_read_reg(uint8_t);
    extern uint8_t ra_read_data(void);
    extern void    ra_cmd(uint8_t);

    ra_write_reg16(0x5F, (uint16_t)x);       /* CURH */
    ra_write_reg16(0x61, (uint16_t)y);       /* CURV */

    /* MRWDP auto-increments, so select it once and keep pulling data. The first read after
     * setting the cursor is a dummy on this controller — the pipeline is one deep. */
    ra_cmd(0x04);
    (void)ra_read_data();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t lo = ra_read_data();
        uint32_t hi = ra_read_data();
        console_hex("px", (hi << 8) | lo);
    }
}

/* ------------------------------------------------------------------ i2c */

/* i2c <ch> — scan a bus.
 *
 * Self-verifying in a way almost nothing else here is: if 0x1C answers on channel 0 and 0x38
 * on channel 2, then the base address, the module-stop bit, the bit-rate divisors, the pin
 * muxing and the start/stop sequencing are ALL correct, and the two expected devices are the
 * ones prior analysis said they would be. If nothing answers, none of that is established
 * and there is no point writing a device driver yet. */
static void cmd_i2c(const char *a)
{
    const char *p = a;
    uint32_t ch = arg_hex(&p);
    if (ch > 2) { console_write("usage: i2c <0|1|2>\r\n"); return; }

    riic_init(ch);
    console_hex("iccr1 before (b1 SCL b0 SDA)", REG8(RIIC_ICCR1(ch)));
    riic_bus_recover(ch);
    console_hex("iccr1 after ", REG8(RIIC_ICCR1(ch)));

    /* Refuse to scan a bus that is still held low, rather than grinding through 112 addresses
     * that each have to time out twice. That is not just slow — it is silent for long enough
     * to look like a hang, which is the one thing this console exists to prevent. */
    if (!(REG8(RIIC_ICCR1(ch)) & ICCR1_SDAI)) {
        console_write("SDA still held low after recovery; a slave is stuck. Not scanning.\r\n");
        return;
    }
    if (!(REG8(RIIC_ICCR1(ch)) & ICCR1_SCLI)) {
        console_write("SCL held low; check the pull-up and the mux. Not scanning.\r\n");
        return;
    }

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (riic_probe(ch, addr) == RIIC_OK) {
            console_hex("device at", addr);
            found++;
        }
        usb_poll();
    }
    console_hex("devices found", (uint32_t)found);
}

/* ---- software I2C, for deciding whether the RIIC peripheral is the problem -------------
 *
 * The RIIC master reports a NACK from every address on every channel, with both lines idle
 * high. That has two very different explanations — the peripheral is misconfigured and never
 * really drives the pads, or the devices are genuinely not answering — and no register on the
 * MCU side can tell them apart, because an undriven bus and an unanswered bus look identical
 * from the master.
 *
 * Bit-banging the same two pads settles it. If a device ACKs here, the RIIC setup is wrong; if
 * nothing ACKs here either, the devices are absent, unpowered or held in reset, and no amount
 * of work on the RIIC driver will help.
 *
 * Pins are given in the pinmap's own notation, hex pin number: `bb 400 401` is P400/P401.
 * Open-drain is emulated the only correct way — drive low, release to input and let the
 * pull-up do the rest. Never drive high: two masters or a stretching slave would be shorted.
 */
static void bb_release(uint32_t pp) { bsp_pin_cfg(pp >> 8, pp & 0xFF, 0); }
static void bb_low(uint32_t pp)     { bsp_pin_cfg(pp >> 8, pp & 0xFF, PFS_PDR); }
static int  bb_read(uint32_t pp)    { return (int)PORT_GET(pp >> 8, pp & 0xFF); }

#define BB_T 5   /* microseconds per half bit -> ~100 kHz, slow and forgiving */

static void bb_start(uint32_t scl, uint32_t sda)
{
    bb_release(sda); bb_release(scl); bsp_delay_us(BB_T);
    bb_low(sda);     bsp_delay_us(BB_T);
    bb_low(scl);     bsp_delay_us(BB_T);
}

static void bb_stop(uint32_t scl, uint32_t sda)
{
    bb_low(sda);     bsp_delay_us(BB_T);
    bb_release(scl); bsp_delay_us(BB_T);
    bb_release(sda); bsp_delay_us(BB_T);
}

/* Returns 1 if the slave pulled SDA low for the ACK bit. */
static int bb_write_byte(uint32_t scl, uint32_t sda, uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        if (v & (1U << i)) bb_release(sda); else bb_low(sda);
        bsp_delay_us(BB_T);
        bb_release(scl);
        /* Honour clock stretching: a slave may hold SCL low until it is ready. */
        for (int t = 0; t < 1000 && !bb_read(scl); t++) bsp_delay_us(1);
        bsp_delay_us(BB_T);
        bb_low(scl);
        bsp_delay_us(BB_T);
    }

    bb_release(sda);                       /* let the slave answer */
    bsp_delay_us(BB_T);
    bb_release(scl);
    bsp_delay_us(BB_T);
    int ack = !bb_read(sda);
    bb_low(scl);
    bsp_delay_us(BB_T);
    return ack;
}

/* bb <scl> <sda> — bit-banged bus scan on any two pads. */
static void cmd_bb(const char *a)
{
    const char *p = a;
    uint32_t scl = arg_hex(&p);
    uint32_t sda = arg_hex(&p);
    if (!scl || !sda) { console_write("usage: bb <sclpin> <sdapin>, e.g. bb 400 401\r\n"); return; }

    /* Save the pin configuration and put it back at the end. Leaving these pads as GPIO takes
     * them away from the RIIC peripheral, so every later RIIC transfer times out — which looks
     * exactly like the peripheral being broken, and briefly convinced me it was. */
    uint32_t saved_scl = REG32(PFS(scl >> 8, scl & 0xFF));
    uint32_t saved_sda = REG32(PFS(sda >> 8, sda & 0xFF));

    bb_release(scl);
    bb_release(sda);
    bsp_delay_us(100);

    /* Idle levels first. Both must read high, or there are no pull-ups on these pads and
     * nothing else is worth trying — that alone would explain everything. */
    console_hex("idle scl", (uint32_t)bb_read(scl));
    console_hex("idle sda", (uint32_t)bb_read(sda));

    uint32_t hits[8];
    int found = 0;
    for (uint32_t addr = 0x08; addr < 0x78; addr++) {
        bb_start(scl, sda);
        int ack = bb_write_byte(scl, sda, (uint8_t)(addr << 1));
        bb_stop(scl, sda);
        if (ack) {
            if (found < 8) hits[found] = addr;
            found++;
        }
        usb_poll();
    }

    /* A count in the dozens is not a bus full of devices, it is SDA never returning high —
     * a missing or unpowered pull-up reads as an ACK from every address. Print the recovered
     * line level so the two are never confused again. */
    bb_release(scl);
    bb_release(sda);
    bsp_delay_us(100);
    console_hex("after scl", (uint32_t)bb_read(scl));
    console_hex("after sda", (uint32_t)bb_read(sda));
    console_hex("devices found", (uint32_t)found);
    if (found <= 8) {
        for (int i = 0; i < found; i++) console_hex("  ACK from", hits[i]);
    } else {
        console_write("  too many to be real: SDA is not recovering\r\n");
    }

    bsp_pin_cfg(scl >> 8, scl & 0xFF, saved_scl);
    bsp_pin_cfg(sda >> 8, sda & 0xFF, saved_sda);
}

/* qtrst — pulse P402, the suspected AT42QT2120 reset.
 *
 * The sensor did not answer at all until a sweep happened to drive P402 low and back high,
 * after which it answered every time. That is suggestive rather than conclusive — the chip
 * might simply have needed time — so this exists to test it deliberately: scan, pulse, scan. */
static void cmd_qtrst(const char *a)
{
    (void)a;
    qt2120_hw_reset();
    console_write("pulsed P402 low 20ms, high, settled 150ms\r\n");
}

/* touch — watch the pot-touch sensor and report every change.
 *
 * Prints only on change rather than streaming, so the output is a record of what was touched
 * rather than a wall of identical lines. Bit p is pot p in PANEL order; the sensor's own key
 * numbering is scrambled relative to the panel and the driver undoes it. */
static void cmd_touch(const char *a)
{
    uint32_t secs = arg_hex(&a);
    if (!secs) secs = 0x10;

    uint16_t last = 0xFFFF;
    console_write("watching pot touch; bit0 = pot 1 ... bit7 = pot 8\r\n");

    for (uint32_t i = 0; i < secs * 100U; i++) {
        uint16_t mask = 0;
        int rc = qt2120_read_touch(&mask);
        if (rc != RIIC_OK) { console_hex("read failed rc", (uint32_t)rc); return; }
        if (mask != last) { console_hex("pots", mask); last = mask; }
        for (int k = 0; k < 40; k++) usb_poll();
        bsp_delay_ms(8);
    }
    console_write("done\r\n");
}

/* gpio <pin> <0|1> — drive any pad, for finding a device's reset or power-enable line. */
static void cmd_gpio(const char *a)
{
    const char *p = a;
    uint32_t pp = arg_hex(&p);
    uint32_t v  = arg_hex(&p);
    bsp_pin_cfg(pp >> 8, pp & 0xFF, v ? (PFS_PDR | PFS_PODR) : PFS_PDR);
    console_hex("drove pin", pp);
    console_hex("  to", v);
}

/* i2crd <ch> <addr> <reg> <count> — read a device's registers. */
static void cmd_i2crd(const char *a)
{
    const char *p = a;
    uint32_t ch   = arg_hex(&p);
    uint32_t addr = arg_hex(&p);
    uint32_t reg  = arg_hex(&p);
    uint32_t n    = arg_hex(&p);
    if (!n) n = 1;
    if (n > 32) n = 32;

    uint8_t buf[32];
    int rc = riic_read_regs(ch, (uint8_t)addr, (uint8_t)reg, buf, n);
    console_hex("rc", (uint32_t)rc);
    if (rc != RIIC_OK) {
        console_write("  -1 busy  -2 nack  -3 timeout\r\n");
        console_hex("  died at stage", g_riic_fail);
        console_hex("  icsr2", g_riic_icsr2);
        console_hex("  iccr2", g_riic_iccr2);
        console_hex("  icmr3", g_riic_icmr3);
        console_write("  1 bbsy 2 addrTDRE 3 addrACK 4 txTDRE 5 txTEND"
                      " 6 rsTDRE 7 rsACK 8 rxRDRF 9 rxSTOP 10 stop\r\n");
        return;
    }
    for (uint32_t i = 0; i < n; i++) console_hex("  ", buf[i]);
}

/* i2craw <ch> <addr> <n> — plain read with no register pointer and no repeated START.
 * If this works when i2crd does not, the fault is specifically the restart. */
static void cmd_i2craw(const char *a)
{
    const char *p = a;
    uint32_t ch   = arg_hex(&p);
    uint32_t addr = arg_hex(&p);
    uint32_t n    = arg_hex(&p);
    if (!n) n = 1;
    if (n > 32) n = 32;

    uint8_t buf[32];
    int rc = riic_read(ch, (uint8_t)addr, buf, n);
    console_hex("rc", (uint32_t)rc);
    console_hex("  stage", g_riic_fail);
    if (rc != RIIC_OK) return;
    for (uint32_t i = 0; i < n; i++) console_hex("  ", buf[i]);
}

/* i2cwr <ch> <addr> <reg> <val> */
static void cmd_i2cwr(const char *a)
{
    const char *p = a;
    uint32_t ch   = arg_hex(&p);
    uint32_t addr = arg_hex(&p);
    uint32_t reg  = arg_hex(&p);
    uint32_t val  = arg_hex(&p);
    console_hex("rc", (uint32_t)riic_write_reg(ch, (uint8_t)addr, (uint8_t)reg, (uint8_t)val));
}


/* ------------------------------------------------------------------ inputs */

/* adcscan — convert every channel of both ADC units, for all 8 mux positions.
 *
 * Prior analysis says the two pot phases are S12AD1 channel 5 and S12AD0 channel 6, but
 * which phase sits on which pad was an open question, and an inferred channel number is
 * exactly the sort of thing that is wrong in a way nothing detects. Turning a knob while this
 * runs answers it by measurement: the channels that move are the channels that matter. */
static void cmd_adcscan(const char *a)
{
    (void)a;
    for (unsigned ch = 0; ch < INPUT_POTS; ch++) {
        inputs_mux_select(ch);
        console_hex("mux", ch);
        for (unsigned unit = 0; unit < 2; unit++) {
            for (unsigned c = 0; c < 8; c++) {
                uint16_t v = inputs_adc_raw(unit, c);
                /* Only report channels that are not pinned at the rails; an unconnected
                 * analog input reads as noise near zero and would bury the real ones. */
                if (v > 0x040 && v < 0xFC0) {
                    console_hex(unit ? "  ad1 ch" : "  ad0 ch", c);
                    console_hex("    value", v);
                }
            }
        }
        usb_poll();
    }
}

/* pots [secs] — stream both pot phases for every mux channel, reporting only real movement.
 * Turning one knob should move exactly one channel's pair. */
static void cmd_pots(const char *a)
{
    uint32_t secs = arg_hex(&a);
    if (!secs) secs = 0x14;

    uint16_t last_a[INPUT_POTS] = {0}, last_b[INPUT_POTS] = {0};
    for (unsigned ch = 0; ch < INPUT_POTS; ch++) {
        inputs_mux_select(ch);
        inputs_read_phases(&last_a[ch], &last_b[ch]);
    }
    console_write("turn a knob; reporting phase pairs that move\r\n");

    for (uint32_t i = 0; i < secs * 80U; i++) {
        for (unsigned ch = 0; ch < INPUT_POTS; ch++) {
            uint16_t va, vb;
            inputs_mux_select(ch);
            inputs_read_phases(&va, &vb);

            /* A deadband of 16 counts, as the stock decode uses. Below that it is noise, and
             * printing noise would make a still panel look like a moving one. */
            int da = (int)va - (int)last_a[ch];
            int db = (int)vb - (int)last_b[ch];
            if (da < 0) da = -da;
            if (db < 0) db = -db;
            if (da > 16 || db > 16) {
                console_hex("mux ch", ch);
                console_hex("  A", va);
                console_hex("  B", vb);
                last_a[ch] = va;
                last_b[ch] = vb;
            }
        }
        usb_poll();
        bsp_delay_ms(10);
    }
    console_write("done\r\n");
}

/* btns [secs] — the two digital sense lines across all 8 mux positions.
 * Bank on P004 carries the six front-panel buttons; bank on P003 the eight pot pushes. */
static void cmd_btns(const char *a)
{
    uint32_t secs = arg_hex(&a);
    if (!secs) secs = 0x14;

    uint32_t last = 0xFFFFFFFFu;
    console_write("press buttons and pot switches; low 8 bits = pot pushes, next 8 = buttons\r\n");

    for (uint32_t i = 0; i < secs * 80U; i++) {
        uint32_t state = 0;
        for (unsigned ch = 0; ch < INPUT_POTS; ch++) {
            int sw, bt;
            inputs_mux_select(ch);
            inputs_read_senses(&sw, &bt);
            if (sw) state |= (1UL << ch);
            if (bt) state |= (1UL << (8 + ch));
        }
        if (state != last) { console_hex("senses", state); last = state; }
        usb_poll();
        bsp_delay_ms(10);
    }
    console_write("done\r\n");
}

/* mux <ch> — park the multiplexer, so a meter or another command sees a stable channel. */
static void cmd_mux(const char *a)
{
    uint32_t ch = arg_hex(&a) & 7U;
    inputs_mux_select(ch);
    uint16_t va, vb;
    inputs_read_phases(&va, &vb);
    int sw, bt;
    inputs_read_senses(&sw, &bt);
    console_hex("mux ch", ch);
    console_hex("  A", va);
    console_hex("  B", vb);
    console_hex("  potsw", (uint32_t)sw);
    console_hex("  button", (uint32_t)bt);
}


/* ------------------------------------------------------------------ watch */

/* Compact hex, because console_hex prints a label and a full 32-bit word per line and this
 * command emits a line per event. At 8 mux channels polled continuously that verbosity is the
 * difference between a readable log and one that outruns the USB transmit path. */
static void put_nib(char **d, uint32_t v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) *(*d)++ = hex[(v >> (4 * i)) & 0xF];
}

/* watch — stream every input as it changes, until a key is pressed.
 *
 * Deliberately not a timed sequence. Whoever is at the device works through the checks at
 * their own pace and in any order; the event tag says which input moved, so nothing depends
 * on doing the right thing at the right moment.
 *
 *   T <mask>          pot touch, bit 0 = pot 1, in PANEL order
 *   P <ch> <A> <B>    pot phase pair moved on mux channel ch
 *   S <state>         sense lines: low 8 = pot pushes, next 6 = front-panel buttons
 */
static void cmd_watch(const char *a)
{
    (void)a;

    uint16_t t_last = 0;
    uint32_t s_last = 0;
    uint16_t a_last[INPUT_POTS], b_last[INPUT_POTS];
    int have_baseline = 0;

    console_write("watching all inputs, PANEL order. Press Enter to stop.\r\n");

    for (uint32_t iter = 0; iter < 3000000UL; iter++) {
        char line[48];
        char *d;

        /* Stop on any host input, so the session ends when the operator is done rather than
         * when a timer says so. */
        uint8_t probe[4];
        if (usb_read(probe, sizeof(probe)) > 0) break;

        /* --- capacitive touch --- */
        uint16_t tmask;
        if (qt2120_read_touch(&tmask) == RIIC_OK && tmask != t_last) {
            d = line;
            *d++ = 'T'; *d++ = ' ';
            put_nib(&d, tmask, 2);
            *d++ = 13; *d++ = 10; *d = 0;
            console_write(line);
            t_last = tmask;
        }

        /* --- pots and senses, one pass over the multiplexer --- */
        uint32_t senses = 0;
        for (unsigned ch = 0; ch < INPUT_POTS; ch++) {
            inputs_mux_select(ch);

            uint16_t va, vb;
            inputs_read_phases(&va, &vb);

            /* Report in PANEL order, so bit 0 is the bottom-left knob rather than whichever
             * knob happens to sit on multiplexer channel 0. Everything above the HAL should
             * only ever see panel numbering. */
            int sw, bt;
            inputs_read_senses(&sw, &bt);
            if (!sw) senses |= (1UL << inputs_mux_to_pot(ch));   /* switches are active low */
            if (!bt) senses |= (1UL << (8 + ch));

            if (!have_baseline) { a_last[ch] = va; b_last[ch] = vb; continue; }

            int da = (int)va - (int)a_last[ch];
            int db = (int)vb - (int)b_last[ch];
            if (da < 0) da = -da;
            if (db < 0) db = -db;

            /* 16 counts of deadband, as the stock decode uses. Below that it is noise, and
             * reporting noise would make a still panel look alive. */
            if (da > 16 || db > 16) {
                d = line;
                *d++ = 'P'; *d++ = ' ';
                put_nib(&d, inputs_mux_to_pot(ch), 1);
                *d++ = ' ';
                put_nib(&d, va, 3);
                *d++ = ' ';
                put_nib(&d, vb, 3);
                *d++ = 13; *d++ = 10; *d = 0;
                console_write(line);
                a_last[ch] = va;
                b_last[ch] = vb;
            }
        }
        have_baseline = 1;

        if (senses != s_last) {
            d = line;
            *d++ = 'S'; *d++ = ' ';
            put_nib(&d, senses, 4);
            *d++ = 13; *d++ = 10; *d = 0;
            console_write(line);
            s_last = senses;
        }

        usb_poll();
        bsp_delay_ms(8);
    }

    console_write("stopped\r\n");
}




/* ------------------------------------------------------------------ M4: transport */

/* bench <KiB, hex> — stream a known pattern as fast as the endpoint will take it.
 *
 * This is the M4 measurement, and it is worth taking on the device rather than from a spec
 * sheet. The port is USB Full Speed — the stock descriptor declares bcdUSB 0x0110 — so the
 * ceiling is 12 Mbit/s: roughly 1 MB/s for bulk against a hard 64 KB/s for HID, because an
 * interrupt endpoint is polled once per bInterval however large the report is.
 *
 * Our CDC endpoints are already bulk, so this measures the bulk path itself: the same transfer
 * type a vendor-specific interface would use, through our own stack including its buffering.
 * Whatever comes out is the real budget the protocol has to live within.
 *
 * The payload is a repeating counter so the host can check for dropped bytes as well as time
 * the transfer — throughput that is only fast because data went missing is not throughput. */
static void cmd_bench(const char *a)
{
    uint32_t kib = arg_hex(&a);
    if (!kib) kib = 0x40;
    if (kib > 0x200) kib = 0x200;

    uint8_t block[64];
    for (uint32_t i = 0; i < sizeof(block); i++) block[i] = (uint8_t)('0' + (i % 10));
    block[sizeof(block) - 2] = 13;
    block[sizeof(block) - 1] = 10;

    uint32_t before = g_usb_tx_dropped;
    console_hex("bench bytes", kib * 1024u);

    uint32_t blocks = (kib * 1024u) / sizeof(block);
    for (uint32_t i = 0; i < blocks; i++) { usb_write_block(block, sizeof(block)); watchdog_kick(); }

    console_write("\r\nbench done\r\n");
    console_hex("dropped", g_usb_tx_dropped - before);
}


/* settle <us hex> [discards hex] — tune the multiplexer settling from the bench.
 *
 * Turning one knob was moving the next channel's reading as well as its own. Rather than pick
 * a delay that "looks safe", this makes the trade-off measurable: raise it until the crosstalk
 * disappears, then lower it until it returns, and keep the number with margin. The scan rate is
 * the thing being traded away — 8 channels at this delay each, plus the discarded conversions. */
static void cmd_settle(const char *a)
{
    const char *p = a;
    uint32_t us = arg_hex(&p);
    uint32_t d  = arg_hex(&p);
    if (us) inputs_set_settle(us, d);
    console_hex("settle us", inputs_get_settle_us());
    console_hex("discards", inputs_get_discards());
}


/* hexdump <addr> <bytes> — compact, 16 bytes a line.
 *
 * `peek` prints two lines per value, which is fine for a register but useless for finding a
 * pattern in a peripheral's whole address space. */
static void cmd_hexdump(const char *a)
{
    const char *p = a;
    uint32_t addr = arg_hex(&p);
    uint32_t n    = arg_hex(&p);
    if (!n) n = 64;
    if (n > 256) n = 256;

    for (uint32_t i = 0; i < n; i += 16) {
        char line[80];
        char *d = line;
        put_nib(&d, addr + i, 8);
        *d++ = ':';
        for (uint32_t j = 0; j < 16 && i + j < n; j++) {
            *d++ = ' ';
            put_nib(&d, REG8(addr + i + j), 2);
        }
        *d++ = 13; *d++ = 10; *d = 0;
        console_write(line);
        usb_poll();
    }
}

/* adctime <n> — cycle-accurate cost of n pot conversions.
 *
 * Uses the Cortex-M4 cycle counter, so it measures the ADC rather than a delay loop. Two uses:
 * it says what an input scan actually costs, and it identifies the sampling-time register by
 * experiment — poke a candidate, re-run, and if the conversion gets longer that is the one.
 * That beats guessing an offset out of a datasheet we do not have. */
static void cmd_adctime(const char *a)
{
    uint32_t n = arg_hex(&a);
    if (!n) n = 0x100;

    /* DWT cycle counter: enable the trace block, then the counter itself. */
    REG32(0xE000EDFCUL) = REG32(0xE000EDFCUL) | (1UL << 24);   /* DEMCR.TRCENA */
    REG32(0xE0001000UL) = REG32(0xE0001000UL) | 1UL;           /* DWT_CTRL.CYCCNTENA */

    uint32_t t0 = REG32(0xE0001004UL);
    for (uint32_t i = 0; i < n; i++) {
        (void)inputs_adc_raw(1, 5);
        (void)inputs_adc_raw(0, 6);
    }
    uint32_t t1 = REG32(0xE0001004UL);

    console_hex("pairs", n);
    console_hex("cycles", t1 - t0);
    console_hex("cycles per pair", (t1 - t0) / (n ? n : 1));
}


/* xtalk [reps] — measure channel-to-channel contamination, with nobody touching the device.
 *
 * The insight that makes this automatic: the pots sit at different, fixed voltages. So if a
 * channel's reading depends on WHICH channel was selected before it, that dependence is
 * contamination, and it can be measured by simply varying the predecessor. Nothing has to move.
 *
 * For each target channel, read it once after each of the eight possible predecessors and
 * report the spread. Zero spread means the sample-and-hold is fully charging to the new
 * channel. A noise floor is measured alongside, by repeating with the predecessor held
 * constant — a spread at or below that floor is as good as the ADC can do, and chasing it
 * further would just be tuning against noise.
 */
static void cmd_xtalk(const char *a)
{
    uint32_t reps = arg_hex(&a);
    if (!reps) reps = 4;
    if (reps > 16) reps = 16;

    uint32_t worst = 0, noise = 0;

    for (unsigned t = 0; t < INPUT_POTS; t++) {
        uint16_t mn = 0xFFFF, mx = 0;

        for (unsigned prev = 0; prev < INPUT_POTS; prev++) {
            uint32_t acc = 0;
            for (uint32_t r = 0; r < reps; r++) {
                uint16_t x, y;
                inputs_mux_select(prev);
                inputs_read_phases(&x, &y);          /* charge the S/H with the predecessor */
                inputs_mux_select(t);
                inputs_read_phases(&x, &y);          /* now measure the target */
                acc += x;
            }
            uint16_t avg = (uint16_t)(acc / reps);
            if (avg < mn) mn = avg;
            if (avg > mx) mx = avg;

            /* Predecessor == target is the control: any spread there is noise, not crosstalk. */
            if (prev == t) {
                uint16_t nmn = 0xFFFF, nmx = 0;
                for (uint32_t r = 0; r < reps; r++) {
                    uint16_t x, y;
                    inputs_mux_select(t);
                    inputs_read_phases(&x, &y);
                    if (x < nmn) nmn = x;
                    if (x > nmx) nmx = x;
                }
                if ((uint32_t)(nmx - nmn) > noise) noise = (uint32_t)(nmx - nmn);
            }
            usb_poll();
        }

        uint32_t spread = (uint32_t)(mx - mn);
        if (spread > worst) worst = spread;
    }

    console_hex("worst spread", worst);
    console_hex("noise floor", noise);
    console_hex("sampling", inputs_get_sampling());
    console_hex("settle us", inputs_get_settle_us());
    console_hex("discards", inputs_get_discards());
    console_hex("mux mode", inputs_get_mux_mode());
}

/* adcsst <hex> — ADC sampling window in ADCLK states, for both phase channels. */
static void cmd_muxmode(const char *a)
{
    uint32_t m = arg_hex(&a);
    inputs_set_mux_mode(m);
    console_hex("mux mode", inputs_get_mux_mode());
}

static void cmd_adcsst(const char *a)
{
    uint32_t v = arg_hex(&a);
    if (v) inputs_set_sampling((uint8_t)(v > 255 ? 255 : v));
    console_hex("sampling states", inputs_get_sampling());
}


/* qt — what the touch sensor actually sees, per key.
 *
 * "Sticks on" and "drops out" are both reports about the DETECT BIT, which is the sensor's
 * conclusion rather than its evidence. delta = reference - signal is the evidence, and it says
 * which of the two is happening: a latched key sits marginally past its threshold with no
 * finger present, while a dropout is a real delta falling back under it.
 *
 * Printed in panel order with the key number alongside, because the sensor's key numbering is
 * scrambled relative to the panel and comparing the wrong two columns wastes time. */
static void cmd_qt(const char *a)
{
    uint32_t secs = arg_hex(&a);

    qt2120_detail_t d;
    uint32_t iters = secs ? secs * 4u : 1u;

    for (uint32_t i = 0; i < iters; i++) {
        if (qt2120_read_detail(&d) != RIIC_OK) { console_write("read failed\r\n"); return; }

        for (unsigned p = 0; p < 8; p++) {
            extern const unsigned char qt2120_pot_to_key[8];
            unsigned k = qt2120_pot_to_key[p];
            int delta = (int)d.reference[k] - (int)d.signal[k];

            char line[64];
            char *dst = line;
            *dst++ = 'p'; put_nib(&dst, p, 1);
            *dst++ = ' '; *dst++ = 'k'; put_nib(&dst, k, 1);
            *dst++ = ' '; *dst++ = 's'; put_nib(&dst, d.signal[k], 4);
            *dst++ = ' '; *dst++ = 'r'; put_nib(&dst, d.reference[k], 4);
            *dst++ = ' '; *dst++ = 'd';
            put_nib(&dst, (uint32_t)(delta < 0 ? -delta : delta), 4);
            *dst++ = ' ';
            *dst++ = (d.keys & (1u << k)) ? '*' : '.';
            *dst++ = 13; *dst++ = 10; *dst = 0;
            console_write(line);
        }
        console_hex("threshold", QT2120_THRESHOLD);
        if (iters > 1) { for (int j = 0; j < 200; j++) usb_poll(); bsp_delay_ms(240); }
        usb_poll();
    }
}

/* qtcal — force a recalibration. Only meaningful with nothing touching the panel. */
static void cmd_qtcal(const char *a)
{
    (void)a;
    console_hex("recalibrate rc", (uint32_t)qt2120_recalibrate());
}


/* qtd [secs] — one line per sample: the eight touch deltas and the detect bits.
 *
 * `qt` prints nine lines per sample, which is fine for a snapshot and useless for watching a
 * key misbehave over time — it also outruns the console. This is the same evidence at 5 Hz in
 * a form that can be left running while somebody works the knobs. */
static void cmd_qtd(const char *a)
{
    uint32_t secs = arg_hex(&a);
    if (!secs) secs = 0x14;

    extern const unsigned char qt2120_pot_to_key[8];
    console_write("D d0..d7 (panel order) then detect bits\r\n");

    for (uint32_t i = 0; i < secs * 5u; i++) {
        qt2120_detail_t det;
        if (qt2120_read_detail(&det) != RIIC_OK) { console_write("read failed\r\n"); return; }

        char line[80];
        char *dst = line;
        *dst++ = 'D'; *dst++ = ' ';

        uint32_t bits = 0;
        for (unsigned p = 0; p < 8; p++) {
            unsigned k = qt2120_pot_to_key[p];
            int delta = (int)det.reference[k] - (int)det.signal[k];
            if (delta < 0) delta = 0;
            if (delta > 0xFFF) delta = 0xFFF;
            put_nib(&dst, (uint32_t)delta, 3);
            *dst++ = ' ';
            if (det.keys & (1u << k)) bits |= (1u << p);
        }
        *dst++ = '|'; *dst++ = ' ';
        put_nib(&dst, bits, 2);
        *dst++ = 13; *dst++ = 10; *dst = 0;
        console_write(line);

        for (int j = 0; j < 100; j++) usb_poll();
        bsp_delay_ms(180);
    }
    console_write("done\r\n");
}

/* qtgain <key> <value> — per-key pulse/scale, register 0x28 + key.
 *
 * The AT42QT2120 gives every key its own gain: high nibble is scale, low nibble is the burst
 * pulse count. Raising it increases that key's touch delta without touching any other key, and
 * without moving the global threshold — which is the right lever when ONE electrode is weaker
 * than its neighbours rather than the whole panel being insensitive. */
static void cmd_qtgain(const char *a)
{
    const char *p = a;
    uint32_t key = arg_hex(&p);
    uint32_t val = arg_hex(&p);
    console_hex("key", key);
    console_hex("  rc", (uint32_t)qt2120_set_gain((uint8_t)key, (uint8_t)val));
    console_hex("  value", val);
}


/* qtpeak — the largest touch delta each key has seen since the last call, and its current one.
 *
 * Peak-hold runs continuously in the background, so this needs no coordination with whoever is
 * at the device: work the knobs, then ask. Compare a suspect knob's peak against a known-good
 * one — a weak electrode shows a proportionally smaller peak, and that is a per-key gain
 * problem rather than anything to do with the global threshold. */
static void cmd_qtpeak(const char *a)
{
    uint16_t pos[8], neg[8], last[8], seen;
    uint32_t calls;
    int reset = (*a == 'r');
    qt2120_get_peaks(pos, neg, last, &seen, &calls, reset);

    /* calls proves the background tracker ran; seen proves the sensor reported detects. If
     * calls is zero the measurement never happened, which is a completely different problem
     * from a touch that produced no delta — and the two are indistinguishable from peaks
     * alone, which is exactly how the last round wasted everyone's time. */
    console_hex("track calls", calls);
    console_hex("keys seen", seen);

    for (unsigned p = 0; p < 8; p++) {
        char line[64];
        char *d = line;
        *d++ = 'p'; put_nib(&d, p, 1);
        *d++ = ' '; *d++ = '+'; put_nib(&d, pos[p], 4);
        *d++ = ' '; *d++ = '-'; put_nib(&d, neg[p], 4);
        *d++ = ' '; *d++ = '='; put_nib(&d, last[p], 4);
        *d++ = ' ';
        *d++ = (pos[p] >= QT2120_THRESHOLD || neg[p] >= QT2120_THRESHOLD) ? '*' : '.';
        *d++ = 13; *d++ = 10; *d = 0;
        console_write(line);
    }
    console_hex("threshold", QT2120_THRESHOLD);
}


/* qtrel <ms> — release debounce, tuned live. */
static void cmd_qtrel(const char *a)
{
    uint32_t ms = arg_hex(&a);
    if (ms) qt2120_set_release_ms(ms);
    console_hex("release ms", qt2120_get_release_ms());
}


/* ------------------------------------------------------------------ selftest */

/* selftest — run the EMP/1 codec tests on the device.
 *
 * The same test bodies run on the host via tools/run-proto-tests.ps1. Running them here as well
 * is not redundant: the host cannot check that the real compiler, the real alignment behaviour
 * and the real integer widths agree with the spec. A wire format verified only on a
 * workstation is a wire format whose first target-specific bug ships. */
static void selftest_report(const char *name, int passed)
{
    console_write(passed ? "  ok   " : "  FAIL ");
    console_write(name);
    console_write("\r\n");
    usb_poll();
}

static void cmd_selftest(const char *a)
{
    (void)a;
    extern int emp_run_selftests(void (*report)(const char *, int));
    console_write("EMP/1 codec tests (device)\r\n");
    console_hex("failures", (uint32_t)emp_run_selftests(selftest_report));
}


/* hang — deliberately stop the main loop, to prove the watchdog actually recovers.
 *
 * An untested recovery mechanism is worth very little; this session has repeatedly shown that
 * the difference between "should work" and "does work" is the whole game. Running this should
 * take the device off the bus for a couple of seconds and bring it back in the BOOTLOADER, with
 * a breadcrumb of 0xFF, entirely by itself.
 *
 * Rehearsing it now, while the device is known good and reachable, is the same reasoning that
 * made M0 exercise the SD-card recovery before anything depended on it. */
static void cmd_hang(const char *a)
{
    (void)a;
    console_write("hanging on purpose; the watchdog should reboot to the bootloader\r\n");
    for (int i = 0; i < 64; i++) usb_poll();
    for (;;) { }                      /* interrupts stay enabled: SysTick must catch this */
}


/* emp — protocol session state. Whether the unsolicited traffic is firing is not something to
 * be reasoned about from the host end; the counters say it directly. */
static void cmd_emp(const char *a)
{
    (void)a;
    const emp_stats_t *st = emp_session_stats();
    console_hex("millis now", bsp_millis());
    console_hex("dwt cyccnt", REG32(0xE0001004UL));
    console_hex("session_id", st->session_id);
    console_hex("host_seen", (uint32_t)st->host_seen);
    console_hex("rx_messages", st->rx_messages);
    console_hex("tx_messages", st->tx_messages);
    console_hex("rx_fragments", st->rx_fragments);
    console_hex("tx_fragments", st->tx_fragments);
    console_hex("rx_decode_errors", st->rx_decode_errors);
    console_hex("rx_seq_gaps", st->rx_seq_gaps);
    console_hex("tx_dropped", st->tx_dropped);
    console_hex("hellos", st->hellos);
    console_hex("pings", st->pings);
    console_hex("fields", surf_field_count());
    console_hex("revision_lo", (uint32_t)surf_applied_revision());
}


/* sim <what> <a> <b> — inject an input event without touching the panel.
 *
 * Separates "the protocol path works" from "the physical input works". The panel proves the
 * hardware; this proves everything above it, and it can be run with nobody at the device. When
 * an end-to-end test comes back empty, being able to exercise each half alone is the difference
 * between a diagnosis and a shrug.
 *
 *   sim r <pot> <delta>   rotate, delta is signed hex (F..= negative)
 *   sim p <pot>           push
 *   sim t <mask>          touch mask
 *   sim b <button>        button press then release
 */
static void cmd_sim(const char *a)
{
    const char *p = a;
    while (*p == ' ') p++;
    char what = *p ? *p++ : 0;

    uint32_t x = arg_hex(&p);
    uint32_t y = arg_hex(&p);

    switch (what) {
    case 'r': (void)ui_state_rotate(x & 7u, (int32_t)y ? (int32_t)y : 1, bsp_millis()); break;
    case 'p': surf_on_push(x & 7u, 1); break;
    case 't': surf_on_touch((uint16_t)x); ui_state_touch((uint16_t)x, bsp_millis()); break;
    case 'b': surf_on_button(x, 1); surf_on_button(x, 0); break;
    default:  console_write("usage: sim r|p|t|b <a> [b]\r\n"); return;
    }
    console_write("injected\r\n");
}


/* type — prove the text renderer on the real panel.
 *
 * Draws the three faces at a few sizes, plus the digits that matter most: a value readout in
 * the monospaced face, so it can be checked that the digits do not shift as they change. */
#include "text.h"
static void cmd_type(const char *a)
{
    (void)a;
    ra8876_set_fg(0x0000);
    ra8876_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    text_draw(&font_ui, 24, 30, "Electra One Mini", 0xFFFF, 0x0000);
    text_draw(&font_value, 24, 80, "0123456789", 0x07FF, 0x0000);
    console_write("drew specimen");
}


/* shot — read the framebuffer back out of the controller and send it to the host.
 *
 * This is the "remote screenshot" the project set out to have, and it is worth more than
 * convenience: it closes the loop between what the firmware believes it drew and what is
 * actually in video memory. A black panel has at least two very different causes — nothing was
 * drawn, or something was drawn and is not being scanned out — and from the outside they are
 * identical. This tells them apart in one command.
 *
 * Raw RGB565 after a text header, because hex would triple the size of an already 768 KB
 * transfer. The host frames on the header and the byte count. */
static void cmd_shot(const char *a)
{
    const char *p = a;
    uint32_t x = arg_hex(&p), y = arg_hex(&p);
    uint32_t w = arg_hex(&p), h = arg_hex(&p);
    if (!w || !h) { x = 0; y = 0; w = LCD_WIDTH; h = LCD_HEIGHT; }

    extern void    ra_write_reg16(uint8_t, uint16_t);
    extern void    ra_cmd(uint8_t);
    extern uint8_t ra_read_data(void);

    emp_session_mute(1);          /* no frames in the middle of the pixels */

    char hdr[48];
    char *d = hdr;
    *d++ = 'S'; *d++ = 'H'; *d++ = 'O'; *d++ = 'T'; *d++ = ' ';
    put_nib(&d, w, 4); *d++ = ' ';
    put_nib(&d, h, 4);
    *d++ = 13; *d++ = 10; *d = 0;
    console_write(hdr);

    /* Read window, then stream. The controller advances its own pointer, so this is one long
     * linear read rather than per-row addressing. */
    ra_write_reg16(0x56, (uint16_t)x);
    ra_write_reg16(0x58, (uint16_t)y);
    ra_write_reg16(0x5A, (uint16_t)w);
    ra_write_reg16(0x5C, (uint16_t)h);
    ra_write_reg16(0x5F, (uint16_t)x);
    ra_write_reg16(0x61, (uint16_t)y);
    ra_cmd(0x04);
    (void)ra_read_data();                 /* the pipeline is one deep; first read is stale */

    uint8_t line[256];
    uint32_t total = w * h * 2u, sent = 0;
    while (sent < total) {
        uint32_t n = (total - sent) < sizeof(line) ? (total - sent) : sizeof(line);
        for (uint32_t i = 0; i < n; i++) line[i] = ra_read_data();
        usb_write_block(line, n);
        sent += n;
        watchdog_kick();
    }

    /* Restore the window, or every later fill is clipped to the screenshot rectangle. */
    ra_write_reg16(0x56, 0);
    ra_write_reg16(0x58, 0);
    ra_write_reg16(0x5A, LCD_WIDTH);
    ra_write_reg16(0x5C, LCD_HEIGHT);
    emp_session_mute(0);
    console_write("\r\nSHOTEND\r\n");
}

/* ui — draw the surface UI, using a locally built descriptor if the host has not sent one. */
/* ui — draw the surface UI's demo scene.
 *
 * src/app/ui.c was excluded from this build for weeks because linking it made the application
 * fail to start, and that was never explained. It was the pixel path: every route into ui.c
 * ends in text_draw() -> ra8876_blit_*, which wrote nothing to VRAM while leaving the active
 * window shrunk to a glyph-sized rectangle. With the bulk write fixed the file links and the
 * application boots normally, so the exclusion is lifted. */
static void cmd_ui(const char *a)
{
    (void)a;
    ui_render_demo();
    console_write("drew surface UI");
}


/* wdt on|off — the liveness watchdog, which is currently suspected of false positives. */
static void cmd_wdt(const char *a)
{
    if (*a == 'o' && a[1] == 'n') { watchdog_arm(); console_write("watchdog armed\r\n"); }
    else { g_watchdog_armed = 0; console_write("watchdog off\r\n"); }   /* the clock keeps running */
}


/* dump <hexaddr> <hexlen> — stream any memory region to the host, raw.
 *
 * The immediate use is the stock bootloader at 0x00000000, which we have never seen. It sets up
 * the entire machine before handing over — clocks, NMI enables, the option bytes at 0x00000400,
 * ThreadX's SysTick, whatever display state exists — and every one of our hardest problems has
 * been a question about inherited state that we answered by inference instead of by reading.
 * One stray SysTick from that bootloader already cost days.
 *
 * Reading flash is just memory access, so this is safe and read-only; the risk is entirely in
 * writing, which this cannot do.
 *
 * Raw bytes after a text header, same shape as `shot`, because hex would triple a 1 MB
 * transfer. Protocol traffic is muted so nothing interleaves into the payload. */
static void cmd_dump(const char *a)
{
    const char *p = a;
    uint32_t addr = arg_hex(&p);
    uint32_t len  = arg_hex(&p);
    if (!len) len = 0x100;

    emp_session_mute(1);

    char hdr[48];
    char *d = hdr;
    *d++ = 'D'; *d++ = 'U'; *d++ = 'M'; *d++ = 'P'; *d++ = ' ';
    put_nib(&d, addr, 8); *d++ = ' ';
    put_nib(&d, len, 8);
    *d++ = 13; *d++ = 10; *d = 0;
    console_write(hdr);

    uint32_t sent = 0;
    while (sent < len) {
        uint32_t n = (len - sent) < 256u ? (len - sent) : 256u;
        usb_write_block((const uint8_t *)(addr + sent), n);
        sent += n;
        watchdog_kick();
    }

    console_write("\r\nDUMPEND\r\n");
    emp_session_mute(0);
}


/* blitfill <x> <y> <w> <h> <rgb565> <mode> — push a solid block through the pixel-write path.
 *
 * Text renders nothing on the device while the same code renders correctly in the host
 * simulator, so the fault is in how pixels reach VRAM rather than in the glyph maths. This
 * strips the font out of the question entirely: same windowed write, trivially predictable
 * content.
 *
 * mode 0 uses the multi-frame bulk burst; mode 1 pushes the identical bytes one 16-bit frame
 * at a time, which is exactly the framing the screenshot read path uses and which is therefore
 * known to work. If mode 1 draws and mode 0 does not, the bulk burst is the culprit. */
static void cmd_blitfill(const char *a)
{
    const char *p = a;
    uint32_t x = arg_hex(&p), y = arg_hex(&p), w = arg_hex(&p), h = arg_hex(&p);
    uint32_t c = arg_hex(&p), mode = arg_hex(&p);
    if (!w || !h) { console_write("usage: blitfill x y w h rgb565 mode"); return; }

    extern void ra_data(uint8_t);
    static uint16_t buf[LCD_WIDTH];
    for (uint32_t i = 0; i < w && i < LCD_WIDTH; i++) buf[i] = (uint16_t)c;

    ra8876_blit_begin((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h);
    for (uint32_t r = 0; r < h; r++) {
        if (mode == 0) {
            ra8876_blit_pixels(buf, w);
        } else {
            for (uint32_t i = 0; i < w; i++) {
                ra_data((uint8_t)(buf[i] & 0xFF));
                ra_data((uint8_t)(buf[i] >> 8));
            }
        }
        watchdog_kick();
    }
    ra8876_blit_end();
    console_write("blitfill done");
}


/* rst — a bare warm reset, requesting nothing.
 *
 * Exactly what the rear RESET button does, and deliberately distinct from `boot`, which sets
 * BOOT_REQ_UPDATE and is therefore answered by the bootloader's very first test. That
 * difference matters: the crash-counter path is only reachable when no request is pending, so
 * with only `boot` available it could never be exercised from the host, and a bug that made
 * the bootloader refuse a known-good image sat unnoticed behind it. */
static void cmd_rst(const char *a)
{
    (void)a;
    console_write("resetting");
    for (volatile uint32_t i = 0; i < 200000u; i++) { }   /* let the reply drain */
    REG32(SCB_AIRCR) = 0x05FA0004UL;                      /* SYSRESETREQ */
    for (;;) { }
}


/* potcap <pot> <secs> — park on one knob and stream every raw phase pair.
 *
 * The rotation decode has to know what these two tracks actually do as the shaft turns, and
 * that is a measurement, not a datasheet fact: the pots are endless and two-track, the stock
 * thresholds we inherited (5 / 0x200 / 0x3FE) are in 10-bit units against our 12-bit ADC, and
 * the existing `pots` command only prints samples that already moved past a deadband — which
 * throws away exactly the shape we need.
 *
 * So: one channel, no deadband, no filtering, compact output. Turn the knob slowly through a
 * few full revolutions and the capture contains the complete transfer function. */
static void cmd_potcap(const char *a)
{
    const char *p = a;
    uint32_t pot  = arg_hex(&p);
    uint32_t secs = arg_hex(&p);
    if (pot >= INPUT_POTS) pot = 0;
    if (!secs) secs = 0x0A;

    extern const unsigned char inputs_mux_to_pot_rotation[8];
    unsigned ch = 0;
    for (unsigned c = 0; c < 8u; c++) if (inputs_mux_to_pot_rotation[c] == pot) ch = c;

    inputs_mux_select(ch);

    /* Records immediately, for the whole window. No arming.
     *
     * An earlier version waited for movement before recording, to remove the need to time the
     * start. It made things worse: the operator turned the knob to arm it, recording began
     * silently, and by then they had stopped -- 4,497 samples of a stationary pot whose value
     * had demonstrably changed since the previous capture. The device has no way to say "now",
     * so the honest arrangement is to record the entire window and ask for continuous movement
     * throughout it. A capture with a still section at each end costs nothing to analyse. */
    console_write("POTCAP");

    for (uint32_t i = 0; i < secs * 200u; i++) {
        uint16_t va, vb;
        inputs_read_phases(&va, &vb);

        char line[16];
        char *d = line;
        put_nib(&d, va, 3);
        *d++ = ' ';
        put_nib(&d, vb, 3);
        *d++ = 13; *d++ = 10; *d = 0;
        console_write(line);

        usb_poll();
        watchdog_kick();
        bsp_delay_ms(5);
    }
    console_write("CAPEND");
}


/* uistate [secs] — stream the interaction state alongside the inputs feeding it.
 *
 * Focus problems are invisible from either end on its own: the sensor can read a clean zero
 * while the UI stays zoomed, and the UI can look correct while the mask underneath it is
 * latched. Chasing that by screenshot cost a long detour once already, so here are both halves
 * on one line. */
static void cmd_uistate(const char *a)
{
    uint32_t secs = arg_hex(&a);
    if (!secs) secs = 0x0A;

    console_write("raw filt moved | focus page ws held press");
    for (uint32_t i = 0; i < secs * 10u; i++) {
        uint16_t raw_mask = 0;
        (void)qt2120_read_touch(&raw_mask);

        const ui_state_t *st = ui_state();
        char line[96];
        char *d = line;
        put_nib(&d, raw_mask, 2);          *d++ = ' ';
        put_nib(&d, g_touch_filtered, 2);  *d++ = ' ';
        put_nib(&d, g_moved_mask, 2);      *d++ = ' '; *d++ = '|'; *d++ = ' ';
        put_nib(&d, (uint32_t)(st->focused < 0 ? 0xFFu : (uint32_t)st->focused), 2); *d++ = ' ';
        put_nib(&d, st->page, 2);          *d++ = ' ';
        put_nib(&d, (uint32_t)(uint8_t)st->ws, 2);         *d++ = ' ';
        put_nib(&d, (uint32_t)(uint8_t)st->held_digit, 2); *d++ = ' ';
        put_nib(&d, st->press_mask, 2);
        *d++ = 13; *d++ = 10; *d = 0;
        console_write(line);

        usb_poll();
        watchdog_kick();
        bsp_delay_ms(100);
    }
    console_write("done");
}


/* tcap [secs] — the touch path end to end, on one line per sample.
 *
 * Three faults are left on this panel and all three are about WHEN a touch is believed: drilling
 * in lags, drilling out lags, and drilling out sometimes happens with a finger still on the
 * knob. Those have different causes and the same symptom, and none of them can be told apart
 * from the outside.
 *
 * So: a millisecond stamp, the sensor's own detect mask, the filtered mask the UI consumes, and
 * the per-key margin the detect decision is actually made on. If a touch only clears its
 * threshold by a couple of counts, the sensor is deciding on noise and no amount of software
 * debounce will make it steady. */
static void cmd_tcap(const char *a)
{
    uint32_t secs = arg_hex(&a);
    if (!secs) secs = 0x08;

    extern const unsigned char qt2120_pot_to_key[8];

    /* Heartbeats and READY frames share this pipe and contain printable ASCII, so they land in
     * the middle of a sample line and make the trace unparseable. */
    emp_session_mute(1);
    console_write("ms raw filt | per-pot delta (reference - signal), threshold in hex below");
    console_hex("threshold", QT2120_THRESHOLD);

    uint32_t t_end = bsp_millis() + secs * 1000u;
    while ((int32_t)(bsp_millis() - t_end) < 0) {
        uint16_t rawm = 0;
        (void)qt2120_read_touch(&rawm);

        qt2120_detail_t det;
        int have = (qt2120_read_detail(&det) == RIIC_OK);

        char line[128];
        char *d = line;
        put_nib(&d, bsp_millis(), 5); *d++ = ' ';
        put_nib(&d, rawm, 2);         *d++ = ' ';
        put_nib(&d, g_touch_filtered, 2); *d++ = ' '; *d++ = '|';

        for (unsigned p = 0; p < 8u; p++) {
            *d++ = ' ';
            if (!have) { *d++ = '?'; *d++ = '?'; *d++ = '?'; continue; }
            uint8_t k = qt2120_pot_to_key[p];
            int32_t delta = (int32_t)det.reference[k] - (int32_t)det.signal[k];
            if (delta < 0) { *d++ = '-'; delta = -delta; } else { *d++ = '+'; }
            if (delta > 0xFFF) delta = 0xFFF;
            put_nib(&d, (uint32_t)delta, 3);
        }
        *d++ = 13; *d++ = 10; *d = 0;
        console_write(line);

        usb_poll();
        watchdog_kick();
        bsp_delay_ms(20);
    }
    emp_session_mute(0);
    console_write("done");
}

static const console_cmd_t app_cmds[] = {
    { "help", "list commands",                       console_help },
    { "id",   "identify + startup status",           cmd_id       },
    { "boot", "reboot into the bootloader (update)", cmd_boot     },
    { "rst",  "bare warm reset, as the RESET button", cmd_rst      },
    { "lcd",  "on|off|r|g|b|t",                      cmd_lcd      },
    { "type", "draw a type specimen",                cmd_type     },
    { "blitfill","blitfill x y w h rgb mode", cmd_blitfill },
    { "ui",   "draw the surface UI",                 cmd_ui       },
    { "shot", "shot [x y w h]  read the screen back", cmd_shot    },
    { "dump", "dump <addr> <len>  read memory out",  cmd_dump    },
    { "rd",   "read an RA8876 register, hex",        cmd_rd       },
    /* 2048 (0x0800) is FULL and 8192 (0x2000) is OFF — the controller's compare is
     * 0x2000 - brightness, so the scale runs backwards. Named in the help so nobody else
     * turns the backlight off while trying to turn it up, which is exactly what happened. */
    { "bl",   "backlight: 2048=full .. 8192=off",    cmd_bl       },
    { "sdram","non-destructive SDRAM walk (can fault)", cmd_sdram },
    { "peek", "peek <b|h|w> <addr> [count]",         cmd_peek     },
    { "poke", "poke <b|h|w> <addr> <value>",         cmd_poke     },
    { "pfs",  "pfs <port>  dump pin muxing",         cmd_pfs      },
    { "wr8",  "wr8 <reg> <val>  write RA8876 reg",   cmd_wr8      },
    { "pix",  "pix <x> <y> [n]  read back VRAM",     cmd_pix      },
    { "i2c",  "i2c <0|1|2>  scan a bus",             cmd_i2c      },
    { "i2crd","i2crd <ch> <addr> <reg> <n>",         cmd_i2crd    },
    { "i2cwr","i2cwr <ch> <addr> <reg> <val>",       cmd_i2cwr    },
    { "i2craw","i2craw <ch> <addr> <n>  plain read",  cmd_i2craw   },
    { "bb",   "bb <scl> <sda>  bit-banged scan",      cmd_bb       },
    { "gpio", "gpio <pin> <0|1>  drive a pad",        cmd_gpio     },
    { "qtrst","pulse P402, the QT2120 reset",        cmd_qtrst  },
    { "qt",   "qt [secs]  sensor signal/reference/delta", cmd_qt    },
    { "qtcal","recalibrate the touch sensor",           cmd_qtcal  },
    { "qtd",  "qtd [secs]  stream touch deltas",        cmd_qtd    },
    { "qtgain","qtgain <key> <val>  per-key gain",      cmd_qtgain },
    { "qtpeak","qtpeak [r]  peak touch delta per key", cmd_qtpeak },
    { "qtrel", "qtrel <ms hex>  release debounce",   cmd_qtrel  },
    { "touch","touch [secs]  watch pot capsense",   cmd_touch  },
    { "adcscan","convert all ADC channels, all mux",  cmd_adcscan },
    { "pots", "pots [secs]  stream pot phases",       cmd_pots    },
    { "potcap","potcap <pot> <secs>  raw phase capture", cmd_potcap  },
    { "btns", "btns [secs]  stream buttons/switches", cmd_btns    },
    { "mux",  "mux <ch>  park the multiplexer",       cmd_mux     },
    { "watch","stream all inputs until a key",      cmd_watch   },
    { "uistate","uistate [secs]  focus + touch state", cmd_uistate },
    { "tcap", "tcap [secs]  touch path with margins",  cmd_tcap    },
    { "bench","bench <KiB hex>  bulk throughput",   cmd_bench },
    { "selftest","run the EMP/1 codec tests",        cmd_selftest },
    { "emp",  "EMP session counters",                cmd_emp     },
    { "sim",  "sim r|p|t|b <a> [b]  inject an input", cmd_sim },
    { "hang", "deliberately hang: tests the watchdog",cmd_hang    },
    { "wdt",  "wdt on|off  liveness watchdog",       cmd_wdt     },
    { "settle","settle <us> [discards]  mux timing", cmd_settle },
    { "hexdump","hexdump <addr> <bytes>",             cmd_hexdump },
    { "adctime","adctime <n>  cycles per conversion", cmd_adctime },
    { "xtalk","xtalk [reps]  measure mux contamination", cmd_xtalk },
    { "adcsst","adcsst <states>  ADC sampling window",   cmd_adcsst },
    { "muxmode","muxmode <0|1|2>  strobe behaviour",    cmd_muxmode },
};

/* ------------------------------------------------------------------ main */


/* Report to the bootloader that this image works.
 *
 * The crumb channel and the crash counter share one word, because the bootloader is already in
 * flash, cannot be replaced without the SD card, and its `id` command prints `app_faults`
 * verbatim. That sharing had a bug with teeth: health used to be reported before display
 * bring-up, so the counter was zeroed and then STAGE_LCD_INIT (0x26) through STAGE_LOOP (0x29)
 * were written back into it. APP_FAULT_LIMIT is 3. A perfectly healthy application therefore
 * finished with app_faults == 41, and pressing the rear RESET button made the bootloader
 * refuse to launch a known-good image as a crash loop. It was masked only because the `boot`
 * command sets BOOT_REQ_UPDATE, which the bootloader tests first.
 *
 * So this runs from the main loop instead, after the last crumb has been written. That keeps
 * both properties we actually want: every startup stage is still recorded for an application
 * that dies on the way up, and an application that reaches its loop with the host attached
 * leaves the counter clear.
 *
 * "Reached the loop" is the strongest claim we can make cheaply, and it is deliberately
 * stronger than "main() started": the hang this mechanism exists to catch happened during
 * display bring-up, which is upstream of here. */
static void report_healthy(void)
{
    static uint8_t reported;
    if (reported || !usb_configured() || !boot_handshake_valid()) return;

    volatile boot_handshake_t *h = &BOOT_HANDSHAKE;
    h->app_faults = 0;
    h->health     = BOOT_HEALTHY;
    h->checksum   = boot_handshake_checksum(h);
    reported = 1;
}

int main(void)
{
    /* NOTHING that touches hardware happens before USB is up.
     *
     * The SDRAM self-test used to run here, and it writes to 0x90000000. If the external bus
     * or the SDRAM controller is not working, that is a bus fault, and our fault handler
     * spins — indistinguishable from a hang, with no console to say so. The bootloader never
     * touches SDRAM and the bootloader always works; the application touched it as its first
     * act and never enumerated. That contrast is the evidence.
     *
     * Same principle as the display: bring up the channel that reports failures before doing
     * anything that can fail. The self-test is now the `sdram` command. */
    boot_stage(STAGE_MAIN);
    usb_init();
    boot_stage(STAGE_USB_INIT);
    console_init(app_cmds, sizeof(app_cmds) / sizeof(app_cmds[0]));

    /* The protocol shares the console's pipe, discriminated by EMP's magic byte. session_id is
     * seeded from the boot counter and the cycle counter so it differs on every boot — that is
     * what lets a host notice, within one heartbeat, that the device it is talking to has
     * restarted and forgotten everything. */
    emp_session_init((BOOT_HANDSHAKE.boot_count << 16) ^ REG32(0xE0001004UL));
    console_set_binary_sink(emp_session_feed);
    boot_stage(STAGE_CONSOLE);

    /* Let the host enumerate BEFORE touching the display. The debug channel must never be
     * blocked by the thing being debugged: the first build called ra8876_init() and drew a
     * test pattern before ever polling USB, and because every status wait is an SPI
     * transaction that a non-answering controller runs to the limit, the app never reached
     * its poll loop. The device then vanished from the bus entirely — the single most
     * misleading symptom we have had, because it looks like the image is dead. */
    boot_stage(STAGE_ENUM_WAIT);
    for (uint32_t i = 0; i < 300000 && !usb_configured(); i++) usb_poll();
    for (uint32_t i = 0; i < 20000; i++) usb_poll();
    if (usb_configured()) boot_stage(STAGE_ENUMERATED);

    /* Only NOW report ourselves healthy to the bootloader.
     *
     * The first version cleared this at the very top of main, which defeated the whole
     * mechanism: an application that hung a moment later had already told the bootloader it
     * was fine, so the fault counter never reached its limit and the bootloader kept
     * launching the hang. "Healthy" has to mean something the hang cannot reach — here, that
     * the host has actually enumerated us and the console is available. */
    /* Deliberately NOT reported here any more — see report_healthy(), called from the main
     * loop. Reporting it at this point cleared the crash counter and then let every later
     * boot_stage() write a stage code straight back into it. */

    /* Inputs. Both I2C buses come up here so the console commands work without a scan having
     * been run first — the earlier arrangement only initialised a bus as a side effect of
     * `i2c`, and a transfer issued before that reported a busy bus, which reads like a wiring
     * fault rather than a missing call.
     *
     * The reset pulse is not optional. The AT42QT2120 does not answer at all until P402 is
     * taken low and released: verified from cold, where a scan found nothing, a pulse was
     * applied, and the same scan then found 0x1C every time. */
    riic_init(RIIC_CH_TOUCHKEY);
    riic_init(RIIC_CH_TOUCHSCR);
    inputs_init();
    g_qt_init_rc = qt2120_init();

    boot_stage(STAGE_LCD_INIT);
#if LCD_SKIP_INIT
    /* Diagnostic build: bring up SPI but leave the RA8876 exactly as the stock bootloader
     * configured it.
     *
     * The bootloader is a full ThreadX/SSP application that drives this panel itself — its
     * strings include "LCD: initialised", "Display Initialized", "TextBTE" and "FrameBuffer" —
     * so by the time our first instruction runs the controller is already programmed and
     * working. That makes our own init strictly optional, and therefore testable: if drawing
     * works with it skipped and fails with it applied, the fault is in what we write, and the
     * bootloader's live register values become the reference we have been missing.
     *
     * Skipping it also lets us read the working configuration off the chip directly, which is
     * far cheaper than recovering it from 300 KB of disassembled C++. */
    spi1_init();
    g_lcd_init_rc = 0;
#else
    g_lcd_init_rc = ra8876_init();
    boot_stage(STAGE_LCD_ON);
    ra8876_display_on(1);
    boot_stage(STAGE_BACKLIGHT);
    ra8876_backlight(0x0800);
#endif
    boot_stage(STAGE_LOOP);

    /* Test pattern is now a command (`lcd t`), not a startup action. Drawing ~80 rectangles
     * through a controller that may not be answering is exactly the sort of thing that
     * should be requested deliberately, not done on every boot. */

#if !LCD_SKIP_INIT
    /* The real surface UI is what the panel shows now. The input monitor is still available
     * behind `panel on` while inputs are being proven, but it is no longer what boots.
     *
     * A demo descriptor is loaded so the panel says something before a host has ever
     * connected. It is replaced the moment a real DESC_BEGIN arrives, and it makes the device
     * useful to look at on a bench with nothing plugged into it. */
    surf_demo_descriptor();
    ui_state_init();
    ui_reset();
    ui_render();
#endif

    /* Armed only once everything that runs unboundedly at startup is finished, so a slow
     * display bring-up cannot be mistaken for a hang. */
#if BSS_BALLAST_BYTES > 0
    g_bss_ballast[0] = 1;                 /* stop the linker discarding the evidence */
#endif

    /* NOT armed at boot, for now.
     *
     * The watchdog is firing on a build that otherwise looks healthy, and an automatic reboot
     * loop is the worst possible state to debug from — it destroys the evidence it exists to
     * preserve. Made opt-in via `wdt on` until the reason is understood, so the device stays up
     * and can be interrogated. The fault handlers still recover from real faults; only the
     * liveness timer is off. */
    /* The CLOCK is armed here unconditionally; the liveness watchdog is not.
     *
     * The watchdog stays opt-in behind `wdt on` because it once fired on a build that otherwise
     * looked healthy, and that was never explained. Leaving it off is a defensible caution.
     * Leaving the clock off was not -- and for a long time they were the same switch. */
    clock_start();

    for (;;) {
        g_loop_beat++;
        usb_poll();
        console_poll();
        report_healthy();       /* one-shot; see above for why not before the loop */

        emp_session_poll(bsp_millis());

        /* Service on the CLOCK, not on a loop counter.
         *
         * This used to fire every 8192 iterations of the main loop, which made the input rate a
         * function of how much work the loop happened to be doing. That was survivable while
         * the loop was almost empty; it stopped being survivable once a service pass grew a
         * full eight-pot analog sweep and a UI repaint, because those slowed the very loop
         * whose speed set the rate. The result was seconds of latency on a touch while rotation
         * -- decoded inside the same pass -- still felt immediate, which is a confusing pair of
         * symptoms to be handed.
         *
         * A deadline in milliseconds cannot drift like that: if a pass overruns, the next one
         * starts immediately and the rate degrades gracefully instead of collapsing. */
        {
            uint32_t now = bsp_millis();
            if ((int32_t)(now - g_next_service) < 0) goto no_service;
            g_next_service = now + SERVICE_PERIOD_MS;

            /* Measured, and reported by `id`. A service rate that quietly falls is exactly the
             * kind of fault that presents as "the hardware feels broken". */
            g_service_count++;
            if ((int32_t)(now - g_hz_mark) >= 1000) {
                g_hz_mark = now;
                g_service_hz = g_service_count;
                g_service_count = 0;
            }
        }
        {
            /* Order matters. The scan dwells on whatever the FILTERED mask says is touched, so
             * a momentary sensor dropout does not stop us sampling that pot — otherwise the
             * dropout would hide the very movement that repairs it. */
            static uint16_t touch_filtered;
            uint16_t touch_raw = 0;
            (void)qt2120_read_touch(&touch_raw);

            input_sample_t in;
            inputs_service(touch_filtered, &in);

            static uint32_t last_ms;
            uint32_t now_ms = bsp_millis();
            uint32_t dt_ms  = now_ms - last_ms;
            last_ms = now_ms;
            if (dt_ms > 500u) dt_ms = 500u;      /* first pass, or a long stall elsewhere */

            touch_filtered = qt2120_touch_filter(touch_raw, in.moved_mask, dt_ms);
            g_touch_filtered = touch_filtered;

            /* Physical input -> protocol.
             *
             * The two-track pots are endless, so rotation is a signed delta rather than a
             * position, and the surface layer decides per field whether to integrate it locally
             * or hand the delta to the host. Pushes and buttons are edge-triggered; touch is
             * reported as a mask so focus can be per-lane rather than per-control. */
            /* Seeded from the first sweep, not from zero. Starting at zero makes any switch
             * that happens to be held at power-on look like a fresh press on the first pass --
             * which, now that a pot press drills into a field, would open a view nobody asked
             * for before the panel had even finished starting. */
            static uint16_t prev_push, prev_btn, prev_touch;
            static uint8_t  edges_primed;
            if (!edges_primed) {
                prev_push = in.push_mask;
                prev_btn  = in.button_mask;
                edges_primed = 1;
            }
            g_moved_mask = in.moved_mask;

            for (unsigned p = 0; p < INPUT_POTS; p++) {
                if (in.detents[p]) (void)ui_state_rotate(p, in.detents[p], now_ms);

                uint16_t bit = (uint16_t)(1u << p);
                if ((in.push_mask & bit) != (prev_push & bit)) {
                    int down = (in.push_mask & bit) != 0;
                    ui_state_push(p, down, now_ms);
                    surf_on_push(p, down);
                }
            }
            for (unsigned b = 0; b < INPUT_BUTTONS; b++) {
                uint16_t bit = (uint16_t)(1u << b);
                if ((in.button_mask & bit) != (prev_btn & bit)) {
                    int down = (in.button_mask & bit) != 0;
                    ui_state_button(b, down, now_ms);
                    /* Still reported to the host. BUTTON is diagnostics-only by design, so
                     * device-local navigation never looks like a user editing a value. */
                    surf_on_button(b, down);
                }
            }
            if (touch_filtered != prev_touch) {
                surf_on_touch(touch_filtered);
                ui_state_touch(touch_filtered, now_ms);
            }
            ui_state_tick(now_ms);

            prev_push  = in.push_mask;
            prev_btn   = in.button_mask;
            prev_touch = touch_filtered;

            /* Peak-hold the raw deltas so a touch can be measured after the fact rather than
             * needing anyone to touch a knob inside a capture window. Three I2C transactions,
             * the most expensive thing in this block, so it runs at a quarter rate: it is
             * instrumentation and must not be what limits input responsiveness. */
            static uint8_t peak_div;
            if ((++peak_div & 3u) == 0) qt2120_track_peaks();

            {
                /* Timed, because "drilling in feels slow" has two possible causes with the same
                 * symptom -- the input taking time to be believed, or the panel taking time to
                 * redraw -- and they need opposite fixes. A view switch repaints everything, so
                 * it is the expensive case by far. */
                uint32_t t0 = bsp_millis();
                ui_render();
                uint32_t dt = bsp_millis() - t0;
                if (dt > g_render_ms_max) g_render_ms_max = dt;
            }
        }
    no_service:;

        /* If we are not configured for a long stretch, re-attach. A host that gave up on us
         * while we were busy elsewhere will never retry on its own — it needs to see a
         * disconnect first. Without this, one slow startup costs a physical power cycle. */
        if (!usb_configured()) {
            if (++g_unconfigured > 3000000u) {
                g_unconfigured = 0;
                usb_detach();
                usb_init();
            }
        } else {
            g_unconfigured = 0;
        }

        /* Slow blink on P700 so liveness is visible without a host attached. */
        if ((++g_ticks & 0x3FFFF) == 0) PORT_SET(7, 0);
        else if ((g_ticks & 0x3FFFF) == 0x20000) PORT_CLR(7, 0);
    }
}
