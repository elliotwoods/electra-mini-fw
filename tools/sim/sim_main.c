/* Host simulator for the device's drawing code.
 *
 * Compiles the REAL text renderer and the REAL font data against a framebuffer instead of the
 * RA8876, and writes a PPM. That means layout, typography, spacing and colour can be judged in
 * a second on a workstation, with no hardware in the loop and no 20-second flash cycle — and
 * when something looks wrong on the panel, it separates "the drawing code is wrong" from "the
 * device path is wrong", which are completely different investigations.
 *
 * Only the RA8876 entry points are stubbed. Everything above them is the same source that runs
 * on the device, because a simulator that reimplements the thing it is simulating tests
 * nothing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ra8876.h"
#include "font.h"
#include "text.h"
#include "surface.h"
#include "ui.h"       /* UI_BOOT_FRAMES: the scene list must not carry its own copy */

#include "sim_hal.h"
#include "fixtures.h"

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }

    fprintf(f, "P6\n%d %d\n255\n", LCD_WIDTH, LCD_HEIGHT);
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        uint16_t c = sim_framebuffer()[i];
        /* RGB565 -> 8 bit, replicating the high bits into the low ones so white stays white. */
        uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)((c & 0x1F) * 255 / 31);
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ scenes */

void ui_render_empty(void);         /* the resting state: no host, no fields */
void ui_render_boot(unsigned step); /* the power-on sequence, replayed from frame 0 */
void ui_render_demo(void);          /* in ui.c, shared with the device */
void ui_render_demo_focused(void);  /* the zoomed readout, mid-edit */
void ui_render_demo_page2(void);    /* Choice and ReadOnly fields */
void ui_render_demo_focused_top(void);  /* the mirrored drilled view */
void ui_render_demo_focused_toggle(void);
void ui_render_demo_focused_negative(void);
void ui_render_demo_focused_choice(void);
void ui_render_demo_focused_fine(void);
void ui_render_demo_focused_color(void);
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

static void scene_specimen(void)
{
    const uint16_t WHITE = 0xFFFF, DIM = 0x8410, ACCENT = 0x07FF, BG = 0x0000;

    ra8876_set_fg(BG);
    ra8876_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);

    text_draw(&font_ui,    24,  30, "Electra One Mini", WHITE, BG);
    text_draw(&font_small, 24,  66, "custom firmware / EMP/1 transport", DIM, BG);

    text_draw(&font_ui,    24, 120, "Cutoff", DIM, BG);
    text_draw(&font_value, 24, 160, "0.5300", ACCENT, BG);
    text_draw(&font_small, 150, 160, "Hz", DIM, BG);

    text_draw(&font_ui,    24, 220, "The quick brown fox jumps over the lazy dog", WHITE, BG);
    text_draw(&font_small, 24, 254, "The quick brown fox jumps over the lazy dog", DIM, BG);

    text_draw(&font_value, 24, 320, "0123456789", WHITE, BG);
    text_draw(&font_value, 24, 360, "1111111111", WHITE, BG);

    text_draw(&font_small, 24, 430,
              "if the two rows above are the same width, digits will not jitter", DIM, BG);
}

int main(int argc, char **argv)
{
    const char *scene = (argc > 1) ? argv[1] : "ui";
    const char *out   = (argc > 2) ? argv[2] : "build/sim.ppm";

    sim_reset();

    /* "boot" alone shows the finished sequence; "boot-N" stops after frame N, so the sweep can
     * be judged part way rather than only at its end. */
    if (strncmp(scene, "boot", 4) == 0 && (scene[4] == 0 || scene[4] == '-')) {
        unsigned step = (scene[4] == '-') ? (unsigned)atoi(scene + 5) : UI_BOOT_FRAMES - 1u;
        ui_render_boot(step);
        write_ppm(out);
        printf("wrote %s (%dx%d) after boot frame %u of %u\n",
               out, LCD_WIDTH, LCD_HEIGHT, step, UI_BOOT_FRAMES - 1u);
        return 0;
    }

    if      (strncmp(scene,"sparse-",7)==0) {fixture_sparse((unsigned)atoi(scene+7),0);ui_render();}
    else if (strcmp(scene,"holes")==0) {fixture_sparse(4,1);ui_render();}
    else if (strcmp(scene, "specimen") == 0) scene_specimen();
    else if (strcmp(scene,"operator-settings")==0) {surf_demo_operator();ui_state_init();ui_state_reveal(8);ui_render();}
    else if (strcmp(scene,"operator-settings-press")==0) {surf_demo_operator();ui_state_init();ui_state_reveal(8);ui_state_touch(1,100);ui_state_push(0,1,120);ui_render();}
    else if (strcmp(scene,"operator-touch")==0) {surf_demo_operator();ui_state_init();ui_state_touch(1,100);ui_render();}
    else if (strcmp(scene,"operator-press")==0) {surf_demo_operator();ui_state_init();ui_state_touch(1,100);ui_state_push(0,1,120);ui_render();}
    else if (strcmp(scene, "operator") == 0) { surf_demo_operator(); ui_render(); }
    else if (strcmp(scene, "empty")    == 0) ui_render_empty();
    else if (strcmp(scene, "focus")    == 0) ui_render_demo_focused();
    else if (strcmp(scene, "page2")    == 0) ui_render_demo_page2();
    else if (strcmp(scene, "focustop") == 0) ui_render_demo_focused_top();
    else if (strcmp(scene, "focus-toggle") == 0) ui_render_demo_focused_toggle();
    else if (strcmp(scene, "focus-negative") == 0) ui_render_demo_focused_negative();
    else if (strcmp(scene, "focus-choice") == 0) ui_render_demo_focused_choice();
    else if (strcmp(scene, "choice-first") == 0) ui_render_demo_choice_waveform(0u);
    else if (strcmp(scene, "choice-middle") == 0) ui_render_demo_choice_waveform(2u);
    else if (strcmp(scene, "choice-last") == 0) ui_render_demo_choice_waveform(4u);
    else if (strcmp(scene, "choice-7") == 0) ui_render_demo_choice_short();
    else if (strcmp(scene, "choice-5") == 0) ui_render_demo_choice_medium();
    else if (strcmp(scene, "choice-3-top") == 0) ui_render_demo_choice_long_top();
    else if (strcmp(scene, "focus-fine") == 0) ui_render_demo_focused_fine();
    else if (strcmp(scene, "focus-color") == 0) ui_render_demo_focused_color();
    else if (strcmp(scene, "system")   == 0) ui_render_demo_system();
    else if (strcmp(scene, "reboot")   == 0) ui_render_demo_reboot();
    else if (strcmp(scene, "cal")       == 0 || strcmp(scene, "cal-active") == 0)
                                                    ui_render_demo_calibration();
    else if (strcmp(scene, "cal-select") == 0)     ui_render_demo_calibration_select();
    else if (strcmp(scene, "cal-baseline") == 0)   ui_render_demo_calibration_baseline();
    else if (strcmp(scene, "cal-failed") == 0)     ui_render_demo_calibration_failed();
    else if (strcmp(scene, "cal-complete") == 0)   ui_render_demo_calibration_complete();
    else if (strcmp(scene, "brightness-full") == 0) ui_render_demo_brightness_full();
    else if (strcmp(scene, "brightness-mid") == 0)  ui_render_demo_brightness_mid();
    else if (strcmp(scene, "brightness-min") == 0)  ui_render_demo_brightness_min();
    else if (strcmp(scene, "brightness-error") == 0) ui_render_demo_brightness_error();
    else                                     ui_render_demo();

    write_ppm(out);
    printf("wrote %s (%dx%d)\n", out, LCD_WIDTH, LCD_HEIGHT);
    return 0;
}
