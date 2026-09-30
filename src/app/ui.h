/* The control-surface UI. See ui.c for the layout reasoning. */

#ifndef ELECTRA_UI_H
#define ELECTRA_UI_H

#include <stdint.h>

void ui_reset(void);          /* stage a clear and force a full repaint; publish with ui_render */
void ui_render(void);         /* repaint only what changed; reads ui_state() */

/* The power-on sequence, one incremental frame at a time: the eight cells fill left-to-right
 * across the panel while an accent rule sweeps the band, then READY, then a hold.
 *
 * The CALLER paces it and must service USB between frames -- this animation must never be the
 * reason the device stops answering SETUP. Frames are incremental, so run 0..n rather than
 * expecting frame n to stand on its own. See ui.c for why it is counted rather than timed. */
#define UI_BOOT_CELL_FRAMES  8u                             /* one per knob */
#define UI_BOOT_READY_FRAME  (1u + UI_BOOT_CELL_FRAMES)     /* the word appears on this one */
#define UI_BOOT_FRAMES       (UI_BOOT_READY_FRAME + 6u)     /* plus the hold */

void ui_boot_ready_frame(unsigned step);

/* Fixed scenes, for the simulator and for the `ui` console command. */
void ui_render_empty(void);           /* the resting state: no host, no fields */
void ui_render_boot(unsigned step);   /* the power-on sequence replayed from frame 0 */
void ui_render_demo(void);            /* the overview */
void ui_render_demo_focused(void);    /* the focused readout, mid-edit */
void ui_render_demo_focused_top(void);/* drilled into a TOP field: readout mirrors below */
void ui_render_demo_page2(void);      /* the page with Choice and ReadOnly fields */
void ui_render_demo_choice_waveform(uint16_t selected);
void ui_render_demo_choice_short(void);
void ui_render_demo_choice_medium(void);
void ui_render_demo_choice_long_top(void);
void ui_render_demo_system(void);
void ui_render_demo_reboot(void);
void ui_render_demo_calibration(void);
void ui_render_demo_calibration_select(void);
void ui_render_demo_calibration_baseline(void);
void ui_render_demo_calibration_failed(void);
void ui_render_demo_calibration_complete(void);
void ui_render_demo_brightness_full(void);
void ui_render_demo_brightness_mid(void);
void ui_render_demo_brightness_min(void);
void ui_render_demo_brightness_error(void);

#endif /* ELECTRA_UI_H */
