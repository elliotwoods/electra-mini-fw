/* RA8876 display controller driver.
 *
 * Every register number and value below was recovered from the stock firmware. Where a value
 * is computed rather than literal, the computation is reproduced and its result for this
 * panel is stated in a comment, so the arithmetic can be checked without running it.
 *
 * Panel: 800x480, 33.3 MHz nominal pixel clock, HFP 40 / HBP 88 / HSW 128,
 *        VFP 13 / VBP 32 / VSW 2. Totals 1056 x 527.
 *
 * The achieved pixel clock is 33.125 MHz, not 33.3: the PLL solver cannot hit 33.3 exactly
 * from a 10 MHz reference, and picks k=4, n=53. Refresh is therefore
 * 33.125e6 / (1056*527) = 59.52 Hz, not the 59.84 the nominal clock would give.
 */

#include "s7g2.h"
#include "bsp.h"
#include "spi.h"
#include "ra8876.h"

/* --- registers ---------------------------------------------------------- */
#define R_SRR           0x00    /* software reset */
#define R_CCR           0x01    /* chip config */
#define R_MACR          0x02
#define R_ICR           0x03    /* input control; [1:0] selects memory-port target */
#define R_MRWDP         0x04    /* memory read/write data port */
#define R_PPLLC1        0x05
#define R_PPLLC2        0x06
#define R_MPLLC1        0x07
#define R_MPLLC2        0x08
#define R_SPLLC1        0x09
#define R_SPLLC2        0x0A
#define R_MPWCTR        0x10
#define R_PIPCDEP       0x11
#define R_DPCR          0x12
#define R_PCSR          0x13
#define R_HDWR          0x14
#define R_HDWFTR        0x15
#define R_HNDR          0x16
#define R_HNDFTR        0x17
#define R_HSTR          0x18
#define R_HPWR          0x19
#define R_VDHR0         0x1A
#define R_VNDR0         0x1C
#define R_VSTR          0x1E
#define R_VPWR          0x1F
#define R_MISA          0x20    /* main image start address — what is scanned out */
#define R_MIW           0x24
#define R_MWULX         0x26
#define R_CVSSA         0x50    /* canvas start address — where drawing lands */
#define R_CVS_IMWTH     0x54
#define R_AWUL_X        0x56
#define R_AW_WTH        0x5A
#define R_AW_COLOR      0x5E
#define R_CURH          0x5F    /* graphic write cursor, X then Y at 0x61 */
#define R_CURV          0x61
#define R_DCR0          0x67    /* line/triangle draw control */
#define R_DCR1          0x76    /* square/circle draw control */
#define R_PSCLR         0x84    /* PWM prescaler */
#define R_PMUXR         0x85
#define R_PCFGR         0x86
#define R_DZ_LENGTH     0x87
#define R_TCMPB1        0x8C    /* backlight compare */
#define R_TCNTB1        0x8E
#define R_BTE_CTRL0     0x90
#define R_BTE_CTRL1     0x91
#define R_BTE_COLR      0x92
#define R_S0_STR        0x93
#define R_S0_WR         0x97
#define R_S0_X          0x99
#define R_DT_STR        0xA7
#define R_DT_WR         0xAB
#define R_DT_X          0xAD
#define R_BTE_WTH       0xB1
#define R_FGCR          0xD2    /* foreground colour R/G/B */
#define R_BGCR          0xD5    /* background colour = chroma key */
#define R_SDRAR         0xE0
#define R_SDRMD         0xE1
#define R_SDR_REF_L     0xE2
#define R_SDRCR         0xE4

/* STSR bits, read with the 0x40 prefix. */
#define ST_CORE_BUSY    0x08
#define ST_FIFO_FULL    0x80
#define ST_NOT_NORMAL   0x02
#define ST_SDRAM_READY  0x04

/* Status polls are SPI transactions, not register reads: each one costs a full 16-bit frame
 * at 15 MHz. A limit of 2,000,000 was not a safety net, it was a hang — ~80 fills in the test
 * pattern, each burning that budget, is hours. The controller answers in microseconds when it
 * answers at all, so a few thousand is already generous and a failure is reported rather than
 * waited on. */
#define POLL_LIMIT 4000U

/* Called from inside the wait loops so a slow or broken display cannot starve USB. Weak, so
 * a build without USB (or the bootloader) links without it. */
__attribute__((weak)) void ra8876_yield(void) { }

static uint16_t origin_x, origin_y;

/* Bounded. A blind poll must fail in milliseconds, not hang the bring-up: if reads are
 * broken every one of these spins to the limit, and a generous limit would turn a display
 * fault into an apparently dead device. */
static int wait_status_clear(uint8_t mask)
{
    for (uint32_t i = 0; i < POLL_LIMIT; i++) {
        if ((ra_read_status() & mask) == 0) return 0;
        if ((i & 0x3F) == 0) ra8876_yield();
    }
    return -1;
}

static int wait_status_set(uint8_t mask)
{
    for (uint32_t i = 0; i < POLL_LIMIT; i++) {
        if ((ra_read_status() & mask) != 0) return 0;
        if ((i & 0x3F) == 0) ra8876_yield();
    }
    return -1;
}

static void wait_core(void) { (void)wait_status_clear(ST_CORE_BUSY); }

/* --- hardware reset on P313 --------------------------------------------- */

/* Unused: see ra8876_init() on why P313 is left alone. Kept because the pin and the
 * timing are real information that would be expensive to rediscover. */
#ifndef RA8876_USE_P313_RESET
#define RA8876_USE_P313_RESET 0
#endif
#if RA8876_USE_P313_RESET
static void hw_reset(void)
{
    /* Redundant, and deliberately kept: the pin table already declares this pad as an output
     * driven high — it appears there as "P30D", because the pinmap tool numbers pins in hex
     * while Renesas numbers them in decimal. Searching that table for "P313", finding nothing
     * and concluding the reset line was undeclared was a wrong diagnosis; the real fault was
     * the SPI receive overrun in spi.c. Setting it again costs nothing and makes the driver
     * visibly own the line it drives. */
    bsp_pin_cfg(3, 13, PFS_PDR | PFS_PODR);      /* output, idle high */
    bsp_delay_ms(1);

    PORT_CLR(3, 13);
    bsp_delay_ms(5);
    PORT_SET(3, 13);
    bsp_delay_ms(1);
}
#endif

/* --- PLLs ---------------------------------------------------------------
 *
 * f_out = ref * n / 2^k, encoded as C1 = (k & 0x7F) << 1, C2 = (n - 1) & 0xFF.
 * Constraints from the solver: 2 <= n <= 64, VCO (= ref * n) in 100..600 MHz,
 * and ref must be divisible by 2^k.
 *
 * With ref = 10 MHz the stock firmware lands on:
 *   MPLL (memory) 120 MHz : k=1, n=24  -> C1 0x02, C2 0x17
 *   SPLL (core)   120 MHz : k=1, n=24  -> C1 0x02, C2 0x17
 *   PPLL (pixel)  33.125  : k=4, n=53  -> C1 0x08, C2 0x34
 *
 * PPLL picks k=4 because k=1 gives a 60 MHz VCO (below the 100 MHz floor) and k=2/k=3 both
 * land on 32.5 MHz, 800 kHz short; k=4 reaches 33.125, only 175 kHz short. k>=5 is rejected
 * because 10000 is not divisible by 32.
 */
static void pll_program(void)
{
    /* MEASURED, not solved. These are the values read back off this controller while it was
     * demonstrably working — see docs/hardware-notes.md and tools/deploy/dumpmem.py.
     *
     * The previous values here (02/17, 02/17, 08/34) were computed for a 33.3 MHz pixel clock
     * from a datasheet formula we could not verify. They are wrong, and wrong in the worst
     * possible way: a bad memory PLL breaks the controller's SDRAM while leaving its register
     * interface perfectly healthy, so every readback looks correct while the 2D core hangs and
     * every pixel read returns 0xFFFF. Do not "improve" these from a formula. Read them back
     * off a working chip. */
    ra_write_reg8(R_MPLLC1, 0x14);
    ra_write_reg8(R_MPLLC2, 0x85);
    ra_write_reg8(R_SPLLC1, 0x14);
    ra_write_reg8(R_SPLLC2, 0x64);
    ra_write_reg8(R_PPLLC1, 0x14);
    ra_write_reg8(R_PPLLC2, 0x3C);

    ra_write_reg8(R_CCR, 0x00);
    ra_write_reg8(R_CCR, 0x80);          /* reconfigure PLLs */
    (void)wait_status_clear(ST_NOT_NORMAL);
}

/* --- the controller's own SDRAM -----------------------------------------
 *
 * 4 banks, 12 row bits, 9 column bits, 16-bit -> 16 MB.
 *   SDRAR = 0x20 (4 banks) | ((12-11)*8 & 0x18) | (9 & 3)          = 0x29
 *   SDRMD = CAS latency 3                                          = 0x03
 *   refresh interval = (64 ms * 120 MHz) >> 12 rows = 1875         = 0x0753
 */
static int sdram_program(void)
{
    ra_write_reg8(R_SDRAR, 0x29);
    ra_write_reg8(R_SDRMD, 0x03);
    ra_write_reg8(R_SDR_REF_L,     0x53);
    ra_write_reg8(R_SDR_REF_L + 1, 0x07);
    ra_write_reg8(R_SDRCR, 0x01);
    return wait_status_set(ST_SDRAM_READY);
}

/* --- panel timing -------------------------------------------------------
 *
 * HDWR   = width/8 - 1        = 99
 * HNDR   = HBP/8 - 1          = 10
 * HSTR   = (HFP+4)/8 - 1      = 4
 * HPWR   = (HSW+4)/8 - 1      = 15
 * VDHR   = height - 1         = 479
 * VNDR   = VBP - 1            = 31
 * VSTR   = VFP - 1            = 12
 * VPWR   = VSW - 1            = 1
 * All four horizontal figures divide by 8 exactly for this panel.
 */
static void panel_program(void)
{
    ra_write_reg8(R_CCR,  0x80);
    ra_write_reg8(R_MACR, 0x00);
    ra_write_reg8(R_ICR,  0x00);
    ra_write_reg8(R_DPCR, 0x80);         /* display still off */
    ra_write_reg8(R_PCSR, 0xC0);

    ra_write_reg8(R_HDWR,   (LCD_WIDTH / 8) - 1);
    ra_write_reg8(R_HDWFTR, LCD_WIDTH % 8);
    ra_write_reg16(R_VDHR0, LCD_HEIGHT - 1);
    ra_write_reg8(R_HNDR,   (88 / 8) - 1);
    ra_write_reg8(R_HNDFTR, 88 % 8);
    ra_write_reg8(R_HSTR,   ((40 + 4) / 8) - 1);
    ra_write_reg8(R_HPWR,   ((128 + 4) / 8) - 1);
    ra_write_reg16(R_VNDR0, 32 - 1);
    ra_write_reg8(R_VSTR,   13 - 1);
    ra_write_reg8(R_VPWR,   2 - 1);
}

/* --- canvas, active window, BTE defaults -------------------------------- */

static void canvas_program(void)
{
    ra_write_reg8 (R_MPWCTR,    0x04);          /* main window 16 bpp, PIP off */
    ra_write_reg32(R_MISA,      0x00000000);    /* scan out page 0 */
    ra_write_reg16(R_MIW,       LCD_WIDTH);
    ra_write_reg32(R_MWULX,     0x00000000);
    ra_write_reg32(R_CVSSA,     0x00000000);    /* draw into page 0 too, initially */
    ra_write_reg16(R_CVS_IMWTH, LCD_WIDTH);
    ra_write_reg32(R_AWUL_X,    0x00000000);
    ra_write_reg32(R_AW_WTH,    ((uint32_t)LCD_HEIGHT << 16) | LCD_WIDTH);
    ra_write_reg8 (R_AW_COLOR,  0x01);          /* canvas 16 bpp, block mode */
    ra_write_reg8 (R_BTE_COLR,  0x25);          /* S0/S1/dest all 16 bpp */
    ra_write_reg16(R_DT_WR,     LCD_WIDTH);
    ra_write_reg16(R_S0_WR,     LCD_WIDTH);
    ra_write_reg16(R_S0_WR + 10, LCD_WIDTH);    /* S1_WR at 0xA1 */

    origin_x = 0;
    origin_y = 0;
}

/* --- backlight ----------------------------------------------------------
 *
 * PWM1 only. Clock = 120 MHz / 30 (prescaler) / 4 (mux divider) = 1 MHz;
 * period 0x1800 = 6144 -> 162.8 Hz. Duty is (0x2000 - value) / 0x1800, so LARGER value
 * means DIMMER, and 0xFFFF drives the compare past the period, i.e. fully off.
 */
static void pwm_init(void)
{
    ra_write_reg8(R_DZ_LENGTH, 0x7F);
    ra_write_reg8(R_PSCLR,     0x1D);
    ra_write_reg8(R_PMUXR,     0x88);
    ra_write_reg8(0x8A, 0x00); ra_write_reg8(0x8B, 0x18);   /* TCNTB0 = 0x1800 */
    ra_write_reg8(0x88, 0xFF); ra_write_reg8(0x89, 0xFF);   /* TCMPB0 = 0xFFFF */
    ra_write_reg8(R_TCNTB1,     0x00);
    ra_write_reg8(R_TCNTB1 + 1, 0x18);
    ra_write_reg8(R_TCMPB1,     0xFF);
    ra_write_reg8(R_TCMPB1 + 1, 0xFF);
    ra_write_reg8(R_PCFGR,     0x30);           /* timer1 start + auto-reload */
}

static uint16_t programmed_pwm;

/* `brightness` follows the stock firmware's convention, which is inverted and does NOT
 * extend to 0: compare = 0x2000 - brightness, and the PWM period is 0x1800. A compare
 * ABOVE the period is how stock blanks the screen (it writes 0xFFFF, giving compare
 * 0x2001). So anything below 0x0800 also lands above the period and turns the backlight
 * OFF rather than making it brighter.
 *
 *   0x0800  compare 0x1800 = period  -> full brightness
 *   0x1900  compare 0x0700          -> ~29%, what stock dims to before sleeping
 *   0x2000+ compare <= 0            -> off
 *
 * Values are clamped rather than trusted, because passing 0x0400 here is exactly the
 * mistake that produced a black screen on the first flash.
 */
void ra8876_backlight(uint16_t brightness)
{
    if (brightness < 0x0800U) brightness = 0x0800U;
    if (brightness > 0x2000U) brightness = 0x2000U;
    programmed_pwm = (uint16_t)(0x2000U - brightness);
    ra_write_reg16(R_TCMPB1, programmed_pwm);
}

void ra8876_backlight_percent(uint8_t percent)
{
    programmed_pwm = ra8876_backlight_compare_for_percent(percent);
    ra_write_reg16(R_TCMPB1, programmed_pwm);
}

uint16_t ra8876_backlight_programmed_pwm(void) { return programmed_pwm; }

/* --- public ------------------------------------------------------------- */

/* Bit set per stage that did not confirm. Non-zero does NOT abort the bring-up.
 *
 * Rationale, learned the hard way: the first flash returned early on a status-poll timeout,
 * so the display was never turned on and the backlight never raised — which looks exactly
 * like a dead panel and tells you nothing about which layer failed. Every stage now runs
 * unconditionally and records what it could not verify, because the write path can be
 * perfectly healthy while the read path is not, and only the writes are needed to get
 * light out of the panel. */
uint32_t g_ra_status;

#define RA_ST_RESET_POLL   (1U << 0)
#define RA_ST_PLL_POLL     (1U << 1)
#define RA_ST_SDRAM_POLL   (1U << 2)
#define RA_ST_STATUS_STUCK (1U << 3)

int ra8876_init(void)
{
    g_ra_status = 0;

    spi1_init();

    /* Why this brings the controller's own clocks and VRAM up rather than inheriting them.
     *
     * The stock bootloader at 0x00000000 is not a loader but a complete ThreadX/SSP
     * application that drives this panel itself ("LCD: initialised", "Display Initialized",
     * "FrameBuffer", "TextBTE" all appear in its image), so inheriting its display setup the
     * way we inherit SCB->VTOR looked reasonable. It is not: measured from a cold power-on,
     * the controller reports SDRAM-not-ready, because the bootloader only brings the display
     * up on the paths where it intends to draw. Inheriting works after a warm reset and fails
     * after a cold one, which is the worst of both worlds.
     *
     * We do NOT pulse P313 here. It is documented as the controller's reset, but that has
     * never actually been demonstrated — the one test that appeared to confirm it was invalid,
     * its marker write having silently failed — and pulsing it was observed to set the core
     * busy rather than clear it. Since a full soft reset does what we need, leave the
     * unproven line alone. */

    /* Is the read path answering at all? A status register that reads all-zero or all-ones on
     * repeated polls means SPI reads are not working, and every poll below is therefore
     * meaningless rather than merely slow. Worth knowing before interpreting them. */
    uint8_t s1 = ra_read_status();
    uint8_t s2 = ra_read_status();
    if ((s1 == 0x00 && s2 == 0x00) || (s1 == 0xFF && s2 == 0xFF)) {
        g_ra_status |= RA_ST_STATUS_STUCK;
    }

    ra_write_reg8(R_SRR, 0x01);
    if (wait_status_clear(ST_NOT_NORMAL) != 0) g_ra_status |= RA_ST_RESET_POLL;
    bsp_delay_ms(10);                      /* fixed settle, in case the poll is blind */

    pll_program();
    bsp_delay_ms(10);

    if (sdram_program() != 0) g_ra_status |= RA_ST_SDRAM_POLL;
    bsp_delay_ms(20);                      /* SDRAM init needs time even unobserved */

    panel_program();
    canvas_program();
    pwm_init();

    return 0;                              /* never fatal; inspect g_ra_status instead */
}

/* Absolute write, never read-modify-write.
 *
 * DPCR bit7 = PCLK inversion, bit6 = display enable. The stock firmware arrives at 0xC0 by
 * RMW, which is fine when reads work. Ours must not depend on that: if SPI reads return 0x00
 * (a plausible failure that leaves writes perfectly healthy), an RMW computes 0x40 — display
 * enabled but PCLK inversion silently dropped, which on a panel that needs inverted PCLK
 * gives a backlit black screen and no other symptom.
 *
 * Every RA8876 register whose intended final value we know is now written absolutely, for
 * exactly this reason. Read-modify-write on this chip couples the write path to the read
 * path for no benefit. */
void ra8876_display_on(int on)
{
    ra_write_reg8(R_DPCR, on ? 0xC0U : 0x80U);
}

void ra8876_set_canvas(uint32_t addr)       { ra_write_reg32(R_CVSSA, addr); }
void ra8876_set_display_page(uint32_t addr) { ra_write_reg32(R_MISA,  addr); }

/* RGB565 -> the controller's three 8-bit colour registers. */
static void set_colour(uint8_t base, uint16_t c)
{
    ra_write_reg8(base,     (uint8_t)((c >> 8) & 0xF8U));
    ra_write_reg8(base + 1, (uint8_t)(((c >> 5) & 0x3FU) << 2));
    ra_write_reg8(base + 2, (uint8_t)((c & 0x1FU) << 3));
}

void ra8876_set_fg(uint16_t rgb565) { set_colour(R_FGCR, rgb565); }
void ra8876_set_bg(uint16_t rgb565) { set_colour(R_BGCR, rgb565); }

/* Draw-engine filled rectangle. The stock firmware uses this rather than the BTE for
 * plain fills, so we match: DCR1 = 0xE0 is "filled square, start". */
/* A straight line through the same draw engine as fill_rect.
 *
 * Aliased -- the RA8876 has no antialiased line primitive, and blending one through the text
 * path would cost a scanline buffer per line for a one-pixel diagonal nobody looks at closely.
 * The link lines it draws are structural rather than typographic: they say "this knob controls
 * that digit", and a hard edge reads as deliberate at that width. */
void ra8876_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    ra_write_reg16(0x68, (uint16_t)(x0 + origin_x));
    ra_write_reg16(0x6A, (uint16_t)(y0 + origin_y));
    ra_write_reg16(0x6C, (uint16_t)(x1 + origin_x));
    ra_write_reg16(0x6E, (uint16_t)(y1 + origin_y));
    ra_write_reg8(R_DCR0, 0x80);
    wait_core();
}

void ra8876_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h)
{
    ra_write_reg16(0x68, (uint16_t)(x + origin_x));
    ra_write_reg16(0x6A, (uint16_t)(y + origin_y));
    ra_write_reg16(0x6C, (uint16_t)(x + origin_x + w - 1));
    ra_write_reg16(0x6E, (uint16_t)(y + origin_y + h - 1));
    ra_write_reg8(R_DCR1, 0xE0);
    wait_core();
}

/* BTE solid fill — useful when filling into a page that is not the current canvas. */
void ra8876_bte_fill(uint32_t dst, int16_t x, int16_t y, int16_t w, int16_t h)
{
    ra_write_reg32(R_DT_STR, dst);
    ra_write_reg16(R_DT_X,     (uint16_t)(x + origin_x));
    ra_write_reg16(R_DT_X + 2, (uint16_t)(y + origin_y));
    ra_write_reg16(R_BTE_WTH,     (uint16_t)w);
    ra_write_reg16(R_BTE_WTH + 2, (uint16_t)h);
    ra_write_reg8(R_BTE_CTRL1, 0x0C);        /* ROP 0, op 0xC = solid fill */
    ra_write_reg8(R_BTE_CTRL0, 0x10);        /* start */
    wait_core();
}

/* Straight memory copy. Note S0 coordinates are NOT offset by the active-window origin,
 * but destination coordinates are — that asymmetry is the stock driver's and it is load
 * bearing, because sources are raw VRAM pages while destinations are windowed. */
void ra8876_bte_copy(uint32_t src, int16_t sx, int16_t sy,
                     uint32_t dst, int16_t dx, int16_t dy,
                     int16_t w, int16_t h)
{
    ra_write_reg32(R_S0_STR, src);
    ra_write_reg16(R_S0_X,     (uint16_t)sx);
    ra_write_reg16(R_S0_X + 2, (uint16_t)sy);
    ra_write_reg32(R_DT_STR, dst);
    ra_write_reg16(R_DT_X,     (uint16_t)(dx + origin_x));
    ra_write_reg16(R_DT_X + 2, (uint16_t)(dy + origin_y));
    ra_write_reg16(R_BTE_WTH,     (uint16_t)w);
    ra_write_reg16(R_BTE_WTH + 2, (uint16_t)h);
    ra_write_reg8(R_BTE_CTRL1, 0xC2);        /* ROP 0xC (dest := S0), op 2 = memory copy */
    ra_write_reg8(R_BTE_CTRL0, 0x10);
    wait_core();
}

/* Transparent blit: pixels matching the background colour are skipped. Set the key with
 * ra8876_set_bg() first — the stock firmware keys on black. */
void ra8876_bte_copy_chroma(uint32_t src, int16_t sx, int16_t sy,
                            uint32_t dst, int16_t dx, int16_t dy,
                            int16_t w, int16_t h)
{
    ra_write_reg32(R_S0_STR, src);
    ra_write_reg16(R_S0_X,     (uint16_t)sx);
    ra_write_reg16(R_S0_X + 2, (uint16_t)sy);
    ra_write_reg32(R_DT_STR, dst);
    ra_write_reg16(R_DT_X,     (uint16_t)(dx + origin_x));
    ra_write_reg16(R_DT_X + 2, (uint16_t)(dy + origin_y));
    ra_write_reg16(R_BTE_WTH,     (uint16_t)w);
    ra_write_reg16(R_BTE_WTH + 2, (uint16_t)h);
    ra_write_reg8(R_BTE_CTRL1, 0xC5);        /* op 5 = memory copy with chroma key */
    ra_write_reg8(R_BTE_CTRL0, 0x10);
    wait_core();
}

/* --- pixel blitting ------------------------------------------------------
 *
 * Set the active window to a rectangle, park the write cursor at its origin, and stream
 * pixels: the controller wraps at the window edge by itself, so a rectangle arrives as one
 * linear run with no per-row addressing.
 *
 * This is the path text takes. It is the expensive one per pixel — two SPI bytes per data byte
 * — which is exactly why it is used for glyphs and never for backgrounds: a full screen this
 * way is about 0.8 s, while a 200x30 label is about 6 ms. Cost scales with ink.
 */
void ra8876_blit_begin(int16_t x, int16_t y, int16_t w, int16_t h)
{
    ra_write_reg16(R_AWUL_X,     (uint16_t)x);
    ra_write_reg16(R_AWUL_X + 2, (uint16_t)y);
    ra_write_reg16(R_AW_WTH,     (uint16_t)w);
    ra_write_reg16(R_AW_WTH + 2, (uint16_t)h);

    ra_write_reg16(R_CURH, (uint16_t)x);
    ra_write_reg16(R_CURV, (uint16_t)y);

    ra_cmd(R_MRWDP);
}

void ra8876_blit_pixels(const uint16_t *px, uint32_t count)
{
    /* Little-endian on the wire, matching the canvas' 16 bpp layout. */
    ra_write_bulk((const uint8_t *)px, count * 2u);
}

void ra8876_blit_end(void)
{
    /* Put the window back, or every later fill is clipped to the last string drawn — a
     * spectacular and very confusing failure mode. */
    ra_write_reg16(R_AWUL_X,     0);
    ra_write_reg16(R_AWUL_X + 2, 0);
    ra_write_reg16(R_AW_WTH,     LCD_WIDTH);
    ra_write_reg16(R_AW_WTH + 2, LCD_HEIGHT);
}
