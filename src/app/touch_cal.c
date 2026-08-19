#include <string.h>

#include "persist.h"
#include "qt2120.h"
#include "touch_cal.h"

/* Datasheet-recommended oversampling pairs: high nibble PULSE, low nibble SCALE. */
static const uint8_t gain_steps[] = { 0x00u, 0x21u, 0x42u, 0x63u };

static touch_cal_status_t st;
static touch_cal_rec_t record;
static uint32_t deadline;
static uint32_t hold_started;
static uint32_t release_started;
static uint16_t min_touch;
static uint16_t peak_touch;
static uint8_t gain_step;
static uint8_t saw_move;
static uint8_t validating;

static unsigned key_for_pot(unsigned pot)
{
    extern const unsigned char qt2120_pot_to_key[8];
    return qt2120_pot_to_key[pot];
}

static void fail(const char *why)
{
    st.phase = TOUCH_CAL_FAILED;
    st.message = why;
    (void)qt2120_apply_pot_config(st.pot, st.threshold[st.pot], st.gain[st.pot]);
    (void)qt2120_recalibrate();
}

static void begin_baseline(uint32_t now_ms)
{
    st.phase = TOUCH_CAL_BASELINE;
    st.cycle = 0;
    st.noise = 0;
    st.touch = 0;
    peak_touch = 0;
    saw_move = 0;
    st.raw_touch = 0;
    st.moved_mask = 0;
    st.delta = 0;
    st.peak = 0;
    st.hold_ms = 0;
    st.saw_move = 0;
    min_touch = 0xFFFFu;
    validating = 0;
    st.message = "Keep hands clear - starts automatically";
    deadline = now_ms + 2000u;
    st.clear_ms_remaining = 2000u;
    (void)qt2120_apply_pot_config(st.pot, QT2120_THRESHOLD, gain_steps[gain_step]);
    if (qt2120_recalibrate() != 0) fail("Touch controller did not calibrate");
}

static int next_selected(unsigned after)
{
    for (unsigned p = after; p < 8u; p++) if (st.selected_mask & (1u << p)) return (int)p;
    return -1;
}

static void commit_pot(uint32_t now_ms)
{
    uint8_t bit = (uint8_t)(1u << st.pot);
    st.valid_mask |= bit;
    st.threshold[st.pot] = qt2120_threshold_for_pot(st.pot);
    st.gain[st.pot] = qt2120_gain_for_pot(st.pot);

    record.magic = TOUCH_CAL_MAGIC;
    record.version = TOUCH_CAL_VERSION;
    record.generation++;
    record.valid_mask = st.valid_mask;
    memcpy(record.threshold, st.threshold, sizeof(record.threshold));
    memcpy(record.gain, st.gain, sizeof(record.gain));
    record.checksum = persist_touch_cal_checksum(&record);
    if (persist_touch_cal_write(&record) != 0) { fail("Could not save calibration"); return; }

    int next = next_selected((unsigned)st.pot + 1u);
    if (next < 0) {
        st.custom_active = st.valid_mask != 0;
        st.phase = TOUCH_CAL_COMPLETE;
        st.message = "Calibration saved";
        return;
    }
    st.pot = (uint8_t)next;
    gain_step = 0;
    begin_baseline(now_ms);
}

void touch_cal_init(void)
{
    memset(&st, 0, sizeof(st));
    memset(&record, 0, sizeof(record));
    for (unsigned p = 0; p < 8u; p++) {
        st.threshold[p] = QT2120_THRESHOLD;
        st.gain[p] = QT2120_DEFAULT_GAIN;
    }
    if (persist_touch_cal_read(&record)) {
        st.valid_mask = (uint8_t)(record.valid_mask & 0xFFu);
        for (unsigned p = 0; p < 8u; p++) {
            if (!(st.valid_mask & (1u << p))) continue;
            if (qt2120_apply_pot_config((uint8_t)p, record.threshold[p], record.gain[p]) == 0) {
                st.threshold[p] = record.threshold[p];
                st.gain[p] = record.gain[p];
            } else {
                st.valid_mask &= (uint8_t)~(1u << p);
            }
        }
        (void)qt2120_recalibrate();
    }
    st.custom_active = st.valid_mask != 0;
    st.phase = TOUCH_CAL_IDLE;
    st.message = "Ready";
}

void touch_cal_start(uint8_t pot_mask, uint32_t now_ms)
{
    if (!pot_mask) return;
    st.selected_mask = pot_mask;
    int first = next_selected(0);
    if (first < 0) return;
    st.pot = (uint8_t)first;
    gain_step = 0;
    begin_baseline(now_ms);
}

void touch_cal_retry(uint32_t now_ms)
{
    if (st.phase != TOUCH_CAL_FAILED) return;
    gain_step = 0;
    begin_baseline(now_ms);
}

void touch_cal_done(void)
{
    if (st.phase != TOUCH_CAL_COMPLETE) return;
    st.phase = TOUCH_CAL_IDLE;
    st.message = "Ready";
}

void touch_cal_cancel(void)
{
    st.phase = TOUCH_CAL_IDLE;
    st.clear_ms_remaining = 0;
    st.message = "Cancelled; previous values kept";
    for (unsigned p = 0; p < 8u; p++)
        (void)qt2120_apply_pot_config((uint8_t)p, st.threshold[p], st.gain[p]);
    (void)qt2120_recalibrate();
}

void touch_cal_restore_defaults(void)
{
    for (unsigned p = 0; p < 8u; p++) {
        st.threshold[p] = QT2120_THRESHOLD;
        st.gain[p] = QT2120_DEFAULT_GAIN;
        (void)qt2120_apply_pot_config((uint8_t)p, QT2120_THRESHOLD, QT2120_DEFAULT_GAIN);
    }
    memset(&record, 0, sizeof(record));
    record.magic = TOUCH_CAL_MAGIC;
    record.version = TOUCH_CAL_VERSION;
    record.generation = 1;
    record.checksum = persist_touch_cal_checksum(&record);
    (void)persist_touch_cal_write(&record);
    st.valid_mask = 0;
    st.custom_active = 0;
    st.phase = TOUCH_CAL_IDLE;
    st.message = "Touch defaults restored";
    (void)qt2120_recalibrate();
}

const touch_cal_status_t *touch_cal_status(void) { return &st; }

void touch_cal_tick(uint32_t now_ms, uint16_t raw_touch, uint16_t moved_mask)
{
    if (st.phase == TOUCH_CAL_IDLE || st.phase == TOUCH_CAL_COMPLETE
        || st.phase == TOUCH_CAL_FAILED) return;

    uint16_t wanted = (uint16_t)(1u << st.pot);

    qt2120_detail_t d;
    if (qt2120_read_detail(&d) != 0) { fail("Touch controller read failed"); return; }
    unsigned key = key_for_pot(st.pot);
    int delta = (int)d.signal[key] - (int)d.reference[key];
    if (delta < 0) delta = -delta;
    if (delta > 0xFFFF) delta = 0xFFFF;
    st.raw_touch = raw_touch;
    st.moved_mask = moved_mask;
    st.delta = (uint16_t)delta;
    st.peak = peak_touch;
    st.hold_ms = st.phase == TOUCH_CAL_HOLD
               ? (uint16_t)(((uint32_t)(now_ms - hold_started) > 0xFFFFu)
                            ? 0xFFFFu : (uint32_t)(now_ms - hold_started))
               : 0u;
    st.saw_move = saw_move;

    if (st.phase == TOUCH_CAL_BASELINE || st.phase == TOUCH_CAL_VALIDATE) {
        if (raw_touch) {
            /* A stray touch discards the partial clear interval. It is not a calibration
             * failure: the countdown simply starts over and advances when uninterrupted. */
            deadline = now_ms + 2000u;
            st.noise = 0;
            st.clear_ms_remaining = 2000u;
            st.message = "Keep hands clear - countdown restarted";
            return;
        }
        uint32_t remaining = (int32_t)(now_ms - deadline) >= 0 ? 0u : deadline - now_ms;
        st.clear_ms_remaining = (uint16_t)(remaining > 2000u ? 2000u : remaining);
        st.message = "Keep hands clear - starts automatically";
        if ((uint16_t)delta > st.noise) st.noise = (uint16_t)delta;
        if ((int32_t)(now_ms - deadline) >= 0) {
            st.clear_ms_remaining = 0;
            st.phase = TOUCH_CAL_TOUCH;
            st.message = validating ? "Final check: touch and turn this dial"
                                    : "Touch, hold and turn this dial";
        }
        return;
    }

    /* During the active gesture, another dial is ambiguous and remains a hard failure. */
    if (raw_touch & (uint16_t)~wanted) { fail("A different dial was touched"); return; }

    if (st.phase == TOUCH_CAL_TOUCH) {
        if (!(raw_touch & wanted)) return;
        st.phase = TOUCH_CAL_HOLD;
        hold_started = now_ms;
        peak_touch = 0;
        saw_move = 0;
        st.peak = 0;
        st.hold_ms = 0;
        st.saw_move = 0;
        st.message = "Keep touching and turn the dial";
    }

    if (st.phase == TOUCH_CAL_HOLD) {
        if ((uint16_t)delta > peak_touch) peak_touch = (uint16_t)delta;
        if (moved_mask & wanted) saw_move = 1;
        st.peak = peak_touch;
        st.hold_ms = (uint16_t)(((uint32_t)(now_ms - hold_started) > 0xFFFFu)
                               ? 0xFFFFu : (uint32_t)(now_ms - hold_started));
        st.saw_move = saw_move;
        if (raw_touch & wanted) {
            if ((uint32_t)(now_ms - hold_started) >= 1000u && saw_move)
                st.message = "Now release the dial completely";
            if ((uint32_t)(now_ms - hold_started) >= 6000u) {
                fail("Dial was not released when requested"); return;
            }
            return;
        }
        if ((uint32_t)(now_ms - hold_started) < 1000u || !saw_move) {
            fail("Touch was too short or the dial was not turned"); return;
        }
        if (peak_touch <= st.noise + 8u) { fail("Touch signal is too close to idle noise"); return; }
        if (peak_touch < min_touch) min_touch = peak_touch;
        st.touch = min_touch;
        st.phase = TOUCH_CAL_RELEASE;
        release_started = now_ms;
        deadline = now_ms + 400u;
        st.message = "Release the dial completely";
        return;
    }

    if (st.phase == TOUCH_CAL_RELEASE) {
        if (raw_touch & wanted) {
            if ((uint32_t)(now_ms - release_started) >= 3000u) {
                fail("Dial did not return to released state"); return;
            }
            deadline = now_ms + 400u;
            return;
        }
        if ((int32_t)(now_ms - deadline) < 0) return;

        if (validating) { commit_pot(now_ms); return; }
        st.cycle++;
        if (st.cycle < 3u) {
            st.phase = TOUCH_CAL_TOUCH;
            st.message = "Repeat touch, hold, turn and release";
            return;
        }

        uint8_t threshold = 0;
        touch_cal_config_result_t choice = touch_cal_choose_config(
            st.noise, min_touch, gain_step + 1u < sizeof(gain_steps), &threshold);
        if (choice == TOUCH_CAL_CONFIG_MORE_GAIN) {
            gain_step++;
            begin_baseline(now_ms);
            return;
        }
        if (choice == TOUCH_CAL_CONFIG_FAIL) {
            fail("No safe threshold separates touch from noise"); return;
        }
        if (qt2120_apply_pot_config(st.pot, threshold, gain_steps[gain_step]) != 0
            || qt2120_recalibrate() != 0) {
            fail("Could not apply candidate calibration"); return;
        }
        validating = 1;
        st.noise = 0;
        st.phase = TOUCH_CAL_VALIDATE;
        deadline = now_ms + 2000u;
        st.clear_ms_remaining = 2000u;
        st.message = "Keep hands clear - final check starts automatically";
    }
}
