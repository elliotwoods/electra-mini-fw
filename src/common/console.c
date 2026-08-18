/* Line-oriented command console over USB CDC, shared by the bootloader and the application.
 *
 * Text, not binary, and deliberately so: the device shows up as a COM port, so any terminal
 * drives it with no tooling at all. When something is wrong at 3am that matters more than
 * the few bytes a binary framing would save. The bulk transfer used for flashing is binary
 * and escapes this, but everything control-plane is typeable.
 *
 * The command tables differ between the two images; the parsing does not.
 */

#include "console.h"
#include "usb_fs.h"

#define LINE_MAX 96

static char     line[LINE_MAX];
static uint32_t line_len;

static const console_cmd_t *cmds;
static uint32_t             cmd_count;

void console_init(const console_cmd_t *table, uint32_t count)
{
    cmds = table;
    cmd_count = count;
    line_len = 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void console_write(const char *s) { usb_puts(s); }

void console_hex(const char *label, uint32_t v) { usb_printf_u32(label, v); }

static void dispatch(void)
{
    if (line_len == 0) { console_write("> "); return; }
    line[line_len] = 0;

    /* Split off the first token; the rest is passed through as arguments. */
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg == ' ') { *arg = 0; arg++; } else { arg = line + line_len; }

    for (uint32_t i = 0; i < cmd_count; i++) {
        if (str_eq(line, cmds[i].name)) {
            cmds[i].fn(arg);
            console_write("> ");
            return;
        }
    }

    console_write("? unknown command. try 'help'\r\n> ");
}

void console_help(const char *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < cmd_count; i++) {
        console_write(cmds[i].name);
        console_write("  ");
        console_write(cmds[i].help);
        console_write("\r\n");
    }
}

/* Optional binary sink sharing the same pipe.
 *
 * The EMP protocol and this console both arrive on the one bulk endpoint, and they are told
 * apart by the first byte: an EMP fragment starts with 0xE1, which no console line can. The
 * sink is offered every byte first and says whether it took it.
 *
 * Keeping both alive is a deliberate choice. Every hardware fault in this project was
 * diagnosed through the text console, and a binary protocol that displaced its own debugging
 * tool would be a poor trade — particularly since the flashing path also speaks console. */
static console_sink_fn binary_sink;

void console_set_binary_sink(console_sink_fn fn) { binary_sink = fn; }

void console_poll(void)
{
    uint8_t buf[64];
    uint32_t n = usb_read(buf, sizeof(buf));

    for (uint32_t i = 0; i < n; i++) {
        if (binary_sink && binary_sink(buf[i])) continue;

        char c = (char)buf[i];

        if (c == '\r' || c == '\n') {
            usb_puts("\r\n");
            dispatch();
            line_len = 0;
        } else if (c == 8 || c == 127) {              /* backspace */
            if (line_len) { line_len--; usb_puts("\b \b"); }
        } else if (line_len < LINE_MAX - 1 && c >= 0x20 && c < 0x7F) {
            line[line_len++] = c;
            char echo[2] = { c, 0 };
            usb_puts(echo);                            /* local echo: terminals expect it */
        }
    }
}
