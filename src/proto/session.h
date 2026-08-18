/* EMP/1 session: the control channel on the device side.
 *
 * Sits on top of the fragment layer in frame.c and below anything that knows what a surface
 * is. Its job is the part of the protocol that must work before the application layer means
 * anything: identify the device, answer pings, and emit an unsolicited heartbeat.
 *
 * Shares the bulk pipe with the ASCII console. The two are told apart by the first byte: an
 * EMP fragment always begins with EMP_MAGIC (0xE1), which is not a byte a console line can
 * start with. Keeping the console is deliberate — every hardware problem in this project was
 * diagnosed through it, and a protocol that displaces its own debugging tool is a bad trade.
 */

#ifndef ELECTRA_EMP_SESSION_H
#define ELECTRA_EMP_SESSION_H

#include <stdint.h>
#include "frame.h"

void emp_session_init(uint32_t session_id);

/* Offer one received byte. Returns 1 if the framer consumed it, 0 if it belongs to the
 * console. */
int emp_session_feed(uint8_t b);

/* Called regularly. Emits the unsolicited traffic the protocol owes the host: READY until a
 * HELLO has been seen, and a heartbeat once a second thereafter. `now_ms` is monotonic. */
void emp_session_poll(uint32_t now_ms);

/* Silence unsolicited traffic. Required around anything that streams unframed bytes down the
 * shared pipe — a screenshot, for instance — because interleaved protocol frames are
 * indistinguishable from payload once they are mixed in. */
void emp_session_mute(int on);

/* Report a completed repaint, in microseconds. Feeds HEARTBEAT.render_us_max, which exists so
 * the full-repaint cost stays continuously checkable from the host rather than quoted from a
 * document. Microseconds, not milliseconds: a 900 us repaint and a 1400 us one are the same
 * number in ms, and telling them apart is the entire purpose of the measurement. */
void emp_session_note_render_us(uint32_t us);

/* The MTU actually in use for transmission, after HELLO. Never larger than what this device
 * advertises it can receive, and never smaller than one HID report's worth. */
uint16_t emp_session_tx_mtu(void);

/* Counters, reported in HEARTBEAT and by the console's `emp` command. */
typedef struct {
    uint32_t rx_fragments;
    uint32_t tx_fragments;
    uint32_t rx_decode_errors;
    uint32_t rx_seq_gaps;
    uint32_t rx_messages;
    uint32_t tx_messages;
    uint32_t tx_dropped;
    uint32_t hellos;
    uint32_t pings;
    uint32_t session_id;
    int      host_seen;
} emp_stats_t;

const emp_stats_t *emp_session_stats(void);

#endif /* ELECTRA_EMP_SESSION_H */
