/* Outbound queues for device-to-host traffic.
 *
 * NOTHING SENDS FROM WHERE IT IS PRODUCED. Everything is queued here and drained by txq_pump()
 * from the service loop.
 *
 * The path this replaces framed each message synchronously and handed it to a blocking write
 * that spins up to twenty thousand times waiting for room, then drops what is left into a
 * counter the host never sees. Two consequences, both of which showed on the panel:
 *
 *   A knob turned quickly produced a message per detent, each one blocking INSIDE the rotation
 *   handler. The instrument stopped redrawing while the hand was still moving, which reads as
 *   the device being slow rather than as the link being full.
 *
 *   When the spin finally gave up, the loss landed in a console counter while the on-wire
 *   tx_dropped stayed at zero. The host was told the link was perfect at precisely the moments
 *   it was not.
 *
 * Three queues, per docs/protocol.md 4 plus rule F4:
 *
 *   COALESCING (EDIT, EDIT_DELTA, FOCUS, SCREEN): one slot per opcode and field id. A newer
 *   event for the same field supersedes the older one, so a queue that cannot drain degrades in
 *   RESOLUTION rather than losing events -- the knob still arrives where the hand left it.
 *
 *   DISCRETE (RESET, PLAY_PAUSE, BUTTON): a FIFO, never coalesced. A button press is not an
 *   approximation of anything; two presses are not one press, so merging them would be wrong in
 *   a way that merging two positions of the same knob is not.
 *
 *   CONTROL: its own queue, so channel 0 can never be starved by a burst of knob movement --
 *   which is exactly when liveness matters most.
 *
 * Transmission is injected as a callback rather than called directly, for the reason given in
 * docs/protocol.md 7: this file depends on <stdint.h> and <string.h> only, so the queueing and
 * coalescing rules can be tested on a workstation with no device and no MCU headers.
 */

#ifndef EMP_TXQ_H
#define EMP_TXQ_H

#include <stdint.h>

/* Frame and transmit one message. Returns non-zero if it went; zero if the transport is full,
 * in which case the entry stays queued and the pump stops for this visit. MUST NOT block. */
typedef int (*txq_emit_fn)(uint8_t channel, uint8_t opcode,
                           const uint8_t *payload, uint32_t len, void *ctx);

/* Report a drop. Called with the opcode that was lost and a reason, so the loss reaches the
 * host as a DIAG instead of accumulating in a counter nobody reads. */
typedef void (*txq_drop_fn)(uint8_t channel, uint8_t opcode, const char *why, void *ctx);

#define TXQ_PAYLOAD_MAX     48u   /* the largest input message: EDIT carries revision + f64 */
#define TXQ_COALESCE_SLOTS  16u   /* eight knobs, two coalescing opcodes each */
#define TXQ_DISCRETE_DEPTH  32u   /* docs/protocol.md 4 names this depth */
#define TXQ_CONTROL_SLOTS   4u
#define TXQ_CONTROL_MAX     264u  /* READY is the big one: 44 fixed bytes plus three strings */

typedef struct {
    uint32_t stamp;               /* 0 = empty; otherwise enqueue order */
    uint16_t key;                 /* field id, or 0 for the singletons */
    uint8_t  channel;
    uint8_t  opcode;
    uint8_t  len;
    uint8_t  payload[TXQ_PAYLOAD_MAX];
} txq_slot_t;

typedef struct {
    uint16_t len;
    uint8_t  opcode;
    uint8_t  payload[TXQ_CONTROL_MAX];
} txq_control_t;

typedef struct {
    txq_slot_t    coalesce[TXQ_COALESCE_SLOTS];
    txq_slot_t    discrete[TXQ_DISCRETE_DEPTH];
    txq_control_t control[TXQ_CONTROL_SLOTS];

    uint32_t stamp;
    uint8_t  discrete_head, discrete_count;
    uint8_t  control_head, control_count;

    uint32_t dropped;             /* every drop, counted; reported on the wire as tx_dropped */

    txq_emit_fn emit;
    txq_drop_fn drop;
    void       *ctx;
} txq_t;

void txq_init(txq_t *q, txq_emit_fn emit, txq_drop_fn drop, void *ctx);

/* Discard everything queued, keeping the drop count. For losing the host: a knob position from
 * before the cable was pulled would arrive as a fresh edit and overwrite whatever the host has
 * since decided, which is worse than never sending it. */
void txq_clear(txq_t *q);

/* Queue one message. Never blocks and never transmits. */
void txq_push(txq_t *q, uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len);

/* Send what the transport will take, in order, control first. Returns the number of messages
 * emitted. Stops at the first refusal rather than spinning. */
unsigned txq_pump(txq_t *q);

/* How many messages are waiting, across all three queues. */
unsigned txq_depth(const txq_t *q);

#endif /* EMP_TXQ_H */
