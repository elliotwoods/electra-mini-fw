/* The only thing the real surface layer needs from outside a device: a way to send bytes.
 *
 * This replaces tools/sim/sim_surface.c, which was a hand-written stand-in for the whole
 * surface layer. That file's own header warned that "a stub that drifts from the thing it
 * stands in for is worse than no stub", and it had already drifted: the real descriptor grew a
 * `step` field and EMP_PRESENT_STEP, and the stub never did — so the simulator was judging the
 * UI against fields the device would never actually hold.
 *
 * src/proto/surface.c turns out to need exactly one external symbol, `emp_send`, so compiling
 * the real thing costs this file and nothing else. That also makes the simulator strictly more
 * useful than it was: what the device WOULD have put on the wire is now observable on the
 * host, so the input path can be tested without a device attached.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "emp.h"
#include "sim_wire.h"

#define SIM_WIRE_CAPACITY 64

static sim_wire_msg_t captured[SIM_WIRE_CAPACITY];
static unsigned       captured_count;

void emp_send(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    if (captured_count >= SIM_WIRE_CAPACITY) return;

    sim_wire_msg_t *m = &captured[captured_count++];
    m->channel = channel;
    m->opcode  = opcode;
    m->len     = len;

    uint32_t n = len < sizeof(m->payload) ? len : (uint32_t)sizeof(m->payload);
    if (payload && n) memcpy(m->payload, payload, n);
}

/* --- inspection, for tests and for the simulator's own reporting ------------ */

unsigned sim_wire_count(void) { return captured_count; }
void     sim_wire_reset(void) { captured_count = 0; }

const sim_wire_msg_t *sim_wire_at(unsigned i)
{
    return (i < captured_count) ? &captured[i] : 0;
}

const sim_wire_msg_t *sim_wire_last(uint8_t opcode)
{
    for (unsigned i = captured_count; i-- > 0; ) {
        if (captured[i].opcode == opcode) return &captured[i];
    }
    return 0;
}

static const char *opcode_name(uint8_t op)
{
    switch (op) {
    case EMP_OP_EDIT:         return "EDIT";
    case EMP_OP_EDIT_DELTA:   return "EDIT_DELTA";
    case EMP_OP_FOCUS:        return "FOCUS";
    case EMP_OP_BUTTON:       return "BUTTON";
    case EMP_OP_DESC_REQUEST: return "DESC_REQUEST";
    case EMP_OP_DESC_ACK:     return "DESC_ACK";
    case EMP_OP_RESET:        return "RESET";
    case EMP_OP_PLAY_PAUSE:   return "PLAY_PAUSE";
    case EMP_OP_SCREEN:       return "SCREEN";
    default:                  return "?";
    }
}

void sim_wire_dump(void)
{
    if (!captured_count) {
        printf("  (nothing was sent)\n");
        return;
    }
    for (unsigned i = 0; i < captured_count; i++) {
        const sim_wire_msg_t *m = &captured[i];
        printf("  ch%u %-12s (0x%02X) %2u bytes:", m->channel, opcode_name(m->opcode),
               m->opcode, m->len);
        uint32_t n = m->len < 16 ? m->len : 16;
        for (uint32_t j = 0; j < n; j++) printf(" %02X", m->payload[j]);
        if (m->len > n) printf(" ...");
        printf("\n");
    }
}
