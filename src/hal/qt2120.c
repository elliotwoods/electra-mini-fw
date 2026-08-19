/* AT42QT2120 capacitive touch sensor, one key per pot.
 *
 * Register map and configuration values follow the stock firmware's config struct
 * (0x0031E4A8) as documented in docs/hardware-notes.md, and match the part's datasheet
 * exactly — which is what let the chip be identified in the first place.
 *
 * The one thing not established by inspection, because it is a board fact rather than a software
 * one: the chip does not answer on I2C at all until P402 is pulsed low. Established on
 * hardware — a scan from cold found nothing, a pulse was applied, and the same scan then found
 * 0x1C every time.
 */

#include "s7g2.h"
#include "bsp.h"
#include "riic.h"
#include "qt2120.h"

#define BUS   RIIC_CH_TOUCHKEY

/* Registers */
#define R_CHIP_ID        0x00
#define R_FW_VERSION     0x01
#define R_DETECTION      0x02
#define R_KEY_STATUS_L   0x03
#define R_KEY_STATUS_H   0x04
#define R_CALIBRATE      0x06
#define R_RESET          0x07
#define R_TTD            0x09   /* towards-touch drift rate, x160 ms */
#define R_ATD            0x0A   /* away-from-touch drift rate, x160 ms */
#define R_LP             0x08   /* scan interval, x16 ms; 0 = free-run */
#define R_DI             0x0B   /* detection integrator: consecutive samples to confirm */
#define R_TRD            0x0C   /* max-on duration before forced recalibration, x160 ms */
#define R_DHT            0x0D   /* drift hold time after a detect, x160 ms */
#define R_DETECT_THRESH  0x10   /* .. 0x1B, one per key */
#define R_KEY_CONTROL    0x1C   /* .. 0x27, one per key */
#define R_KEY_PULSE_SCALE 0x28  /* .. 0x33, per-key gain: high nibble scale, low pulses */
#define R_KEY_SIGNAL     0x34   /* .. 0x4B, 12 x u16 big-endian */
#define R_KEY_REFERENCE  0x4C   /* .. 0x63, 12 x u16 big-endian */

#define QT2120_CHIP_ID   0x3E

/* Pot -> key. The sensor's key order is not the panel's, and neither is the stock firmware's.
 *
 * The stock table (DAT_1FFE14D4) is { 5,7,6,4,3,0,1,2 }. Measured against the real panel it
 * produces a constant rotation of exactly 4 — touching the first knob lit bit 4, the second
 * bit 5, and so on, wrapping. Four is half of eight and the knobs are in two rows of four, so
 * that rotation is a row swap: stock numbers the top row first, while the UI numbers the
 * bottom row first. Apply that row swap here so touch, push, rotation, and displayed fields all
 * use the same physical order.
 */
const unsigned char qt2120_pot_to_key[8] = { 3, 0, 1, 2, 5, 7, 6, 4 };
#define pot_to_key qt2120_pot_to_key

static uint8_t pot_threshold[8] = {
    QT2120_THRESHOLD, QT2120_THRESHOLD, QT2120_THRESHOLD, QT2120_THRESHOLD,
    QT2120_THRESHOLD, QT2120_THRESHOLD, QT2120_THRESHOLD, QT2120_THRESHOLD
};
static uint8_t pot_gain[8];

void qt2120_hw_reset(void)
{
    bsp_pin_cfg(4, 2, PFS_PDR);                  /* P402 low: reset asserted */
    bsp_delay_ms(20);
    bsp_pin_cfg(4, 2, PFS_PDR | PFS_PODR);       /* release */
    bsp_delay_ms(150);                           /* the part needs time before it answers */
}

int qt2120_init(void)
{
    qt2120_hw_reset();

    uint8_t id[2];
    int rc = riic_read_regs(BUS, QT2120_ADDR, R_CHIP_ID, id, sizeof(id));
    if (rc != RIIC_OK) return rc;
    if (id[0] != QT2120_CHIP_ID) return -100;    /* answered, but is not the part we expect */

    /* Software reset on top of the hardware one, so configuration always starts from a known
     * state regardless of what a previous firmware left behind. */
    (void)riic_write_reg(BUS, QT2120_ADDR, R_RESET, 1);
    bsp_delay_ms(200);

    /* Enable the eight wired keys, disable the four that are not.
     * A disabled key still reports noise into the status word if left enabled with nothing
     * attached, which would look exactly like a spurious touch. */
    for (uint8_t k = 0; k < 8; k++) {
        rc = riic_write_reg(BUS, QT2120_ADDR, (uint8_t)(R_KEY_CONTROL + k), 0x00);
        if (rc != RIIC_OK) return rc;
    }
    for (uint8_t k = 8; k < 12; k++) {
        rc = riic_write_reg(BUS, QT2120_ADDR, (uint8_t)(R_KEY_CONTROL + k), 0x01);
        if (rc != RIIC_OK) return rc;
    }

    /* Detection threshold.
     *
     * Stock uses 8, which is sensitive. A low threshold makes a key more likely to cross into
     * detect on an environmental change rather than a finger — and once in detect, the part
     * SUSPENDS drift compensation for that key, so it can never drift back out. That is the
     * mechanism behind keys getting stuck, and it is why the stock firmware has the same
     * problem: nothing recovers the key until the max-on timer fires.
     *
     * 12 is still comfortably sensitive for a finger on a metal knob while leaving more margin
     * against drift. */
    for (uint8_t k = 0; k < 8; k++) {
        rc = riic_write_reg(BUS, QT2120_ADDR, (uint8_t)(R_DETECT_THRESH + k), QT2120_THRESHOLD);
        if (rc != RIIC_OK) return rc;
    }

    /* Drift and recovery behaviour. Stock leaves all of these at reset defaults, which is the
     * other half of the stuck-key problem.
     *
     * TRD forces a recalibration on a key held in detect for TRD x 160 ms. Its default of 255
     * is about 41 seconds — long enough that a latched key feels permanent. But it is left
     * LONG here (63 ~ 10 s) rather than made aggressive, because the chip's timer cannot tell a
     * stuck key from a hand resting on a knob, and recalibrating with a finger present bakes
     * the finger into the reference: one wrong reading traded for another.
     *
     * The software watchdog below does the same job with information the chip does not have —
     * whether the pot is moving, and how far the signal actually is from its reference. TRD is
     * only the backstop for the case where our own polling has stopped.
     *
     * DI is how many consecutive samples must agree before a detect changes state. The default
     * of 4 is kept: raising it would reject brief excursions, but it would equally delay the
     * onset of a real touch, and momentary DROPOUTS are handled in software where the fix can
     * be asymmetric — instant to acquire, reluctant to release. */
    (void)riic_write_reg(BUS, QT2120_ADDR, R_TRD, 63);
    /* DI is how many consecutive scans must agree before detect changes state. At LP=1 each
     * scan is 16 ms, so 4 costs 64 ms before a touch is even reported and the same again on
     * release. Two halves that. It does not need to carry the noise rejection on its own --
     * the software release debounce above is doing that job, and doing it with a measurement
     * behind it rather than a guess. */
    (void)riic_write_reg(BUS, QT2120_ADDR, R_DI,  2);

    /* Scan interval, in units of 16 ms. Written explicitly rather than inherited.
     *
     * Nothing set this before, so the part ran at whatever its reset default happened to be,
     * and the detect latency was DI scans of an interval we had never established. Touch
     * arrived visibly later than rotation from the same knob -- rotation is decoded from the
     * ADC in the service pass, touch has to survive the sensor's own integration first.
     *
     * 1 is the fastest setting: a 16 ms scan, so DI=4 confirms a touch in about 64 ms. The
     * part draws more current this way, which on a mains-powered instrument is not a
     * consideration worth a slower gesture. */
    (void)riic_write_reg(BUS, QT2120_ADDR, R_LP, 1);
    (void)riic_write_reg(BUS, QT2120_ADDR, R_TTD, 20);
    (void)riic_write_reg(BUS, QT2120_ADDR, R_ATD, 5);
    /* Drift Hold Time: how long reference drifting stays suspended after a detect.
     *
     * The default of 25 is about 4 seconds — after which the reference resumes drifting TOWARD
     * the touched signal even though the finger is still there. The delta shrinks, crosses back
     * under the threshold, and the touch is dropped mid-gesture. That is the dropout, and it
     * explains why it was worst during rotation: rotations are simply the long gestures.
     *
     * Held at maximum (about 41 s) so the reference cannot creep up under a finger. This is
     * only safe because TRD is SHORTER (10 s): a key that latches spuriously is recalibrated by
     * the max-on timer well before the drift hold would have rescued it, so lengthening the
     * hold cannot make sticking worse. The two settings have to be read together. */
    (void)riic_write_reg(BUS, QT2120_ADDR, R_DHT, 255);

    /* Calibrate last, with nothing touching the panel — the references it captures here are
     * what every later detection is measured against. */
    rc = riic_write_reg(BUS, QT2120_ADDR, R_CALIBRATE, 1);
    if (rc != RIIC_OK) return rc;
    bsp_delay_ms(200);

    return RIIC_OK;
}

int qt2120_read_keys(uint16_t *keys_out)
{
    /* One 5-byte read from register 0 gets identity, detection status and both key bytes in a
     * single transaction, which is how the stock firmware polls it too. */
    uint8_t b[5];
    int rc = riic_read_regs(BUS, QT2120_ADDR, R_CHIP_ID, b, sizeof(b));
    if (rc != RIIC_OK) return rc;

    *keys_out = (uint16_t)(b[3] | ((uint16_t)b[4] << 8));
    return RIIC_OK;
}

int qt2120_read_touch(uint16_t *mask_out)
{
    uint16_t keys;
    int rc = qt2120_read_keys(&keys);
    if (rc != RIIC_OK) return rc;

    uint16_t pots = 0;
    for (uint8_t p = 0; p < 8; p++) {
        if (keys & (1U << pot_to_key[p])) pots |= (uint16_t)(1U << p);
    }
    *mask_out = pots;
    return RIIC_OK;
}

/* Little-endian u16 pairs.
 *
 * Earlier notes said big-endian. Read that way every value came back
 * with its high byte in the 0x02-0x03 range and its low byte varying wildly — 0x3903, 0xCF02,
 * 0xB602 — which is exactly the signature of a byte swap, since real signal levels here are a
 * few hundred counts. Read little-endian the same bytes give 825, 719, 694: sensible, stable,
 * and equal to their references at rest, which is what drift compensation should produce.
 *
 * Worth stating because a swapped u16 does not fail, it merely lies plausibly. */
static void le16_block(const uint8_t *raw, uint16_t *out, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        out[i] = (uint16_t)((uint16_t)raw[2 * i] | ((uint16_t)raw[2 * i + 1] << 8));
    }
}

int qt2120_read_detail(qt2120_detail_t *out)
{
    uint8_t raw[24];
    int rc;

    rc = riic_read_regs(BUS, QT2120_ADDR, R_KEY_SIGNAL, raw, sizeof(raw));
    if (rc != RIIC_OK) return rc;
    le16_block(raw, out->signal, 12);

    rc = riic_read_regs(BUS, QT2120_ADDR, R_KEY_REFERENCE, raw, sizeof(raw));
    if (rc != RIIC_OK) return rc;
    le16_block(raw, out->reference, 12);

    return qt2120_read_keys(&out->keys);
}

int qt2120_recalibrate(void)
{
    int rc = riic_write_reg(BUS, QT2120_ADDR, R_CALIBRATE, 1);
    if (rc != RIIC_OK) return rc;
    bsp_delay_ms(200);
    return RIIC_OK;
}

int qt2120_set_threshold(uint8_t key, uint8_t threshold)
{
    if (key >= 12u || threshold == 0u) return -101;
    return riic_write_reg(BUS, QT2120_ADDR, (uint8_t)(R_DETECT_THRESH + key), threshold);
}

/* Touch conditioning: the two failure modes need opposite treatments.
 *
 * Reported symptoms, both of which the stock firmware also has:
 *
 *   1. A key sticks ON. The part suspends drift compensation while a key is in detect, so a
 *      key that crosses the threshold on an environmental change can never drift back out.
 *   2. A key drops OFF while the knob is genuinely being held — especially during rotation,
 *      as the finger rolls across the knob and the contact area momentarily shrinks.
 *
 * Treating either alone makes the other worse: raising the threshold cures sticking and
 * aggravates dropping; lowering it does the reverse. So neither is treated with the threshold.
 *
 * Instead, two mechanisms that use information the sensor does not have:
 *
 *   ROTATION IMPLIES TOUCH. A pot physically cannot turn without a hand on it. So a moving pot
 *   asserts its own touch bit regardless of what the sensor says, which fixes symptom 2 exactly
 *   where it was reported — during rotation — without desensitising anything.
 *
 *   RELEASE IS DEBOUNCED, ACQUISITION IS NOT. Touch is reported the instant the sensor sees it,
 *   because focus latency is felt. Release is only reported after the sensor has agreed for
 *   QT2120_RELEASE_MS AND the pot has stopped moving. A dropout shorter than that never reaches
 *   anything upstream.
 *
 * And for symptom 1, a watchdog that recalibrates a key which claims touch while its pot has
 * not moved for a long time AND whose signal sits only marginally past its threshold — a real
 * finger gives a delta far larger than the threshold, a latched key typically sits just past
 * it. Requiring both conditions is what stops the watchdog firing on somebody simply resting a
 * hand on a knob.
 */
/* Runtime-tunable, because "how long before release registers" is a feel judgement that has to
 * be made on the device, not in a header. Too short and a dropout mid-gesture leaks through;
 * too long and letting go of a knob feels sticky. */
static uint32_t release_ms = QT2120_RELEASE_MS;

void     qt2120_set_release_ms(uint32_t ms) { release_ms = ms; }
uint32_t qt2120_get_release_ms(void)        { return release_ms; }

uint16_t qt2120_touch_filter(uint16_t raw, uint16_t moved, uint32_t dt_ms)
{
    static uint16_t stable;
    static uint32_t released_ms[8];
    static uint32_t held_ms[8];

    for (unsigned p = 0; p < 8; p++) {
        uint16_t bit = (uint16_t)(1u << p);

        /* Touch comes from the sensor alone. `moved` is deliberately NOT consulted.
         *
         * The rule used to be "a pot cannot turn without a hand on it", so movement asserted
         * touch and repaired a dropped reading. It was a workaround for a sensor we did not
         * trust, and it became the mechanism of the fault it was meant to fix: the movement
         * detector had a deadband four times too small for a 12-bit ADC and compared samples
         * taken eight passes apart, so it produced false movement, which latched a touch, which
         * made the scanner dwell on that knob, which kept the latch alive. The sensor read a
         * clean zero throughout -- every per-key delta 0 or 1 against a threshold of 12 -- so
         * the one input that was telling the truth was the one being overridden.
         *
         * With the sensor tuned and the deadband corrected there is nothing left to repair, and
         * a rule that can invent a touch is worse than one that occasionally misses a release. */
        if (raw & bit) {
            stable |= bit;                       /* acquire immediately */
            released_ms[p] = 0;
        } else if (stable & bit) {
            released_ms[p] += dt_ms;
            if (released_ms[p] >= release_ms) {
                stable &= (uint16_t)~bit;
                released_ms[p] = 0;
            }
        }

        /* Stuck detection runs on the RAW bit: a hold created by our own rotation rule is by
         * definition not stuck. */
        if ((raw & bit) && !(moved & bit)) held_ms[p] += dt_ms;
        else                               held_ms[p] = 0;
    }

    /* Only consider recalibrating when nothing is moving — recalibrating mid-gesture would
     * capture a finger into the reference. */
    if (moved != 0) return stable;

    int suspect = 0;
    for (unsigned p = 0; p < 8; p++) {
        if (held_ms[p] >= QT2120_STUCK_MS) { suspect = 1; break; }
    }
    if (!suspect) return stable;

    /* Confirm against the sensor's own numbers before acting. A genuine touch shows a delta
     * far past the threshold; a latched key sits just past it. */
    qt2120_detail_t d;
    if (qt2120_read_detail(&d) == RIIC_OK) {
        int marginal = 1;
        for (unsigned p = 0; p < 8; p++) {
            if (held_ms[p] < QT2120_STUCK_MS) continue;
            uint8_t k = pot_to_key[p];
            int delta = (int)d.reference[k] - (int)d.signal[k];
            if (delta < 0) delta = -delta;

            /* MAGNITUDE, not sign. Measured on the panel, a touch drives the signal ABOVE the
             * reference, so `reference - signal` is negative during a real touch — the opposite
             * of the convention assumed when this was written. Comparing the signed value would
             * have made the test never pass, so the watchdog would have judged every genuine
             * touch "marginal" and recalibrated it away. */
            if (delta > 2 * QT2120_THRESHOLD) marginal = 0;   /* looks like a real finger */
        }
        if (!marginal) {
            for (unsigned p = 0; p < 8; p++) held_ms[p] = 0;  /* believe it; stop nagging */
            return stable;
        }
    }

    (void)qt2120_recalibrate();
    for (unsigned p = 0; p < 8; p++) { held_ms[p] = 0; released_ms[p] = 0; }
    stable = 0;
    return stable;
}

int qt2120_set_gain(uint8_t key, uint8_t pulse_scale)
{
    if (key >= 12) return -1;
    return riic_write_reg(BUS, QT2120_ADDR, (uint8_t)(R_KEY_PULSE_SCALE + key), pulse_scale);
}

int qt2120_apply_pot_config(uint8_t pot, uint8_t threshold, uint8_t pulse_scale)
{
    if (pot >= 8u || threshold == 0u) return -101;
    uint8_t pulse = (uint8_t)(pulse_scale >> 4);
    uint8_t scale = (uint8_t)(pulse_scale & 0x0Fu);
    if (pulse > 14u || scale > 7u || pulse > (uint8_t)(scale + 6u)) return -101;

    uint8_t key = pot_to_key[pot];
    int rc = qt2120_set_gain(key, pulse_scale);
    if (rc != RIIC_OK) return rc;
    rc = qt2120_set_threshold(key, threshold);
    if (rc == RIIC_OK) {
        pot_threshold[pot] = threshold;
        pot_gain[pot] = pulse_scale;
    }
    return rc;
}

uint8_t qt2120_threshold_for_pot(uint8_t pot)
{
    return pot < 8u ? pot_threshold[pot] : QT2120_THRESHOLD;
}

uint8_t qt2120_gain_for_pot(uint8_t pot)
{
    return pot < 8u ? pot_gain[pot] : QT2120_DEFAULT_GAIN;
}

/* Peak-hold, so measuring a touch needs no coordination.
 *
 * Three attempts to capture a touch inside a timed window recorded nothing, because the window
 * and the person are in different places. The firmware is in neither: it can watch continuously
 * and remember the largest delta each key has seen, and the measurement can then be collected
 * whenever. This is strictly better instrumentation than a synchronised capture, and it should
 * have been the first approach rather than the fourth. */
static uint16_t peak_pos[8];     /* max(reference - signal) */
static uint16_t peak_neg[8];     /* max(signal - reference), in case touch raises the count */
static uint16_t last_delta[8];
static uint16_t seen_keys;       /* OR of every detect bit observed since the last reset */
static uint32_t track_calls;     /* proves the tracker is running at all */

void qt2120_track_peaks(void)
{
    qt2120_detail_t d;
    if (qt2120_read_detail(&d) != RIIC_OK) return;

    track_calls++;
    seen_keys |= d.keys;

    for (unsigned p = 0; p < 8; p++) {
        unsigned k = pot_to_key[p];
        int delta = (int)d.reference[k] - (int)d.signal[k];

        /* Both directions are tracked because the sign convention is an assumption, not a
         * measurement: a touch raises capacitance, which SHOULD lower the count, but clamping
         * a negative delta to zero would hide the opposite convention completely — and a
         * measurement that can only confirm what you already believe is not a measurement. */
        if (delta >= 0) {
            if ((uint16_t)delta > peak_pos[p]) peak_pos[p] = (uint16_t)delta;
            last_delta[p] = (uint16_t)delta;
        } else {
            if ((uint16_t)(-delta) > peak_neg[p]) peak_neg[p] = (uint16_t)(-delta);
            last_delta[p] = (uint16_t)(-delta);
        }
    }
}

void qt2120_get_peaks(uint16_t out_pos[8], uint16_t out_neg[8], uint16_t out_last[8],
                      uint16_t *out_seen, uint32_t *out_calls, int reset)
{
    for (unsigned p = 0; p < 8; p++) {
        if (out_pos)  out_pos[p]  = peak_pos[p];
        if (out_neg)  out_neg[p]  = peak_neg[p];
        if (out_last) out_last[p] = last_delta[p];
    }
    if (out_seen)  *out_seen  = seen_keys;
    if (out_calls) *out_calls = track_calls;

    if (reset) {
        for (unsigned p = 0; p < 8; p++) { peak_pos[p] = 0; peak_neg[p] = 0; }
        seen_keys = 0;
        track_calls = 0;
    }
}
