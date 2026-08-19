#include <stdint.h>
#include <string.h>

#include "persist.h"
#include "qt2120.h"
#include "touch_cal.h"

typedef void (*touch_cal_report_fn)(const char *name, int passed);

const unsigned char qt2120_pot_to_key[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

static qt2120_detail_t detail;
static uint8_t thresholds[8];
static uint8_t gains[8];
static int failures;
static touch_cal_report_fn report_fn;

int persist_touch_cal_read(touch_cal_rec_t *out) { (void)out; return 0; }
int persist_touch_cal_write(const touch_cal_rec_t *rec) { (void)rec; return 0; }
uint32_t persist_touch_cal_checksum(const touch_cal_rec_t *r)
{
    return r->magic ^ r->version ^ r->generation ^ r->valid_mask;
}

int qt2120_read_detail(qt2120_detail_t *out) { *out = detail; return 0; }
int qt2120_recalibrate(void) { return 0; }
int qt2120_apply_pot_config(uint8_t pot, uint8_t threshold, uint8_t gain)
{
    thresholds[pot] = threshold;
    gains[pot] = gain;
    return 0;
}
uint8_t qt2120_threshold_for_pot(uint8_t pot) { return thresholds[pot]; }
uint8_t qt2120_gain_for_pot(uint8_t pot) { return gains[pot]; }

static void check(const char *name, int passed)
{
    report_fn(name, passed);
    if (!passed) failures++;
}

static void reset_cal(void)
{
    memset(&detail, 0, sizeof(detail));
    memset(thresholds, QT2120_THRESHOLD, sizeof(thresholds));
    memset(gains, QT2120_DEFAULT_GAIN, sizeof(gains));
    touch_cal_init();
}

static uint32_t perform_gesture(unsigned pot, uint32_t now)
{
    uint16_t bit = (uint16_t)(1u << pot);
    detail.signal[pot] = 80;
    detail.reference[pot] = 0;
    touch_cal_tick(now, bit, 0);
    touch_cal_tick(now + 1000u, bit, bit);
    detail.signal[pot] = 0;
    touch_cal_tick(now + 1001u, 0, bit);
    touch_cal_tick(now + 1401u, 0, 0);
    return now + 1401u;
}

static void test_clear_interval_restarts_and_advances(void)
{
    reset_cal();
    touch_cal_start(1u, 0);
    touch_cal_tick(1000, 0x02u, 0);       /* any dial restarts, rather than failing */
    int ok = touch_cal_status()->phase == TOUCH_CAL_BASELINE
          && touch_cal_status()->clear_ms_remaining == 2000u;
    touch_cal_tick(2999, 0, 0);
    ok = ok && touch_cal_status()->phase == TOUCH_CAL_BASELINE
            && touch_cal_status()->clear_ms_remaining == 1u;
    touch_cal_tick(3000, 0, 0);
    ok = ok && touch_cal_status()->phase == TOUCH_CAL_TOUCH
            && touch_cal_status()->clear_ms_remaining == 0u;
    check("clear touch restarts two-second baseline", ok);
}

static void test_active_wrong_dial_still_fails(void)
{
    reset_cal();
    touch_cal_start(1u, 0);
    touch_cal_tick(2000, 0, 0);
    touch_cal_tick(2001, 0x02u, 0);
    check("active-stage wrong dial fails", touch_cal_status()->phase == TOUCH_CAL_FAILED);
}

static void test_failure_preserves_the_triggering_sample(void)
{
    reset_cal();
    touch_cal_start(1u, 0);
    touch_cal_tick(2000, 0, 0);

    detail.signal[0] = 90;
    touch_cal_tick(2020, 0x01u, 0);
    touch_cal_tick(2520, 0x01u, 0x01u);
    const touch_cal_status_t *s = touch_cal_status();
    int ok = s->phase == TOUCH_CAL_HOLD
          && s->raw_touch == 0x01u
          && s->moved_mask == 0x01u
          && s->delta == 90u
          && s->peak == 90u
          && s->hold_ms == 500u
          && s->saw_move == 1u;

    touch_cal_tick(2600, 0, 0);
    s = touch_cal_status();
    ok = ok && s->phase == TOUCH_CAL_FAILED
            && s->raw_touch == 0
            && s->moved_mask == 0
            && s->hold_ms == 580u
            && s->saw_move == 1u
            && s->peak == 90u;
    check("failure preserves its raw timing and movement sample", ok);
}

static void test_retry_keeps_all_sequence_and_final_clear(void)
{
    reset_cal();
    touch_cal_start(0xFFu, 0);
    touch_cal_tick(2000, 0, 0);
    touch_cal_tick(2001, 0x02u, 0);       /* fail dial 1 during its active stage */
    touch_cal_retry(2100);
    int retry_ok = touch_cal_status()->phase == TOUCH_CAL_BASELINE
                && touch_cal_status()->selected_mask == 0xFFu
                && touch_cal_status()->pot == 0u;

    touch_cal_tick(4100, 0, 0);
    uint32_t now = 4101;
    now = perform_gesture(0, now);
    now = perform_gesture(0, now + 1u);
    now = perform_gesture(0, now + 1u);
    int validate_ok = touch_cal_status()->phase == TOUCH_CAL_VALIDATE;

    touch_cal_tick(now + 500u, 0x01u, 0); /* final clear interval restarts too */
    touch_cal_tick(now + 2499u, 0, 0);
    validate_ok = validate_ok && touch_cal_status()->phase == TOUCH_CAL_VALIDATE;
    touch_cal_tick(now + 2500u, 0, 0);
    validate_ok = validate_ok && touch_cal_status()->phase == TOUCH_CAL_TOUCH;

    now = perform_gesture(0, now + 2501u);
    int next_baseline = touch_cal_status()->phase == TOUCH_CAL_BASELINE;
    int next_dial = touch_cal_status()->pot == 1u;
    int batch_kept = touch_cal_status()->selected_mask == 0xFFu;
    check("retry restores failed dial in batch", retry_ok);
    check("final validation requires uninterrupted clear", validate_ok);
    check("retry advances to next batch baseline", next_baseline);
    check("retry advances from dial 1 to dial 2", next_dial);
    check("retry retains remaining batch mask", batch_kept);
}

int touch_cal_run_selftests(touch_cal_report_fn report)
{
    failures = 0;
    report_fn = report;
    test_clear_interval_restarts_and_advances();
    test_active_wrong_dial_still_fails();
    test_failure_preserves_the_triggering_sample();
    test_retry_keeps_all_sequence_and_final_clear();
    return failures;
}
