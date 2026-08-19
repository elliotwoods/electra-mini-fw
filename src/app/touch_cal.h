#ifndef ELECTRA_TOUCH_CAL_H
#define ELECTRA_TOUCH_CAL_H

#include <stdint.h>

typedef enum {
    TOUCH_CAL_IDLE = 0,
    TOUCH_CAL_BASELINE,
    TOUCH_CAL_TOUCH,
    TOUCH_CAL_HOLD,
    TOUCH_CAL_RELEASE,
    TOUCH_CAL_VALIDATE,
    TOUCH_CAL_COMPLETE,
    TOUCH_CAL_FAILED
} touch_cal_phase_t;

typedef struct {
    touch_cal_phase_t phase;
    uint8_t pot;
    uint8_t cycle;
    uint8_t selected_mask;
    uint8_t valid_mask;
    uint8_t custom_active;
    uint8_t threshold[8];
    uint8_t gain[8];
    uint16_t noise;
    uint16_t touch;
    uint16_t clear_ms_remaining;
    uint16_t raw_touch;      /* last raw sensor mask presented to the state machine */
    uint16_t moved_mask;     /* last rotary movement mask presented alongside it */
    uint16_t delta;          /* active dial's absolute signal/reference separation */
    uint16_t peak;           /* largest active-dial delta in the current gesture */
    uint16_t hold_ms;        /* elapsed HOLD time at the last sample, saturated */
    uint8_t saw_move;        /* whether movement has been observed in this gesture */
    const char *message;
} touch_cal_status_t;

typedef enum {
    TOUCH_CAL_CONFIG_FAIL = 0,
    TOUCH_CAL_CONFIG_ACCEPT,
    TOUCH_CAL_CONFIG_MORE_GAIN
} touch_cal_config_result_t;

/* Pure policy helper, shared with host tests. A valid threshold must sit above idle noise and
 * retain at least a 3:1 measured touch margin; weak signals ask for the next safe gain first. */
static inline touch_cal_config_result_t touch_cal_choose_config(uint16_t noise, uint16_t touch,
                                                                int have_more_gain,
                                                                uint8_t *threshold_out)
{
    uint16_t floor = (uint16_t)(noise + 4u);
    if (touch < floor * 4u && have_more_gain) return TOUCH_CAL_CONFIG_MORE_GAIN;
    if (touch < floor * 3u) return TOUCH_CAL_CONFIG_FAIL;
    uint16_t threshold = touch / 8u;
    if (threshold < floor) threshold = floor;
    if (threshold > 255u) threshold = 255u;
    if (threshold_out) *threshold_out = (uint8_t)threshold;
    return TOUCH_CAL_CONFIG_ACCEPT;
}

void touch_cal_init(void);
void touch_cal_start(uint8_t pot_mask, uint32_t now_ms);
void touch_cal_retry(uint32_t now_ms);
void touch_cal_done(void);
void touch_cal_cancel(void);
void touch_cal_restore_defaults(void);
void touch_cal_tick(uint32_t now_ms, uint16_t raw_touch, uint16_t moved_mask);
const touch_cal_status_t *touch_cal_status(void);

#endif
