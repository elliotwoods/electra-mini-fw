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

#endif /* ELECTRA_UI_H */
