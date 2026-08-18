/* What the surface layer would have put on the wire, captured in memory.
 *
 * The struct lived inside sim_wire.c, so the only way for a test to look at a captured message
 * was to restate the layout — two definitions that must agree and no compiler checking that they
 * do. One header instead.
 */

#ifndef SIM_WIRE_H
#define SIM_WIRE_H

#include <stdint.h>

typedef struct {
    uint8_t  channel;
    uint8_t  opcode;
    uint32_t len;
    uint8_t  payload[256];
} sim_wire_msg_t;

unsigned              sim_wire_count(void);
void                  sim_wire_reset(void);
const sim_wire_msg_t *sim_wire_at(unsigned i);

/* Most recent message carrying a given opcode, or NULL. */
const sim_wire_msg_t *sim_wire_last(uint8_t opcode);

void sim_wire_dump(void);

#endif /* SIM_WIRE_H */
