#ifndef ELECTRA_CONSOLE_H
#define ELECTRA_CONSOLE_H

#include <stdint.h>

typedef struct {
    const char *name;
    const char *help;
    void      (*fn)(const char *arg);
} console_cmd_t;

void console_init(const console_cmd_t *table, uint32_t count);
void console_poll(void);
void console_write(const char *s);
void console_hex(const char *label, uint32_t v);
void console_help(const char *arg);

/* A binary protocol may share the pipe. It is offered each received byte and returns 1 if it
 * consumed it; anything it declines is treated as console text. See console.c. */
typedef int (*console_sink_fn)(uint8_t b);
void console_set_binary_sink(console_sink_fn fn);

#endif /* ELECTRA_CONSOLE_H */
