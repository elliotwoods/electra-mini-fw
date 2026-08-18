/* EMP/1 fragment layer: header codec, message writer, reassembler.
 *
 * Pure functions over caller-owned buffers. No allocation, no I/O, no statics — so the same
 * source compiles for the device and for a host test runner, and the wire format is exercised
 * on every build rather than only when hardware is attached.
 */

#ifndef ELECTRA_EMP_FRAME_H
#define ELECTRA_EMP_FRAME_H

#include <stdint.h>
#include "emp.h"

typedef struct {
    uint8_t  version_major;
    uint8_t  channel;
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t seq;
    uint16_t payload_len;
} emp_header_t;

uint32_t emp_crc32c(const uint8_t *data, uint32_t len);

/* Incremental form, for CRCing a message that is being emitted fragment by fragment.
 * Seed with 0xFFFFFFFF; finish by inverting. */
uint32_t emp_crc32c_update(uint32_t crc, const uint8_t *data, uint32_t len);

/* Writes EMP_HEADER_BYTES. The caller guarantees the space. */
void emp_header_write(uint8_t *dst, const emp_header_t *h);

/* Reads a header. Returns EMP_OK or a negative error.
 *
 * Distinguishes "this is padding" from "this is corrupt": a receiver walking a transfer stops
 * at the first byte that is not the magic, and it matters whether the remainder is zeros
 * (benign padding from a fixed-size report) or something else (a real framing fault). */
int emp_header_read(const uint8_t *src, uint32_t avail, emp_header_t *out);

/* True if the remaining bytes are all zero, i.e. legitimate trailing padding. */
int emp_is_padding(const uint8_t *src, uint32_t avail);

/* ---------------------------------------------------------------- writing */

typedef struct {
    uint8_t  *out;
    uint32_t  cap;
    uint32_t  used;
    uint16_t  mtu;         /* payload bytes per fragment, transport-dependent */
    uint16_t  seq;         /* per-direction fragment counter; caller keeps it across calls */
} emp_writer_t;

void emp_writer_init(emp_writer_t *w, uint8_t *out, uint32_t cap, uint16_t mtu);

/* Encode one message, fragmenting as needed. Returns EMP_OK or a negative error.
 *
 * Enforces F1: a channel-0 message that would not fit in a single fragment is rejected rather
 * than silently split, because control traffic that can be split can be delayed behind bulk,
 * which is the priority inversion the design exists to avoid. */
int emp_write_message(emp_writer_t *w, uint8_t channel, uint8_t opcode,
                      const uint8_t *payload, uint32_t len);

/* ---------------------------------------------------------------- reading */

typedef struct {
    uint8_t  buf[EMP_MAX_MESSAGE_RX];
    uint32_t len;

    uint32_t expect_total;
    uint32_t expect_crc;
    uint8_t  opcode;
    uint8_t  channel;
    uint8_t  in_progress;

    uint16_t next_seq;
    uint8_t  have_seq;

    /* Counters, reported in HEARTBEAT. Every drop is visible; a silently degraded link is the
     * failure this protocol exists to eliminate. */
    uint32_t rx_fragments;
    uint32_t rx_decode_errors;
    uint32_t rx_seq_gaps;
} emp_reasm_t;

void emp_reasm_init(emp_reasm_t *r);

/* Abandon a reassembly that has stopped arriving, counting it as a decode error. The caller
 * decides when, because this file has no clock -- see the note on the implementation. */
void emp_reasm_abort(emp_reasm_t *r);

/* Feed one fragment.
 *
 * Returns 1 when a complete message is available (`*msg` / `*msg_len` point into `r->buf`, or
 * at the caller's own fragment for the single-fragment case), 0 when more fragments are
 * needed, or a negative error. `*consumed` always receives the total fragment size so the
 * caller can walk a packed transfer. */
int emp_reasm_fragment(emp_reasm_t *r, const uint8_t *frag, uint32_t avail,
                       uint32_t *consumed, emp_header_t *hdr_out,
                       const uint8_t **msg, uint32_t *msg_len);

#endif /* ELECTRA_EMP_FRAME_H */
