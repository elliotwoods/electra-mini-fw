/* Multiplexed inputs: pots, pot switches, front-panel buttons.
 *
 * Everything hangs off one 8:1 multiplexer. Its three address lines are P900, P901 and P905 —
 * note they are NOT contiguous, which is why the stock firmware packs the channel number as
 * `(ch & 4) << 3 | (ch & 3)` before writing a port: that expression puts channel bit 2 onto
 * port bit 5. Here the three lines are driven individually instead, which says the same thing
 * without the packing trick and cannot be misread.
 *
 * P314 is the strobe. In the pinmap tool's hex notation that pad is called P30E — see the
 * note in docs/hardware-notes.md about decimal versus hex pin numbering, which has already
 * caused one wrong diagnosis in this tree.
 *
 * The pots are two-track endless potentiometers, not encoders and not absolute pots: each has
 * two wipers producing overlapping ramps, and rotation is recovered from how the pair moves
 * rather than from either alone. Both phases are therefore sampled for the same mux channel
 * before moving on, which is why the two ADC units are read back to back below.
 */

#include "s7g2.h"
#include "bsp.h"
#include "inputs.h"

/* Mux address lines and strobe. */
#define MUX_A0_PORT   9
#define MUX_A0_PIN    0
#define MUX_A1_PORT   9
#define MUX_A1_PIN    1
#define MUX_A2_PORT   9
#define MUX_A2_PIN    5
#define MUX_STB_PORT  3
#define MUX_STB_PIN   14

/* Sense lines. Bank assignment is the stock firmware's: the pot pushes and the front-panel
 * buttons are separate pins sharing the same mux address. */
#define SENSE_POT_PORT   0
#define SENSE_POT_PIN    3
#define SENSE_BTN_PORT   0
#define SENSE_BTN_PIN    4

/* S12AD units. Two of them, because the two pot phases are sampled simultaneously — a
 * sequential pair would smear rotation across the sample interval and the quadrant decode
 * depends on the two agreeing about the same instant. */
#define ADC0_BASE     0x4005C000UL
#define ADC1_BASE     0x4005C200UL

#define ADC_ADCSR(b)    ((b) + 0x00UL)   /* 16-bit: b15 ADST */
#define ADC_ADANSA0(b)  ((b) + 0x04UL)   /* 16-bit: channel select */
#define ADC_ADCER(b)    ((b) + 0x0EUL)   /* 16-bit: format */
#define ADC_ADDR(b, ch) ((b) + 0x20UL + 2UL * (ch))

#define ADCSR_ADST      0x8000U

/* Sampling-state registers, one per channel, 8-bit, at 0xE0 + channel.
 *
 * Located by dumping the peripheral's address space and spotting the run of 0x0B — the reset
 * default of 11 states — then confirmed by experiment rather than assertion: writing 0x20,
 * 0x80 and 0xFF moved the measured conversion cost to 572, 1332 and 2372 cycles per pair from
 * a baseline of 412, linear at ~4 core cycles per state per conversion. A datasheet we do not
 * have could not have been more convincing than that. */
#define ADC_ADSSTR(b, ch)  ((b) + 0xE0UL + (ch))

/* Which ADC unit and channel each pot phase lands on. Confirmed by measurement: with the
 * multiplexer parked, these are the two channels whose readings track a turning knob. */
#define PHASE_A_UNIT  1
#define PHASE_A_CH    5
#define PHASE_B_UNIT  0
#define PHASE_B_CH    6

/* --- rotation decode ------------------------------------------------------
 *
 * MEASURED, not ported. tools/deploy/potcap.py captured 4,993 samples of both tracks across
 * roughly six revolutions of one knob, and the (A,B) scatter is a clean DIAMOND: two triangle
 * tracks a quarter-cycle apart, so |A-2048| + |B-2048| is constant (measured median 2063).
 *
 * That shape gives an exact position rather than an inferred direction. Take `db` as the
 * position within a quadrant and the sign of `da` to say which quadrant, and the result is a
 * single monotonic phase covering 8192 counts per revolution. Validated against the capture: a
 * one-direction turn produced 48,646 counts forward against 1,240 backward -- an error floor of
 * 2.55%, with the worst single backward step at 59 counts, well under one detent.
 *
 * This replaces a placeholder that read phase A alone and derived direction from its sign. That
 * could not work: on a two-track pot each track descends for half of every revolution, so
 * single-track sign is wrong half the time by construction.
 */

#define PH_CENTRE   2048            /* both tracks swing about mid-scale on a 12-bit ADC */
#define PH_R        2048            /* the diamond's half-diagonal */
#define PH_PER_REV  (4 * PH_R)      /* 8192 counts of phase per revolution */

/* Counts of phase per detent. A feel parameter, not a measurement: these pots are continuous
 * and have no physical detent, so the number is ours to choose. 128 gives 64 detents per
 * revolution -- fine enough to place a digit without hunting, coarse enough that paging at four
 * detents a page does not fly past the surface. Tune here, on hardware, not by arithmetic. */
#define PH_PER_DETENT 128

/* Noise gate on a single step. The tracks measure about +/-3 counts at rest, and the quadrant
 * corners contribute a few more where the sign of `da` is briefly ambiguous. Below this a step
 * is not motion, and letting it into the accumulator would let a still knob drift. */
#define PH_DEADBAND 6

static int32_t phase_of(uint16_t a, uint16_t b)
{
    int32_t da = (int32_t)a - PH_CENTRE;
    int32_t db = (int32_t)b - PH_CENTRE;

    /* Clamping db is what makes the quadrant boundaries seamless. At a corner db sits at its
     * extreme and `da` is near zero, so the sign that picks the branch is noisy -- and the two
     * branches disagree by twice the overshoot. Clamped, they agree exactly at the corner, and
     * the phase is continuous all the way round. */
    if (db >  PH_R) db =  PH_R;
    if (db < -PH_R) db = -PH_R;

    if (db >= 0) return (da >= 0) ? db : (2 * PH_R - db);
    return (da < 0) ? (2 * PH_R - db) : (4 * PH_R + db);
}

int32_t inputs_decode_rotation(unsigned pot, uint16_t phase_a, uint16_t phase_b)
{
    static int32_t prev[INPUT_POTS];
    static int32_t accum[INPUT_POTS];
    static uint8_t primed;                  /* one bit per pot */

    if (pot >= INPUT_POTS) return 0;

    int32_t p = phase_of(phase_a, phase_b);

    /* Seed each pot from its OWN first reading. Per pot, not once globally: a global flag would
     * seed all eight from whichever was sampled first and hand the other seven a phantom
     * half-revolution the moment they were first read. */
    if (!(primed & (1u << pot))) {
        prev[pot] = p;
        primed |= (uint8_t)(1u << pot);
        return 0;
    }

    int32_t d = p - prev[pot];
    prev[pot] = p;

    /* The phase is circular, so the short way round is the real movement. Without this a
     * wrap read as almost a full revolution in the wrong direction. */
    if (d >  PH_PER_REV / 2) d -= PH_PER_REV;
    if (d < -PH_PER_REV / 2) d += PH_PER_REV;

    if (d > -PH_DEADBAND && d < PH_DEADBAND) return 0;

    /* Accumulate the remainder rather than discarding it, so a slow turn still registers. The
     * placeholder divided each sample independently, so movement below one detent per sample
     * was thrown away and a careful turn moved nothing at all. */
    accum[pot] += d;
    int32_t ticks = accum[pot] / PH_PER_DETENT;
    if (ticks) accum[pot] -= ticks * PH_PER_DETENT;
    return ticks;
}

void inputs_init(void)
{
    REG32(MSTPCRD) = REG32(MSTPCRD) & ~(MSTPD_S12AD0 | MSTPD_S12AD1);

    /* Single-scan mode, right-aligned 12-bit, no trigger: we convert on demand. Free-running
     * would be faster but a synchronous read is far easier to reason about, and the whole
     * input scan only needs to run at about 12 ms as it does in stock. */
    REG16(ADC_ADCSR(ADC0_BASE)) = 0x0000U;
    REG16(ADC_ADCSR(ADC1_BASE)) = 0x0000U;
    REG16(ADC_ADCER(ADC0_BASE)) = 0x0000U;
    REG16(ADC_ADCER(ADC1_BASE)) = 0x0000U;

    /* The mux address lines and strobe are already outputs from the recovered pin table; set
     * a defined starting state rather than inheriting whatever the bootloader left. */
    PORT_CLR(MUX_A0_PORT, MUX_A0_PIN);
    PORT_CLR(MUX_A1_PORT, MUX_A1_PIN);
    PORT_CLR(MUX_A2_PORT, MUX_A2_PIN);
    PORT_SET(MUX_STB_PORT, MUX_STB_PIN);
}

uint16_t inputs_adc_raw(unsigned unit, unsigned ch)
{
    uint32_t base = unit ? ADC1_BASE : ADC0_BASE;

    REG16(ADC_ADANSA0(base)) = (uint16_t)(1U << ch);
    REG16(ADC_ADCSR(base))   = ADCSR_ADST;

    /* Bounded, like every wait in this tree. A conversion that never completes must report a
     * wrong number, not stop the firmware. */
    for (uint32_t t = 0; t < 100000UL; t++) {
        if (!(REG16(ADC_ADCSR(base)) & ADCSR_ADST)) break;
    }
    return (uint16_t)(REG16(ADC_ADDR(base, ch)) & 0x0FFFU);
}

/* Rotation channel order, measured on the panel. See inputs.h for why this differs from the
 * switch mapping. */
const unsigned char inputs_mux_to_pot_rotation[8] = { 4, 0, 1, 5, 2, 7, 3, 6 };

/* Front-panel buttons 1..3 are wired in reverse; 4..6 are in order. */
const unsigned char inputs_button_map[6] = { 2, 1, 0, 3, 4, 5 };

/* Settling, and how many conversions to throw away before believing one.
 *
 * Turning a knob was visibly moving the NEXT multiplexer channel's reading as well as its own,
 * in all eight cases without exception. That is not crosstalk in the wiring — it is the ADC
 * sample-and-hold arriving at a new channel still carrying charge from the previous one. The
 * pot wiper and the multiplexer's on-resistance are in series with the sampling capacitor, so
 * the charge time is much longer than a default sampling window allows.
 *
 * Two independent remedies, because either alone may be marginal: wait longer after switching
 * the multiplexer, and convert more than once, discarding all but the last — each conversion
 * gives the capacitor another sampling window to reach the new voltage.
 *
 * Runtime-tunable so the minimum can be found by measurement on the bench instead of picked
 * out of the air; `settle` on the console drives it. */
static uint32_t settle_us = 150;
static uint32_t discards  = 1;
static uint8_t  sampling  = 0x0B;      /* reset default; raised by inputs_init */

void inputs_set_settle(uint32_t us, uint32_t d)
{
    settle_us = us;
    discards  = (d > 8) ? 8 : d;
}
uint32_t inputs_get_settle_us(void) { return settle_us; }
uint32_t inputs_get_discards(void)  { return discards; }

void inputs_set_sampling(uint8_t states)
{
    sampling = states;
    REG8(ADC_ADSSTR(ADC1_BASE, PHASE_A_CH)) = states;
    REG8(ADC_ADSSTR(ADC0_BASE, PHASE_B_CH)) = states;
}
uint8_t inputs_get_sampling(void) { return sampling; }

/* How the strobe pin is driven around a conversion.
 *
 *   0  pulse low, leave HIGH during the conversion   (the original guess)
 *   1  pulse high, leave LOW during the conversion
 *   2  leave it alone entirely — address lines only
 *
 * Which is right depends on whether P314 is a latch enable or an output enable, and the
 * analysis cannot say: it shows a helper that clears the pin and another that sets it,
 * with no indication of what the part does in between. If it is an OUTPUT enable held in the
 * disabled state during the conversion, the analog node floats and the ADC samples leftover
 * charge — which would be contamination that no amount of extra settling time can fix.
 *
 * That is exactly the signature measured: predecessor-dependent, and completely unchanged by
 * settle times from 0 to 2000 us. So this is made switchable and measured rather than argued
 * about. */
static unsigned mux_mode = 1;   /* measured: LOW during conversion. See docs. */

void inputs_set_mux_mode(unsigned m) { mux_mode = m; }
unsigned inputs_get_mux_mode(void)   { return mux_mode; }

void inputs_mux_select(unsigned ch)
{
    if (ch & 1U) PORT_SET(MUX_A0_PORT, MUX_A0_PIN); else PORT_CLR(MUX_A0_PORT, MUX_A0_PIN);
    if (ch & 2U) PORT_SET(MUX_A1_PORT, MUX_A1_PIN); else PORT_CLR(MUX_A1_PORT, MUX_A1_PIN);
    if (ch & 4U) PORT_SET(MUX_A2_PORT, MUX_A2_PIN); else PORT_CLR(MUX_A2_PORT, MUX_A2_PIN);

    switch (mux_mode) {
    case 1:
        PORT_SET(MUX_STB_PORT, MUX_STB_PIN);
        bsp_delay_us(5);
        PORT_CLR(MUX_STB_PORT, MUX_STB_PIN);      /* stays LOW through the conversion */
        break;
    case 2:
        break;                                     /* address lines only */
    default:
        PORT_CLR(MUX_STB_PORT, MUX_STB_PIN);
        bsp_delay_us(5);
        PORT_SET(MUX_STB_PORT, MUX_STB_PIN);      /* stays HIGH through the conversion */
        break;
    }
    bsp_delay_us(settle_us);
}

/* (definitions moved above; kept here for the explanation) */
/* Which ADC unit and channel each pot phase lands on.
 *
 * Prior analysis says phase A is S12AD1 channel 5 and phase B is S12AD0 channel 6, and the
 * pin table has exactly two analog pads, P014 and P015 (P00E/P00F in the pinmap tool's hex
 * notation). Which phase is on which pad was listed as an open question in the plan, and it is
 * resolved by measurement rather than assumption: `adcscan` on the console converts every
 * channel of both units, and the two that move when a knob is turned are the answer.
 */
void inputs_read_phases(uint16_t *a, uint16_t *b)
{
    /* Discard conversions before believing one: each pass re-samples, so the extra passes are
     * what let the sample-and-hold finish charging to the newly selected channel. */
    for (uint32_t i = 0; i < discards; i++) {
        (void)inputs_adc_raw(PHASE_A_UNIT, PHASE_A_CH);
        (void)inputs_adc_raw(PHASE_B_UNIT, PHASE_B_CH);
    }
    if (a) *a = inputs_adc_raw(PHASE_A_UNIT, PHASE_A_CH);
    if (b) *b = inputs_adc_raw(PHASE_B_UNIT, PHASE_B_CH);
}

void inputs_read_senses(int *pot_switch, int *button)
{
    if (pot_switch) *pot_switch = (int)PORT_GET(SENSE_POT_PORT, SENSE_POT_PIN);
    if (button)     *button     = (int)PORT_GET(SENSE_BTN_PORT, SENSE_BTN_PIN);
}

/* One pass over every multiplexer channel, converted into panel ordering on the way out.
 *
 * A single pass reads all three input classes, because they share the multiplexer: setting a
 * channel presents that knob's two phases to the ADCs and its switch to one sense pin, with a
 * front-panel button on the other. Reading them separately would mean eight extra mux settles
 * for nothing, and the settle is the expensive part.
 */
void inputs_sample(input_sample_t *out)
{
    out->push_mask = 0;
    out->button_mask = 0;

    for (unsigned ch = 0; ch < INPUT_POTS; ch++) {
        inputs_mux_select(ch);

        /* Three different mappings off the same channel, all measured on the panel:
         * rotation is scrambled, the push switch is a row swap, and the buttons have their
         * first three reversed. Applying each here means nothing above the HAL ever sees the
         * multiplexer's ordering. */
        inputs_read_phases(&out->phase_a[inputs_mux_to_pot_rotation[ch]],
                           &out->phase_b[inputs_mux_to_pot_rotation[ch]]);

        int sw, bt;
        inputs_read_senses(&sw, &bt);
        if (!sw) out->push_mask   |= (uint16_t)(1u << inputs_mux_to_pot(ch));   /* active low */
        if (!bt && ch < INPUT_BUTTONS) out->button_mask |= (uint16_t)(1u << inputs_button_map[ch]);
    }
}

/* Rotation: panel pot -> multiplexer channel. The inverse of inputs_mux_to_pot_rotation. */
static const unsigned char pot_to_mux_rotation[8] = { 1, 2, 4, 6, 0, 3, 7, 5 };

/* Digital-only channel select. The strobe gates the ANALOG path, so the button and switch
 * senses need no settling at all — measured identical with the strobe either way. Skipping the
 * analog settle here is what makes a full digital sweep cost tens of microseconds instead of
 * more than a millisecond. */
static void mux_select_digital(unsigned ch)
{
    if (ch & 1U) PORT_SET(MUX_A0_PORT, MUX_A0_PIN); else PORT_CLR(MUX_A0_PORT, MUX_A0_PIN);
    if (ch & 2U) PORT_SET(MUX_A1_PORT, MUX_A1_PIN); else PORT_CLR(MUX_A1_PORT, MUX_A1_PIN);
    if (ch & 4U) PORT_SET(MUX_A2_PORT, MUX_A2_PIN); else PORT_CLR(MUX_A2_PORT, MUX_A2_PIN);
    bsp_delay_us(2);
}

/* Persistent view of the pots, because a service pass only refreshes one of them. */
static input_sample_t cache;

/* One service pass. Cheap enough to call from the main loop at a few hundred hertz.
 *
 * The strategy follows from two measurements. First, contamination only ever appears on the
 * FIRST sample after the multiplexer moves — hold it still and the readings are at the noise
 * floor. Second, settling that node costs on the order of a hundred microseconds, so sweeping
 * all eight channels accurately every pass is the expensive thing, and it is also unnecessary.
 *
 * A knob cannot be turned without being touched, and the capacitive sensor already says which
 * knob a hand is on. So: sweep the digital senses every pass, since they are nearly free and
 * every button matters at all times, then spend the analog budget on the one knob actually in
 * use. When nothing is touched, advance a slow round-robin instead so baselines stay fresh —
 * absolute position is meaningless on an endless pot anyway, only change matters, and change
 * only happens under a finger.
 *
 * The result is full accuracy exactly where it is needed, at a far higher rate than a fair
 * round-robin could give, for about a fifth of a millisecond per pass.
 */
/* Debounce for the two mechanical switch banks.
 *
 * Neither bank had any, and until now that only cost a duplicated toggle. It stops being
 * survivable once a pot press means "drill into this field": a single contact bounce sampled at
 * the wrong moment drills in and straight back out, which is the most visible failure the panel
 * can produce.
 *
 * A state changes only after reading the same way on two consecutive passes. At a 20 ms service
 * period that is 20-40 ms of agreement, which is far longer than switch bounce and far shorter
 * than a deliberate press, so it costs nothing a hand can feel.
 *
 * `settled` is seeded from the first sweep rather than from zero. Starting at zero makes a
 * switch that is already held at power-on look like a fresh press on the first pass. */
static uint16_t debounce(uint16_t sampled, uint16_t *settled, uint16_t *pending, uint8_t *primed)
{
    if (!*primed) { *settled = sampled; *pending = sampled; *primed = 1; return sampled; }

    uint16_t agreed = (uint16_t)~(sampled ^ *pending);   /* bits that read the same twice */
    *settled = (uint16_t)((*settled & ~agreed) | (sampled & agreed));
    *pending = sampled;
    return *settled;
}

void inputs_service(uint16_t touch_mask, input_sample_t *out)
{
    uint16_t push_now = 0, btn_now = 0;

    for (unsigned ch = 0; ch < INPUT_POTS; ch++) {
        mux_select_digital(ch);
        int sw, bt;
        inputs_read_senses(&sw, &bt);
        if (!sw) push_now |= (uint16_t)(1u << inputs_mux_to_pot(ch));
        if (!bt && ch < INPUT_BUTTONS) btn_now |= (uint16_t)(1u << inputs_button_map[ch]);
    }

    static uint16_t push_settled, push_pending, btn_settled, btn_pending;
    static uint8_t  push_primed, btn_primed;
    cache.push_mask   = debounce(push_now, &push_settled, &push_pending, &push_primed);
    cache.button_mask = debounce(btn_now,  &btn_settled,  &btn_pending,  &btn_primed);

    /* Sample EVERY pot's phases, every pass.
     *
     * This used to dwell on whichever knob was touched and round-robin the rest one per pass,
     * on the reasoning that only one knob is turned at a time. That is true of a surface where
     * eight knobs are eight fields, and false of this one: the bottom row selects a field and
     * the TOP row edits its digits, so a hand on a bottom dial and a hand on a top dial is the
     * ordinary case. With the dwell in place the top row was never sampled while a field was
     * held, so digit knobs appeared to work once and then die.
     *
     * It also made movement detection unreliable in a way that was hard to see. Round-robin
     * meant a pot was compared against a reading from eight passes ago, so drift and residual
     * multiplexer coupling accumulated across that gap and could clear the deadband on a knob
     * nobody had touched.
     *
     * The cost is one full multiplexer sweep of the analog inputs, about 12 ms, against 40 ms
     * of service period. Worth it: every knob is now compared against a reading from one pass
     * ago, which is both more responsive and far less prone to false movement. */
    cache.moved_mask = 0;
    (void)touch_mask;

    for (unsigned p = 0; p < INPUT_POTS; p++) {
        inputs_mux_select(pot_to_mux_rotation[p]);
        inputs_read_phases(&cache.phase_a[p], &cache.phase_b[p]);

        /* One decode per pot per pass, here and nowhere else. The decoder accumulates the
         * sub-detent remainder, so calling it twice for the same sample would double-count the
         * movement -- which is why the result travels in the sample rather than being
         * recomputed by whoever wants it. */
        cache.detents[p] = inputs_decode_rotation(p, cache.phase_a[p], cache.phase_b[p]);
        if (cache.detents[p]) cache.moved_mask |= (uint16_t)(1u << p);
    }

    if (out) *out = cache;
}
