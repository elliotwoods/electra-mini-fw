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
#include "sim_wire.h"
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

/* EMP/1.8: the daemon's system page in each state it can show (docs/graphics-evidence/system-page). */
#define T_OK    "\x01"
#define T_WARN  "\x02"
#define T_ALERT "\x03"
static const char *const READOUT_DISARMED =
    T_OK "Fleet\tDISARMED\n" "Last stop\tcontrol surface\n" "Reason\tcontrol surface E-STOP\n"
    "Stopped\t4 min ago\n" "Orch\tlab-orch\n" T_OK "Nodes\t2/2\n" T_OK "Watches\t1 on duty\n"
    "Lease\tidn-test, direct\n" "Boards\t2 Run, 0 Standby";
static const char *const READOUT_UNMET =
    T_OK "Fleet\tDISARMED\n" "Last stop\tnone\n" "Orch\tlab-orch\n" T_WARN "Nodes\t0/2\n"
    T_WARN "Watches\t0 on duty\n" "Boards\t0 Run, 0 Standby";
static const char *const READOUT_ARMED =
    T_ALERT "Fleet\tARMED\n" "Last stop\tcontrol surface\n" "Reason\tcontrol surface E-STOP\n"
    "Stopped\t6 min ago\n" "Orch\tlab-orch\n" T_OK "Nodes\t2/2\n" T_OK "Watches\t1 on duty\n"
    "Lease\tidn-test, direct\n" "Boards\t2 Run, 0 Standby";
static const char *const READOUT_STOPPED_MINI =
    T_WARN "Fleet\tE-STOPPED\n" "Stop by\tcontrol surface\n" "Reason\tcontrol surface E-STOP\n"
    "Stopped\t12 s ago\n" "Orch\tlab-orch\n" T_OK "Nodes\t2/2\n" T_OK "Watches\t1 on duty\n"
    "Lease\tidn-test, direct\n" "Boards\t0 Run, 2 Standby";
static const char *const READOUT_STOPPED_TOOL =
    T_WARN "Fleet\tE-STOPPED\n" "Stop by\ttool\n"
    "Reason\tidn-test: point buffer underrun on CUBE_0008 while armed, stopping the fleet\n"
    "Stopped\t3 s ago\n" "Orch\tlab-orch\n" T_WARN "Nodes\t1/2\n" T_OK "Watches\t1 on duty\n"
    "Lease\tidn-test, direct\n" "Boards\t0 Run, 2 Standby";
/* EMP/1.9: the fleet's detail page, cells 10..15 (Checks, Lease, Nodes, Boards, Stops, Orch). */
static const char *const DETAIL[6] = {
    T_OK "Beacon\tok\n" T_OK "Nodes\t2/2\n" T_OK "Watches\t1\n" T_OK "Lease\theld\n"
    T_OK "Boards\tready\n" T_OK "E-stop\tclear",
    "Holder\tidn-test\n" "Mode\tdirect\n" "Since\t14 min\n" T_OK "Renew\t2 s ago\n" "Expires\t28 s",
    T_OK "Node-1\tup 3 d\n" T_OK "Node-2\tup 3 d\n" "Ping\t0.3 ms\n" "Lasers\t2",
    "0007\tStandby\n" "0008\tStandby\n" "FW\t2.4.1\n" T_OK "Temp\t41, 43 C",
    "Last\tMini\n" "When\t4 min ago\n" "Why\tE-STOP\n" "Cleared\t3 min\n" "Today\t3",
    "Host\tlab-orch\n" "Rev\t09e7853\n" T_OK "Beacon\t0.4 s\n" "Up\t2 h 14 min\n" "Addr\t10.0.1.10",
};

/* The top row since av.daemon-extension minor 1 (docs/graphics-evidence/top-row): strings as
 * control-surface-router/src/system/top.rs and arm.rs write them. */
#define FLEET_LINES T_OK "Nodes\t4/4\n" T_OK "Projectors\t3/3"
static void scene_top_row(const char *which)
{
    fixture_state_button arm_ready = {"ARM", EMP_STYLE_HOLD, EMP_TONE_OK, 1, "DISARMED\narm checks met"};
    fixture_state_button arm_unmet = {"ARM", EMP_STYLE_HOLD, EMP_TONE_WARN, 0, "NOT READY\nno watch"};
    fixture_state_button arm_power = {"ARM", EMP_STYLE_HOLD, EMP_TONE_WARN, 0, "NOT READY\nlaser power"};
    fixture_state_button disarm    = {"DISARM", EMP_STYLE_NORMAL, EMP_TONE_ALERT, 1, "ARMED\npress to disarm"};
    fixture_state_button clear     = {"CLEAR E-STOP", EMP_STYLE_HOLD, EMP_TONE_WARN, 1,
                                      "E-STOPPED\ncontrol surface, 12 s ago: control surface E-STOP"};
    const char *applied = T_OK "Power\tapplied\n" FLEET_LINES;
    if      (strcmp(which, "disarmed") == 0)    fixture_top_page(&arm_ready, 60, applied);
    else if (strcmp(which, "unmet") == 0)       fixture_top_page(&arm_unmet, 60, applied);
    else if (strcmp(which, "armed") == 0)       fixture_top_page(&disarm, 60, applied);
    else if (strcmp(which, "stopped") == 0)     fixture_top_page(&clear, 60, applied);
    else if (strcmp(which, "ramping") == 0)
        fixture_top_page(&arm_ready, 60, "Power\tramping 34->60 %\n" FLEET_LINES);
    else if (strcmp(which, "unconfirmed") == 0)
        fixture_top_page(&arm_power, 60, T_WARN "Power\tunconfirmed: 2/3 boards\n" FLEET_LINES);
    else if (strcmp(which, "app") == 0)         fixture_top_app_page(&arm_ready, 60, applied);
    else if (strcmp(which, "transport") == 0)   fixture_transport_page(&arm_ready, 60, applied);
    else { fprintf(stderr, "unknown top-row scene %s\n", which); exit(2); }
    ui_state_tick(100);
    ui_render();
}

static void scene_system_page(const char *which)
{
    uint32_t t = 100;
    if (strcmp(which, "opens") == 0 || strcmp(which, "detail") == 0) {
        /* "opens": the system page with System marked OPENS. "detail": the page its press reveals
         * (REVEAL 10): the pinned E-STOP panel and the six detail lists. */
        int detail = strcmp(which, "detail") == 0;
        fixture_system_page_ex("DISARMED\nlasers dark", EMP_TONE_OK, 0, "READY\narm checks met", EMP_TONE_OK, 1,
                               READOUT_DISARMED, 1, detail ? DETAIL : 0);
        ui_state_tick(t);
        if (detail) ui_state_reveal(10);
        ui_render();
        return;
    }
    if (strcmp(which, "legacy") == 0 || strcmp(which, "legacy-stopped") == 0) {
        sim_wire_set_peer_minor(7);
        fixture_system_page_17(strcmp(which, "legacy-stopped") == 0);
        ui_render();
        return;
    }
    if (strcmp(which, "disarmed") == 0 || strncmp(which, "readout-", 8) == 0 || strcmp(which, "arming") == 0)
        fixture_system_page("DISARMED\nlasers dark", EMP_TONE_OK, 0,
                            "READY\narm checks met", EMP_TONE_OK, 1, READOUT_DISARMED);
    else if (strcmp(which, "unmet") == 0)
        fixture_system_page("DISARMED\nlasers dark", EMP_TONE_OK, 0,
                            "NOT READY\nno node", EMP_TONE_WARN, 0, READOUT_UNMET);
    else if (strcmp(which, "unavailable") == 0)
        fixture_system_page("DISARMED\nlasers dark", EMP_TONE_OK, 0,
                            "UNAVAILABLE\nnot mapped", EMP_TONE_NEUTRAL, 0, READOUT_DISARMED);
    else if (strcmp(which, "armed") == 0)
        fixture_system_page("ARMED\nlasers may emit", EMP_TONE_ALERT, 0,
                            "ARMED\nalready armed", EMP_TONE_ALERT, 0, READOUT_ARMED);
    else if (strcmp(which, "stopped-mini") == 0 || strcmp(which, "clearing") == 0 || strcmp(which, "cleared") == 0)
        fixture_system_page("E-STOPPED\ncontrol surface, 12 s ago: control surface E-STOP", EMP_TONE_WARN, 1,
                            "STOPPED\nclear E-stop first", EMP_TONE_WARN, 0, READOUT_STOPPED_MINI);
    else if (strcmp(which, "stopped-tool") == 0)
        fixture_system_page("E-STOPPED\ntool, 3 s ago: idn-test: point buffer underrun on CUBE_0008 while armed, stopping the fleet",
                            EMP_TONE_WARN, 1, "STOPPED\nclear E-stop first", EMP_TONE_WARN, 0,
                            READOUT_STOPPED_TOOL);
    else if (strncmp(which, "app", 3) == 0) {
        /* An application's page with the fleet's slots reserved (pinned). "app-2" is its second
         * page, "app-stopped" the first while stopped, "app-clearing" a clear held half way. */
        int stopped = strstr(which, "stopped") || strstr(which, "clearing");
        fixture_reserved_app_page(stopped ? "E-STOPPED\ncontrol surface, 12 s ago: control surface E-STOP"
                                          : "DISARMED\nlasers dark",
                                  stopped ? EMP_TONE_WARN : EMP_TONE_OK, stopped);
        ui_state_tick(t);
        if (strcmp(which, "app-2") == 0) ui_state_button(UI_BTN_PAGE_NEXT, 1, t), ui_state_button(UI_BTN_PAGE_NEXT, 0, t + 10);
        if (strcmp(which, "app-clearing") == 0) { ui_state_button(UI_BTN_PAGE_NEXT, 1, t); ui_state_button(UI_BTN_PAGE_NEXT, 0, t + 10);
                                                  ui_state_push(1, 1, t + 20); ui_state_tick(t + 770); }
        ui_render();
        return;
    }
    else if (strcmp(which, "no-orch") == 0)
        fixture_system_page("NO ORCH\nno orchestrator beacon", EMP_TONE_NEUTRAL, 0,
                            "UNAVAILABLE\nno orchestrator", EMP_TONE_NEUTRAL, 0,
                            "Fleet\tNO ORCH\n" "Nodes\t--\n" "Watches\t--");
    else { fprintf(stderr, "unknown system scene %s\n", which); exit(2); }

    ui_state_tick(t);
    if (strcmp(which, "clearing") == 0) { ui_state_push(1, 1, t); ui_state_tick(t + 750); }
    if (strcmp(which, "cleared") == 0)  { ui_state_push(1, 1, t); ui_state_tick(t + 1600); }
    if (strcmp(which, "arming") == 0)   { ui_state_push(2, 1, t); ui_state_tick(t + 750); }
    if (strncmp(which, "readout-", 8) == 0) ui_state_rotate(3, UI_LIST_STEP * atoi(which + 8), t);
    ui_render();
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

    if (strncmp(scene, "sys-", 4) == 0) { scene_system_page(scene + 4); write_ppm(out); return 0; }
    if (strncmp(scene, "top-", 4) == 0) { scene_top_row(scene + 4); write_ppm(out); return 0; }
    if      (strncmp(scene,"sparse-",7)==0) {fixture_sparse((unsigned)atoi(scene+7),0);ui_render();}
    else if (strncmp(scene,"toggle",6)==0) {
        fixture_toggles();
        if(strstr(scene,"focus")) {
            unsigned knob=strstr(scene,"bottom") ? 4u : 0u;
            surf_set_bool((uint16_t)knob,strstr(scene,"on")!=NULL);
            ui_state_push(knob,1,100);ui_state_push(knob,0,120);
        }
        if(strstr(scene,"group")) ui_state_highlights(3,1500);
        if(strstr(scene,"remote")) ui_state_highlight(0,1500);
        if(strstr(scene,"local")) ui_state_touch(1,100);
        ui_render();
    }
    else if (strcmp(scene,"holes")==0) {fixture_sparse(4,1);ui_render();}
    /* EMP/1.6: estop plate top-left, status tones. status-stopped re-tones via TONED_VALUES. */
    else if (strcmp(scene,"status")==0) {fixture_status(EMP_TONE_OK,"ARMED - 2/2 nodes");ui_render();}
    else if (strcmp(scene,"status-stopped")==0) {fixture_status(EMP_TONE_OK,"ARMED - 2/2 nodes");ui_render();
        fixture_toned_text(1,EMP_TONE_ALERT,"STOPPED - control surface");ui_render();}
    else if (strcmp(scene,"status-press")==0) {fixture_status(EMP_TONE_WARN,"NO ORCHESTRATOR");ui_render();
        ui_state_touch(1,100);ui_state_push(0,1,120);ui_render();}
    /* EMP/1.7: HOLD TO CLEAR at rest, part way through a hold, and after it has fired. */
    else if (strcmp(scene,"status-hold")==0) {fixture_status_hold();ui_render();}
    else if (strcmp(scene,"status-holding")==0) {fixture_status_hold();ui_state_tick(100);ui_state_push(1,1,100);
        ui_state_tick(100+EMP_HOLD_MS*3/5);ui_render();}
    else if (strcmp(scene,"status-held")==0) {fixture_status_hold();ui_state_tick(100);ui_state_push(1,1,100);
        ui_state_tick(100+EMP_HOLD_MS);ui_render();}
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
    else if (strcmp(scene, "modified") == 0) ui_render_demo_modified();
    else if (strcmp(scene, "modified-focus") == 0) ui_render_demo_modified_focused();
    else if (strcmp(scene, "focus-zoom") == 0) ui_render_demo_focused_zoom();
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
