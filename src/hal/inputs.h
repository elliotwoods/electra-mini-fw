#ifndef ELECTRA_INPUTS_H
#define ELECTRA_INPUTS_H

#include <stdint.h>

/* The multiplexed input tree: 8 endless pots, 8 pot push-switches, 6 front-panel buttons.
 *
 * One 8:1 analog multiplexer serves all three. Its address lines select a channel, and that
 * channel's two pot phases appear on the two analog pins while its pot-switch and button
 * sense lines appear on two digital pins. So a full input scan is eight mux settings, not
 * eight separate readings of twenty-two things.
 */

#define INPUT_POTS     8
#define INPUT_BUTTONS  6

/* Panel pot index (0 = bottom-left) <-> multiplexer channel.
 *
 * There are TWO mappings, and they are genuinely different — which is the single most
 * counter-intuitive thing about this input tree.
 *
 *   - Capacitive touch and the pot push switches use a plain row swap, `(n + 4) mod 8`.
 *   - Pot ROTATION is scrambled: `{4,0,1,5,2,7,3,6}` indexed by multiplexer channel.
 *
 * Both are measured on the panel, not inferred. The rotation table is exactly the stock
 * firmware's `{0,4,5,1,6,3,7,2}` with the same +4 row swap applied, so both paths use the
 * UI's bottom-row-first physical order.
 *
 * An earlier pass here concluded the opposite — that one mapping served both — from noticing
 * that pushes and phase movements appeared on the same channel in an interleaved capture. That
 * was a coincidence of timing, not evidence. Turning each knob and watching which cell moved
 * settled it properly.
 */
static inline unsigned inputs_pot_to_mux(unsigned pot) { return (pot + 4u) & 7u; }
static inline unsigned inputs_mux_to_pot(unsigned ch)  { return (ch  + 4u) & 7u; }

/* Rotation only. Not derivable from the switch mapping; see above. */
extern const unsigned char inputs_mux_to_pot_rotation[8];

/* Front-panel buttons: the first three are wired in reverse order. Self-inverse, so the same
 * table converts either way. */
extern const unsigned char inputs_button_map[6];

/* Multiplexer settling, tunable at runtime so it can be measured rather than guessed. */
void inputs_set_settle(uint32_t us, uint32_t discards);

/* ADC sampling window, in ADCLK states, for the two pot-phase channels.
 *
 * This is the hardware answer to the charge-retention problem: the sample-and-hold capacitor
 * charges through the pot wiper and the multiplexer's on-resistance, and this register decides
 * how long it is given. Measured cost is ~4 core cycles per state per conversion at 240 MHz,
 * so it is far cheaper than an equivalent delay loop — the ADC does the waiting, not the CPU. */
/* Strobe behaviour around a conversion: 0 = high during, 1 = low during, 2 = untouched. */
void     inputs_set_mux_mode(unsigned m);
unsigned inputs_get_mux_mode(void);

void inputs_set_sampling(uint8_t states);
uint8_t inputs_get_sampling(void);
uint32_t inputs_get_settle_us(void);
uint32_t inputs_get_discards(void);

/* One full pass over the multiplexer. Everything is in PANEL order: index 0 and bit 0 are the
 * bottom-left knob, and button bit 0 is front-panel button 1. Both switch banks are active low
 * on the wire and are inverted here, so a set bit always means "pressed". */
typedef struct {
    uint16_t phase_a[INPUT_POTS];
    uint16_t phase_b[INPUT_POTS];
    uint16_t push_mask;
    uint16_t button_mask;

    /* Signed detents since the previous service pass, decoded from both tracks. Zero for a
     * knob that did not move, which is the common case. */
    int32_t  detents[INPUT_POTS];

    /* Pots that produced a detent this pass. Diagnostics and display only: it is deliberately
     * NOT used to infer touch any more. See qt2120_touch_filter for why that rule was removed. */
    uint16_t moved_mask;
} input_sample_t;

void inputs_sample(input_sample_t *out);

/* Preferred scanner: digital senses every pass, analog spent on the knob being touched.
 * See inputs.c for why that is both faster and more accurate than a fair sweep. */
void inputs_service(uint16_t touch_mask, input_sample_t *out);

/* Rotation, in detents, from a pot's two phase tracks.
 *
 * Call once per service pass with the freshly sampled pair; the decoder keeps its own history.
 * Returns 0 when the knob has not moved past the deadband, which is the common case.
 *
 * INTERIM. A correct endless-pot decode needs both tracks and the quadrant they describe, and
 * the constants for that were inherited from the stock firmware in 10-bit units while our ADC
 * is 12-bit -- so they cannot simply be transcribed. Deriving them needs a capture of both
 * phases across several revolutions, which needs a hand on the knob. Until that exists this
 * does the part that can be done correctly without it: wrap handling, a real deadband, and no
 * fabricated direction. See docs/hardware-notes.md and tools/deploy/potcap.py. */
int32_t inputs_decode_rotation(unsigned pot, uint16_t phase_a, uint16_t phase_b);

void inputs_init(void);

/* Point the mux at a channel and let it settle. Exposed because the diagnostics drive it
 * directly when working out which ADC channel is which. */
void inputs_mux_select(unsigned ch);

/* Both pot phases for the currently selected mux channel. */
void inputs_read_phases(uint16_t *a, uint16_t *b);

/* Digital sense lines for the currently selected mux channel.
 * `pot_switch` is that channel's pot push; `button` is one of the six front-panel buttons. */
void inputs_read_senses(int *pot_switch, int *button);

/* Raw ADC conversion on either unit, any channel — for identifying which channel the pot
 * phases actually land on, rather than trusting the inferred numbering. */
uint16_t inputs_adc_raw(unsigned unit, unsigned ch);

#endif /* ELECTRA_INPUTS_H */
