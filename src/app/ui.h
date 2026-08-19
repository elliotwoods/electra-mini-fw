/* The control-surface UI. See ui.c for the layout reasoning. */

#ifndef ELECTRA_UI_H
#define ELECTRA_UI_H

#include <stdint.h>

void ui_reset(void);          /* clear and force a full repaint */
void ui_render(void);         /* repaint only what changed; reads ui_state() */

/* Fixed scenes, for the simulator and for the `ui` console command. */
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
