#ifndef ELECTRA_QT2120_H
#define ELECTRA_QT2120_H

#include <stdint.h>

/* AT42QT2120 capacitive touch sensor — the pot-touch detector.
 *
 * Eight of its twelve keys are wired, one per pot. Touching a pot is what tells the host which
 * control the user's hand is on, independently of whether they have turned it yet, and it is
 * the input the surface protocol needs for focus.
 */

#define QT2120_ADDR   0x1C

int  qt2120_init(void);
void qt2120_hw_reset(void);          /* P402 pulse; the chip is mute until this happens */

/* Bit p of the result is pot p (panel numbering), already de-scrambled from key numbering.
 * Returns a negative RIIC error code on failure, so a wedged bus cannot masquerade as
 * "nothing is being touched". */
int  qt2120_read_touch(uint16_t *mask_out);

/* Raw 12-bit key status, for diagnosis. */
int  qt2120_read_keys(uint16_t *keys_out);

/* Detection threshold, in counts. Higher is less sensitive and less prone to latching. */
#define QT2120_THRESHOLD  12

/* How long a key may report touched, with its pot never moving, before we assume it is stuck.
 * Longer than any real touch-and-turn gesture; short enough that a latch is a nuisance rather
 * than a failure. */
#define QT2120_STUCK_MS   8000u

/* Per-key signal and reference, for seeing WHY a key is in detect rather than guessing.
 * delta = reference - signal; a key is in detect when delta exceeds its threshold. */
typedef struct {
    uint16_t signal[12];
    uint16_t reference[12];
    uint16_t keys;
} qt2120_detail_t;

int qt2120_read_detail(qt2120_detail_t *out);
int qt2120_recalibrate(void);

/* Per-key gain (register 0x28 + key): high nibble scale, low nibble burst pulses. The lever for
 * one electrode being weaker than its neighbours, as opposed to the whole panel being deaf. */
int qt2120_set_gain(uint8_t key, uint8_t pulse_scale);

/* Continuous peak-hold of the per-key touch delta, so a touch can be measured without anyone
 * having to hit a capture window. Call track_peaks periodically; read and reset with get_peaks. */
void qt2120_track_peaks(void);
void qt2120_get_peaks(uint16_t out_pos[8], uint16_t out_neg[8], uint16_t out_last[8],
                      uint16_t *out_seen, uint32_t *out_calls, int reset);

/* How long the sensor must agree a key is released, with the pot also still, before release is
 * reported. Longer than a finger rolling across a knob during rotation; far shorter than a
 * deliberate release feels. */
/* Release debounce, SIZED FROM A MEASUREMENT rather than chosen.
 *
 * A 90-second capture of one knob (tools: `tcap`) recorded 19 release runs from the sensor.
 * SEVENTEEN of them were shorter than 400 ms with the finger plainly still down -- durations
 * 46, 46, 46, 90, 91 x7, 137, 181, 182, 182, 226 and 272 ms. Only two were real lifts. The part
 * simply drops detect for a few scans at a time while a finger sits on the knob, and it does it
 * with a strong signal: the margin when it says TOUCHED runs to a median of +49 against a
 * threshold of 12, so this is not marginal sensing that a lower threshold would cure.
 *
 * Using the margin as a second opinion was tried and rejected: it is too noisy to separate the
 * two cases, reading +65 partway through a genuine lift.
 *
 * So the debounce is asymmetric and sized to clear the longest dropout observed with room to
 * spare. Acquisition stays instant, which is what makes the gesture feel immediate; only the
 * release waits. */
#define QT2120_RELEASE_MS  300u

/* Conditions the raw sensor state into what the rest of the firmware should believe.
 *
 * `moved` marks pots whose rotation changed since the last call — a pot cannot turn without a
 * hand on it, so movement asserts touch on its own. Acquisition is immediate, release is
 * debounced, and a key claiming touch with a still pot and a marginal signal is recalibrated.
 * See qt2120.c for why the two reported failure modes cannot both be fixed with the threshold. */
uint16_t qt2120_touch_filter(uint16_t raw, uint16_t moved, uint32_t dt_ms);

void     qt2120_set_release_ms(uint32_t ms);
uint32_t qt2120_get_release_ms(void);

#endif /* ELECTRA_QT2120_H */
