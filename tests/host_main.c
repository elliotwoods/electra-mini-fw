/* Host entry point for the EMP/1 codec tests.
 *
 * The same test bodies run on the device through the `selftest` console command. This one
 * exists so a codec regression is caught in a second on a workstation, with no hardware
 * attached and nothing to plug in — the device run is then confirmation on the real target
 * rather than the only place the tests have ever executed.
 */

#include <stdio.h>

typedef void (*emp_report_fn)(const char *name, int passed);
int emp_run_selftests(emp_report_fn report);
int ui_run_selftests(emp_report_fn report);
int desc_run_selftests(emp_report_fn report);

static void report(const char *name, int passed)
{
    printf("  %-42s %s\n", name, passed ? "ok" : "FAIL");
}

int main(void)
{
    printf("EMP/1 codec tests (host)\n");
    int failures = emp_run_selftests(report);

    /* Descriptor transfer, which is the only path by which the device learns what its knobs
     * mean. Its failure cases are invisible from the panel: a descriptor that fails to load
     * looks exactly like a host that has not sent one. */
    printf("\ndescriptor transfer tests (host)\n");
    failures += desc_run_selftests(report);

    /* The interaction model runs here too. It is the part of the firmware hardest to test with
     * a finger on a knob -- a focus that will not let go, or a wrong digit at three decimals,
     * shows up under your hand and disappears the moment you let go to look at it. */
    printf("\ninteraction model tests (host)\n");
    failures += ui_run_selftests(report);

    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
