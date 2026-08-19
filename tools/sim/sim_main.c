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

static uint16_t fb[LCD_WIDTH * LCD_HEIGHT];

/* Active-window state, mirroring the controller's: a blit wraps at the window edge, and the
 * renderer relies on that, so the simulator has to model it or it would flatter code that
 * would not work on the panel. */
static int16_t aw_x, aw_y, aw_w, aw_h;
static int32_t cursor;

static uint16_t fg_colour;

void ra8876_set_fg(uint16_t c) { fg_colour = c; }
void ra8876_set_bg(uint16_t c) { (void)c; }

void ra8876_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h)
{
    for (int16_t row = 0; row < h; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= LCD_HEIGHT) continue;
        for (int16_t col = 0; col < w; col++) {
            int32_t px = x + col;
            if (px < 0 || px >= LCD_WIDTH) continue;
            fb[py * LCD_WIDTH + px] = fg_colour;
        }
    }
}

void ra8876_blit_begin(int16_t x, int16_t y, int16_t w, int16_t h)
{
    aw_x = x; aw_y = y; aw_w = w; aw_h = h;
    cursor = 0;
}

void ra8876_blit_pixels(const uint16_t *px, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        int32_t cx = aw_x + (cursor % aw_w);
        int32_t cy = aw_y + (cursor / aw_w);
        if (cy >= aw_y + aw_h) break;
        if (cx >= 0 && cx < LCD_WIDTH && cy >= 0 && cy < LCD_HEIGHT) {
            fb[cy * LCD_WIDTH + cx] = px[i];
        }
        cursor++;
    }
}

void ra8876_blit_end(void)
{
    aw_x = 0; aw_y = 0; aw_w = LCD_WIDTH; aw_h = LCD_HEIGHT;
}

/* Unused by the drawing code, but referenced by the header. */
/* Bresenham, because the device's line comes from the RA8876's draw engine and the simulator
 * has no draw engine. Aliased on both sides, so what is judged here is what appears there. */
void ra8876_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 >= 0 && x0 < LCD_WIDTH && y0 >= 0 && y0 < LCD_HEIGHT)
            fb[(size_t)y0 * LCD_WIDTH + x0] = fg_colour;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 = (int16_t)(x0 + sx); }
        if (e2 <  dx) { err += dx; y0 = (int16_t)(y0 + sy); }
    }
}

void ra8876_set_canvas(uint32_t a)       { (void)a; }
void ra8876_set_display_page(uint32_t a) { (void)a; }

/* ------------------------------------------------------------------ output */

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }

    fprintf(f, "P6\n%d %d\n255\n", LCD_WIDTH, LCD_HEIGHT);
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        uint16_t c = fb[i];
        /* RGB565 -> 8 bit, replicating the high bits into the low ones so white stays white. */
        uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)((c & 0x1F) * 255 / 31);
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ scenes */

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

    memset(fb, 0, sizeof(fb));

    if      (strcmp(scene, "specimen") == 0) scene_specimen();
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
