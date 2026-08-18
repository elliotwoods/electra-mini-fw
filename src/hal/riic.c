/* RIIC — polled I2C master.
 *
 * Two buses matter on this board: RIIC0 to the AT42QT2120 pot-touch sensor at 0x1C, and RIIC2
 * to the FT5x06 touchscreen at 0x38. Both are 400 kHz.
 *
 * Polled, not interrupt-driven, for the same reason the USB stack is: a polled transfer that
 * goes wrong stops in a place we can see, and there is no ICU routing to get right before
 * anything works at all. Nothing here is on a latency-critical path — the stock firmware's own
 * input scan runs on a 12 ms period.
 *
 * Every wait is bounded. That is a standing rule in this tree and it was earned: an unbounded
 * spin on a peripheral that is not answering is indistinguishable from a dead device, and we
 * have now lost days twice to exactly that shape of bug. A stuck I2C bus must return an error
 * that the console can print, not hang the image.
 *
 * The receive path is the fiddly part and it is written out longhand below rather than being
 * compressed, because the last-byte handshake is where RIIC implementations go wrong.
 */

#include "s7g2.h"
#include "bsp.h"
#include "riic.h"

/* Generous relative to a 400 kHz byte (~22 us) but far short of anything a human notices. */
#define RIIC_SPIN  200000UL

volatile uint8_t g_riic_stage;
volatile uint8_t g_riic_fail;
volatile uint8_t g_riic_icsr2, g_riic_iccr2, g_riic_icmr3;

static int wait_set(uint32_t reg, uint8_t mask)
{
    for (uint32_t t = 0; t < RIIC_SPIN; t++) {
        if (REG8(reg) & mask) return RIIC_OK;
    }
    return RIIC_ERR_TIMEOUT;
}

static int wait_clear(uint32_t reg, uint8_t mask)
{
    for (uint32_t t = 0; t < RIIC_SPIN; t++) {
        if (!(REG8(reg) & mask)) return RIIC_OK;
    }
    return RIIC_ERR_TIMEOUT;
}

/* Poll for `mask`, but give up early if the slave NACKed — otherwise a NACK is reported as a
 * timeout, and the two have completely different causes: absent device versus stuck bus. */
static int wait_set_or_nack(unsigned ch, uint8_t mask)
{
    for (uint32_t t = 0; t < RIIC_SPIN; t++) {
        uint8_t s = REG8(RIIC_ICSR2(ch));
        if (s & ICSR2_NACKF) return RIIC_ERR_NACK;
        if (s & mask)        return RIIC_OK;
    }
    return RIIC_ERR_TIMEOUT;
}

void riic_init(unsigned ch)
{
    uint32_t mstp = (ch == 0) ? MSTPB_RIIC0 : (ch == 1) ? MSTPB_RIIC1 : MSTPB_RIIC2;
    REG32(MSTPCRB) = REG32(MSTPCRB) & ~mstp;

    /* Hold the module in reset while it is configured. ICE must be cleared before IICRST is
     * set, and set again before the rest of the registers are writable. */
    REG8(RIIC_ICCR1(ch)) = ICCR1_SOWP;
    REG8(RIIC_ICCR1(ch)) = ICCR1_SOWP | ICCR1_IICRST;
    REG8(RIIC_ICCR1(ch)) = ICCR1_SOWP | ICCR1_IICRST | ICCR1_ICE;

    /* Bit rate. PCLKB is 60 MHz; CKS = 2 divides it to 15 MHz, and a period of
     * (ICBRH+1) + (ICBRL+1) = 16 + 22 = 38 gives 394.7 kHz. Deliberately a little under
     * 400 kHz rather than a little over — fast-mode I2C has a maximum, not a target — and the
     * low period is the longer of the two, as fast mode requires. */
    REG8(RIIC_ICMR1(ch)) = 0x28U;                 /* CKS = 2, BCWP = 1 */
    REG8(RIIC_ICBRH(ch)) = (uint8_t)(0xE0U | 15U);
    REG8(RIIC_ICBRL(ch)) = (uint8_t)(0xE0U | 21U);

    REG8(RIIC_ICSER(ch)) = 0x00U;                 /* no slave addresses: we are master only */
    REG8(RIIC_ICMR2(ch)) = 0x06U;
    REG8(RIIC_ICMR3(ch)) = 0x00U;

    /* NACKE suspends the transfer on a NACK, which is what makes a bus scan work: an absent
     * device leaves NACKF set instead of clocking out data nobody is acknowledging.
     * NFE enables the digital noise filter. SCLE enables clock stretching by the slave, which
     * the FT5x06 uses. */
    REG8(RIIC_ICFER(ch)) = 0x72U;
    REG8(RIIC_ICIER(ch)) = 0x00U;                 /* polled throughout */

    REG8(RIIC_ICCR1(ch)) = ICCR1_SOWP | ICCR1_ICE;   /* release reset */
}

/* Which pads each bus uses, for the bit-banged recovery below.
 *
 * Note the pin numbers are the DATASHEET's (decimal): RIIC2 is P511/P512, which appear in
 * docs/pinmap.txt as P50B/P50C because that tool prints the pin number in hex. Getting this
 * wrong toggles the wrong pad and looks exactly like recovery not working. */
typedef struct { uint8_t scl_port, scl_pin, sda_port, sda_pin, valid; } riic_pins_t;

static riic_pins_t bus_pins(unsigned ch)
{
    riic_pins_t p = { 0, 0, 0, 0, 0 };
    if (ch == 0)      { p.scl_port = 4; p.scl_pin = 0;  p.sda_port = 4; p.sda_pin = 1;  p.valid = 1; }
    else if (ch == 2) { p.scl_port = 5; p.scl_pin = 12; p.sda_port = 5; p.sda_pin = 11; p.valid = 1; }
    return p;
}

/* A slave caught mid-byte by an MCU reset sits holding SDA low forever, and no amount of
 * correct master code fixes it: the bus has to be clocked until the slave finishes the byte it
 * believes it is still transmitting. We reset this device constantly while iterating on
 * firmware, so this is not a rare case — it is the normal case after a flash.
 *
 * It has to be done by driving the pads as GPIO. The first attempt used the module's own CLO
 * bit while the module was held in reset, which cannot clock anything; the bus stayed stuck
 * and the routine looked like it had run.
 */
void riic_bus_recover(unsigned ch)
{
    riic_pins_t p = bus_pins(ch);
    if (!p.valid) return;

    if (REG8(RIIC_ICCR1(ch)) & ICCR1_SDAI) return;      /* SDA already high: nothing stuck */

    /* Take both pads away from the peripheral. SCL driven, SDA released so the slave can let
     * go of it — driving SDA here would fight the very device we are trying to free. */
    bsp_pin_cfg(p.scl_port, p.scl_pin, PFS_PDR | PFS_PODR);   /* output, high */
    bsp_pin_cfg(p.sda_port, p.sda_pin, 0);                    /* input */

    for (unsigned i = 0; i < 9; i++) {
        PORT_CLR(p.scl_port, p.scl_pin);
        bsp_delay_us(5);
        PORT_SET(p.scl_port, p.scl_pin);
        bsp_delay_us(5);
        if (PORT_GET(p.sda_port, p.sda_pin)) break;
    }

    /* Manufacture a STOP so the slave's state machine returns to idle: SDA low while SCL is
     * high, then SDA released. Without this the slave may still consider a transfer open. */
    bsp_pin_cfg(p.sda_port, p.sda_pin, PFS_PDR);              /* output, low */
    bsp_delay_us(5);
    PORT_SET(p.scl_port, p.scl_pin);
    bsp_delay_us(5);
    bsp_pin_cfg(p.sda_port, p.sda_pin, 0);                    /* release: line pulls high */
    bsp_delay_us(5);

    /* Back to the peripheral. PSEL 0x7 is RIIC; PMR hands the pad to it. */
    bsp_pin_cfg(p.scl_port, p.scl_pin, PFS_PMR | (0x07UL << 24));
    bsp_pin_cfg(p.sda_port, p.sda_pin, PFS_PMR | (0x07UL << 24));

    riic_init(ch);
}

/* START (or repeated START) followed by the address byte. */
static int start_and_address(unsigned ch, uint8_t addr7, int reading, int repeated)
{
    if (!repeated) {
        g_riic_stage = RIIC_ST_BBSY;
        if (wait_clear(RIIC_ICCR2(ch), ICCR2_BBSY) != RIIC_OK) return RIIC_ERR_BUSY;
        REG8(RIIC_ICSR2(ch)) = 0x00U;                       /* clear stale status */
        REG8(RIIC_ICCR2(ch)) = REG8(RIIC_ICCR2(ch)) | ICCR2_ST;
    } else {
        /* Request the restart, THEN clear TEND. Clearing it first lets the module treat the
         * transmit phase as still running; leaving it set blocks the restart from being
         * recognised. Both orders fail silently as a timeout, which is why this is spelled
         * out rather than left to look arbitrary. */
        REG8(RIIC_ICCR2(ch)) = REG8(RIIC_ICCR2(ch)) | ICCR2_RS;
        REG8(RIIC_ICSR2(ch)) = (uint8_t)(REG8(RIIC_ICSR2(ch)) & ~ICSR2_TEND);
    }

    g_riic_stage = repeated ? RIIC_ST_RS_TDRE : RIIC_ST_ADDR_TDRE;
    if (wait_set(RIIC_ICSR2(ch), ICSR2_TDRE) != RIIC_OK) return RIIC_ERR_TIMEOUT;

    REG8(RIIC_ICDRT(ch)) = (uint8_t)((addr7 << 1) | (reading ? 1U : 0U));

    /* TEND means the address byte has been clocked out and the ACK bit sampled. */
    g_riic_stage = repeated ? RIIC_ST_RS_ACK : RIIC_ST_ADDR_ACK;
    int rc = wait_set_or_nack(ch, reading ? ICSR2_RDRF : ICSR2_TEND);
    return rc;
}

static int issue_stop(unsigned ch)
{
    REG8(RIIC_ICSR2(ch)) = (uint8_t)(REG8(RIIC_ICSR2(ch)) & ~ICSR2_STOP);
    REG8(RIIC_ICCR2(ch)) = REG8(RIIC_ICCR2(ch)) | ICCR2_SP;

    g_riic_stage = RIIC_ST_STOP;
    int rc = wait_set(RIIC_ICSR2(ch), ICSR2_STOP);
    REG8(RIIC_ICSR2(ch)) = 0x00U;
    return rc;
}

/* Abandon a transfer: STOP, then clear the flags so the next transaction starts clean. A
 * NACK that is not followed by a STOP leaves the bus owned and every later transfer fails. */
static int abort_transfer(unsigned ch, int rc)
{
    /* Capture where it actually died BEFORE the recovery stop runs, or the stop overwrites the
     * stage and every failure reports as "stalled in stop" — which is never the real cause. */
    g_riic_fail  = g_riic_stage;
    g_riic_icsr2 = REG8(RIIC_ICSR2(ch));
    g_riic_iccr2 = REG8(RIIC_ICCR2(ch));
    g_riic_icmr3 = REG8(RIIC_ICMR3(ch));

    /* WAIT holds SCL low between bytes. If a read failed after asserting it, leaving it set
     * stalls the clock on every later transfer — a single failure would otherwise poison the
     * bus until reset. */
    REG8(RIIC_ICMR3(ch)) = (uint8_t)(REG8(RIIC_ICMR3(ch)) & ~(ICMR3_WAIT | ICMR3_ACKBT));

    (void)issue_stop(ch);
    REG8(RIIC_ICSR2(ch)) = 0x00U;
    return rc;
}

int riic_probe(unsigned ch, uint8_t addr7)
{
    int rc = start_and_address(ch, addr7, 0, 0);
    if (rc != RIIC_OK) return abort_transfer(ch, rc);
    return issue_stop(ch) == RIIC_OK ? RIIC_OK : RIIC_ERR_TIMEOUT;
}

static int write_body(unsigned ch, const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        g_riic_stage = RIIC_ST_TX_TDRE;
        int rc = wait_set_or_nack(ch, ICSR2_TDRE);
        if (rc != RIIC_OK) return rc;
        REG8(RIIC_ICDRT(ch)) = buf[i];
    }
    g_riic_stage = RIIC_ST_TX_TEND;
    return wait_set_or_nack(ch, ICSR2_TEND);
}

int riic_write(unsigned ch, uint8_t addr7, const uint8_t *buf, uint32_t len)
{
    int rc = start_and_address(ch, addr7, 0, 0);
    if (rc != RIIC_OK) return abort_transfer(ch, rc);

    rc = write_body(ch, buf, len);
    if (rc != RIIC_OK) return abort_transfer(ch, rc);

    return issue_stop(ch) == RIIC_OK ? RIIC_OK : RIIC_ERR_TIMEOUT;
}

/* Receive, longhand.
 *
 * The awkwardness is entirely in the last two bytes. Reading ICDRR is what releases SCL for
 * the *next* byte, so the NACK that terminates the transfer has to be armed one byte early —
 * before the read that clocks in the final byte. WAIT holds the clock low after the
 * second-to-last byte so there is time to do that without the hardware running ahead.
 *
 * Get this wrong and the symptom is subtle rather than obvious: transfers appear to work but
 * every buffer is shifted by one byte, or the bus is left held after each read.
 */
static int read_body(unsigned ch, uint8_t *buf, uint32_t len)
{
    if (len == 0) return RIIC_ERR_ARG;

    /* Everything below counts bytes REMAINING, including the one just received, rather than
     * indexing forwards. That is not a style choice — RIIC runs one byte ahead of the
     * programmer. When RDRF signals byte k, the hardware is already clocking byte k+1, so the
     * NACK that ends the transfer has to be armed while byte N-1 is in hand, and WAIT has to
     * be asserted a byte before that to stop the hardware running past the end.
     *
     * Written forwards, the natural-looking "set ACKBT on the last byte" is one byte too late:
     * the final byte gets ACKed, the slave keeps driving the bus, and the STOP never
     * completes. The symptom is a transfer that returns every byte correctly and then times
     * out — which is exactly what this did. */
    REG8(RIIC_ICMR3(ch)) = REG8(RIIC_ICMR3(ch)) | ICMR3_ACKWP;
    REG8(RIIC_ICMR3(ch)) = (uint8_t)(REG8(RIIC_ICMR3(ch)) & ~ICMR3_ACKBT);

    if (len == 1U) {
        /* No byte ever arrives "one before the last", so both have to be set up front. */
        REG8(RIIC_ICMR3(ch)) = REG8(RIIC_ICMR3(ch)) | ICMR3_WAIT;
        REG8(RIIC_ICMR3(ch)) = REG8(RIIC_ICMR3(ch)) | ICMR3_ACKWP | ICMR3_ACKBT;
    } else if (len == 2U) {
        REG8(RIIC_ICMR3(ch)) = REG8(RIIC_ICMR3(ch)) | ICMR3_WAIT;
    }

    (void)REG8(RIIC_ICDRR(ch));            /* dummy read: starts the first data byte */

    for (uint32_t i = 0; i < len; i++) {
        uint32_t remaining = len - i;

        g_riic_stage = RIIC_ST_RX_RDRF;
        int rc = wait_set(RIIC_ICSR2(ch), ICSR2_RDRF);
        if (rc != RIIC_OK) return rc;

        if (remaining == 3U) {
            REG8(RIIC_ICMR3(ch)) = REG8(RIIC_ICMR3(ch)) | ICMR3_WAIT;
        } else if (remaining == 2U) {
            REG8(RIIC_ICMR3(ch)) = REG8(RIIC_ICMR3(ch)) | ICMR3_ACKWP | ICMR3_ACKBT;
        } else if (remaining == 1U) {
            /* The STOP must be requested before ICDRR is read, because that read is what lets
             * the clock run on. */
            REG8(RIIC_ICSR2(ch)) = (uint8_t)(REG8(RIIC_ICSR2(ch)) & ~ICSR2_STOP);
            REG8(RIIC_ICCR2(ch)) = REG8(RIIC_ICCR2(ch)) | ICCR2_SP;
            buf[i] = REG8(RIIC_ICDRR(ch));
            REG8(RIIC_ICMR3(ch)) = (uint8_t)(REG8(RIIC_ICMR3(ch)) & ~ICMR3_WAIT);

            g_riic_stage = RIIC_ST_RX_STOP;
            rc = wait_set(RIIC_ICSR2(ch), ICSR2_STOP);
            REG8(RIIC_ICSR2(ch)) = 0x00U;
            /* Leave ACK as the default so the next transfer does not start by NACKing. */
            REG8(RIIC_ICMR3(ch)) = (uint8_t)(REG8(RIIC_ICMR3(ch)) & ~ICMR3_ACKBT);
            return rc;
        }

        buf[i] = REG8(RIIC_ICDRR(ch));
    }

    return RIIC_OK;
}

int riic_read(unsigned ch, uint8_t addr7, uint8_t *buf, uint32_t len)
{
    int rc = start_and_address(ch, addr7, 1, 0);
    if (rc != RIIC_OK) return abort_transfer(ch, rc);

    rc = read_body(ch, buf, len);
    if (rc != RIIC_OK) return abort_transfer(ch, rc);
    return RIIC_OK;
}

int riic_write_read(unsigned ch, uint8_t addr7,
                    const uint8_t *wbuf, uint32_t wlen,
                    uint8_t *rbuf, uint32_t rlen)
{
    int rc = start_and_address(ch, addr7, 0, 0);
    if (rc != RIIC_OK) return abort_transfer(ch, rc);

    rc = write_body(ch, wbuf, wlen);
    if (rc != RIIC_OK) return abort_transfer(ch, rc);

    rc = start_and_address(ch, addr7, 1, 1);     /* repeated START, no STOP between */
    if (rc != RIIC_OK) return abort_transfer(ch, rc);

    rc = read_body(ch, rbuf, rlen);
    if (rc != RIIC_OK) return abort_transfer(ch, rc);
    return RIIC_OK;
}

/* Set the register pointer, STOP, then a fresh START to read.
 *
 * Not a repeated START. Both devices on this board are ordinary register-mapped parts that
 * accept either form, and this one depends on nothing but the two primitives already proven
 * to work — riic_write and riic_read. riic_write_read remains available for a device that
 * genuinely requires the bus to be held across the turnaround; nothing here does. */
int riic_read_regs(unsigned ch, uint8_t addr7, uint8_t reg, uint8_t *buf, uint32_t len)
{
    int rc = riic_write(ch, addr7, &reg, 1);
    if (rc != RIIC_OK) return rc;
    return riic_read(ch, addr7, buf, len);
}

int riic_write_reg(unsigned ch, uint8_t addr7, uint8_t reg, uint8_t value)
{
    uint8_t b[2] = { reg, value };
    return riic_write(ch, addr7, b, 2);
}
