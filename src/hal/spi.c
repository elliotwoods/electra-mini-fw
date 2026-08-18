/* SPI1 — the display bus.
 *
 * The RA8876 hangs off R_SPI1 at 15 MHz in mode 3. Everything about the framing below is
 * transcribed from the stock driver, because the RA8876's 4-wire SPI protocol prefixes every
 * transfer with a direction byte and the stock firmware packs those prefixes into the RSPI's
 * 32-bit frames in a specific way:
 *
 *   write register:  00 RR 80 VV      (one 32-bit frame, MSB first)
 *   command only:    00 RR            (one 16-bit frame)
 *   data byte:       80 BB            (one 16-bit frame)
 *   status read:     40 00            (one 16-bit frame, answer in the low byte)
 *   data read:       C0 00            (one 16-bit frame)
 *
 * SPCMD0 selects the frame width and whether chip-select is held between frames. The driver
 * caches the last value and only rewrites it on change, which matters: at 15 MHz the register
 * write costs more than the transfer it configures.
 *
 *   0x0323  32-bit, SSLKP=0   register writes
 *   0x03A3  32-bit, SSLKP=1   bulk pixel bursts (CS held across the whole burst)
 *   0x0F23  16-bit            command/data/status singles
 *
 * Low byte 0x23 = CPHA 1, CPOL 1 (mode 3), BRDV 0, SSL2 as chip select.
 */

#include "s7g2.h"
#include "bsp.h"
#include "spi.h"

#define SPCMD_32BIT      0x0323U
#define SPCMD_32BIT_KEEP 0x03A3U
#define SPCMD_16BIT      0x0F23U

static uint16_t spcmd_cached;

static inline void spcmd_set(uint16_t v)
{
    if (spcmd_cached != v) {
        REG16(SPI1_SPCMD0) = v;
        spcmd_cached = v;
    }
}

/* Bounded, like every other hardware wait in this tree. An SPI unit that never leaves the
 * idle-not-finished state must not be able to take the whole image down with it — the display
 * is the thing most likely to be broken, and it must never be able to silence the console
 * that reports on it. */
static inline void wait_idle(void)
{
    for (uint32_t t = 0; t < 100000UL; t++) {
        if (!(REG8(SPI1_SPSR) & SPSR_IDLNF)) return;
    }
}

void spi1_init(void)
{
    REG32(MSTPCRB) = REG32(MSTPCRB) & ~MSTPB_SPI1;

    REG8(SPI1_SPCR)  = 0x00U;                 /* disabled while we configure */
    /* PCLKA/4 = 30 MHz. Rate = PCLKA / (2 * (SPBR+1) * 2^BRDV), PCLKA = 120 MHz.
     *
     * Was 0x03, i.e. 15 MHz, transcribed from the stock driver. Text travels over this bus
     * pixel by pixel, so it sets how fast a readout can repaint, and the controller's write
     * timing is specified well above this. If the display ever misbehaves after a change here,
     * this is the first thing to put back -- and `blitfill` measures the difference directly. */
    REG8(SPI1_SPBR)  = 0x01U;

    REG16(SPI1_SPCMD0 + 0) = SPCMD_32BIT_KEEP;
    REG16(SPI1_SPCMD0 + 2) = SPCMD_32BIT_KEEP;
    REG16(SPI1_SPCMD0 + 4) = SPCMD_32BIT_KEEP;
    REG16(SPI1_SPCMD0 + 6) = SPCMD_32BIT_KEEP;
    spcmd_cached = SPCMD_32BIT_KEEP;

    REG8(SPI1_SPSCR) = (uint8_t)(REG8(SPI1_SPSCR) & ~0x07U);   /* sequence length 1 */
    REG8(SPI1_SPDCR) = (uint8_t)(REG8(SPI1_SPDCR) & ~0x03U);
    if (REG8(SPI1_SPSR) != 0) REG8(SPI1_SPSR) = 0;

    REG8(SPI1_SPCR) = (uint8_t)(REG8(SPI1_SPCR) | SPCR_MSTR);
    REG8(SPI1_SPCR) = (uint8_t)(REG8(SPI1_SPCR) | SPCR_SPE);
}

/* --- single 32-bit frame ------------------------------------------------ */

/* Reading SPDR after a transfer is not optional, and leaving it out is what made the display
 * look dead.
 *
 * RSPI is full duplex: every byte clocked out clocks a byte in. The write path here never read
 * the receive buffer, so the second write overran it and set SPSR.OVRF — and once OVRF is set
 * the receiver DISCARDS all further data until the flag is cleared. Every subsequent read
 * returned the idle bus, 0xFF, which read exactly like a controller that was not answering.
 * The controller was answering the whole time; we were throwing the answers away.
 *
 * The read must match the current SPLW width, or SPDR does not return what was received. */
static void drain(int wide)
{
    if (wide) (void)REG32(SPI1_SPDR);
    else      (void)REG16(SPI1_SPDR);

    if (REG8(SPI1_SPSR) & SPSR_ERRORS) REG8(SPI1_SPSR) = 0;
}

/* Start every transfer from a clean receiver.
 *
 * Draining afterwards is not sufficient on its own. The bulk pixel path sets SPDCR.SPFC so a
 * single SPDR access moves several frames, and any mismatch between frames sent and frames read
 * leaves OVRF set — after which the receiver DISCARDS everything until the flag is cleared, so
 * every later register read returns the idle bus, 0xFF. That is indistinguishable from a
 * display that has stopped answering, and it has now caused that misdiagnosis twice.
 *
 * Clearing before a transfer, rather than trusting every writer to clean up after itself, makes
 * the read path independent of whatever ran before it. A few cycles against a fault that is
 * invisible until something tries to read. */
static void spi_clear_rx(void)
{
    /* RE-ENABLE THE UNIT FIRST. ra_write_bulk clears SPCR.SPE at the end of a burst to drop
     * chip select — and nothing ever set it again, so from the first text blit onwards every
     * transfer was a silent no-op: register writes vanished, reads returned the idle bus, and
     * the panel stopped changing.
     *
     * That presented as "the display went black and every read is 0xFF", which is
     * indistinguishable from the controller having died — the second time in this project a
     * quiet SPI unit has been misread as a dead display. Asserting the precondition here,
     * instead of trusting every caller to leave the unit enabled, makes it unreintroducible. */
    REG8(SPI1_SPCR) = (uint8_t)(REG8(SPI1_SPCR) | SPCR_SPE);

    /* Then start from a clean receiver: a stale OVRF makes the receiver discard everything
     * until cleared, which is a different fault with the very same symptom. */
    if (REG8(SPI1_SPSR) & SPSR_ERRORS) {
        (void)REG32(SPI1_SPDR);
        REG8(SPI1_SPSR) = 0;
    }
}

static void frame32(uint32_t word)
{
    spi_clear_rx();
    spcmd_set(SPCMD_32BIT);
    REG8(SPI1_SPDCR) = (uint8_t)(REG8(SPI1_SPDCR) | SPDCR_SPLW);
    REG32(SPI1_SPDR) = word;
    wait_idle();
    drain(1);
}

/* --- single 16-bit frame, optionally returning the received low byte ----- */

static uint8_t frame16(uint16_t word)
{
    spi_clear_rx();
    spcmd_set(SPCMD_16BIT);
    REG8(SPI1_SPDCR) = (uint8_t)(REG8(SPI1_SPDCR) & ~SPDCR_SPLW);
    REG16(SPI1_SPDR) = word;
    wait_idle();

    uint8_t rx = (uint8_t)(REG16(SPI1_SPDR) & 0xFFU);
    if (REG8(SPI1_SPSR) & SPSR_ERRORS) REG8(SPI1_SPSR) = 0;
    return rx;
}

/* --- RA8876 primitives -------------------------------------------------- */

void ra_write_reg8(uint8_t reg, uint8_t value)
{
    frame32(((uint32_t)0x00U << 24) | ((uint32_t)reg << 16) |
            ((uint32_t)0x80U << 8)  | (uint32_t)value);
}

void ra_write_reg16(uint8_t reg, uint16_t value)
{
    ra_write_reg8(reg,          (uint8_t)(value & 0xFFU));
    ra_write_reg8((uint8_t)(reg + 1), (uint8_t)(value >> 8));
}

void ra_write_reg32(uint8_t reg, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) {
        ra_write_reg8((uint8_t)(reg + i), (uint8_t)((value >> (8 * i)) & 0xFFU));
    }
}

void ra_cmd(uint8_t reg)
{
    (void)frame16((uint16_t)(0x0000U | reg));
}

void ra_data(uint8_t byte)
{
    (void)frame16((uint16_t)(0x8000U | byte));
}

uint8_t ra_read_status(void)
{
    return frame16(0x4000U);
}

uint8_t ra_read_data(void)
{
    return frame16(0xC000U);
}

uint8_t ra_read_reg(uint8_t reg)
{
    ra_cmd(reg);
    return ra_read_data();
}

/* --- bulk pixel path ----------------------------------------------------
 *
 * One chip-select assertion for the whole burst, first byte 0x80, then the raw stream packed
 * four bytes per 32-bit SPDR write, big-endian. This is the only path that gets anywhere near
 * the bus's capacity — and even so, a full 800x480 frame is ~1.5 MB on the wire at 2 bytes
 * per pixel byte, about 0.8 s. That is why the compositor keeps assets in the RA8876's own
 * VRAM and moves them with the BTE instead of streaming pixels.
 *
 * The stock firmware does this as a polled loop with no DMA. So do we, for now; wiring the
 * DTC to it is unclaimed headroom noted in the plan.
 */
void ra_write_bulk(const uint8_t *buf, uint32_t len)
{
    if (buf == 0 || len == 0) return;

    /* Built out of the single 32-bit frame that every register write already uses, because the
     * previous implementation did not work at all.
     *
     * That version drove SPDCR.SPFC to pack many frames per SPDR access and hold chip select
     * across the whole burst. It wrote nothing — not corrupted pixels, nothing — while
     * reporting success, which is why text rendered correctly in the host simulator and was
     * invisible on the device. It was diagnosed by pushing an identical solid block through
     * two paths (`blitfill ... 0` and `... 1`) and photographing the result: the byte-at-a-time
     * block appeared, the bulk block did not.
     *
     * The RA8876 keeps its memory-write pointer advancing across separate SPI transactions, so
     * the burst does not need chip select held; each frame can carry its own 0x80 data prefix.
     * That gives four wire bytes per three payload bytes, against two-per-one for the naive
     * byte loop, and it reuses framing that is known-good rather than a mode we were guessing
     * at from a register description. If this ever needs to be faster, verify any replacement
     * with `blitfill` before trusting it. */
    uint32_t i = 0;

    while (len - i >= 3u) {
        frame32(0x80000000UL | ((uint32_t)buf[i] << 16)
                             | ((uint32_t)buf[i + 1] << 8)
                             |  (uint32_t)buf[i + 2]);
        i += 3u;
    }
    for (; i < len; i++) ra_data(buf[i]);
}
