/* EMP/1 fragment layer.
 *
 * See docs/protocol.md section 2. The design decisions worth restating here, because they are
 * the ones a maintainer is most likely to "simplify" back into the bugs they prevent:
 *
 *  - `payload_len` is authoritative. A short packet or a zero-length packet is NEVER a
 *    delimiter. Treating them as one is a classic source of silent hangs, and the old SysEx
 *    path was full of length-inference of exactly this kind.
 *
 *  - The opcode is repeated on every fragment so a continuation that disagrees with the
 *    message in progress can be rejected. That single check is the detector for the bug class
 *    that used to truncate a 43 KB transfer into a plausible-looking short one.
 *
 *  - There is no per-fragment CRC. USB already CRC-16s every packet in hardware; what it
 *    cannot catch is our own software corrupting a reassembly, so the integrity budget is
 *    spent there instead — one CRC-32C over the whole message.
 *
 *  - A control message may never be fragmented (F1). Splitting one would let it queue behind
 *    a bulk transfer, which is precisely the priority inversion that makes liveness
 *    measurement useless at the moment it matters most.
 */

#include <string.h>
#include "frame.h"

/* CRC-32C (Castagnoli), reflected, nibble-at-a-time.
 *
 * A 16-entry table rather than the usual 256: 64 bytes of flash against 1 KB, for roughly half
 * the speed. At the message sizes here (a descriptor field is 60-250 bytes) the difference is
 * a few microseconds and the flash matters more.
 *
 * Pinned by test against the vector in the spec: crc32c("123456789") == 0xE3069283. */
static const uint32_t crc_tbl[16] = {
    0x00000000U, 0x105EC76FU, 0x20BD8EDEU, 0x30E349B1U,
    0x417B1DBCU, 0x5125DAD3U, 0x61C69362U, 0x7198540DU,
    0x82F63B78U, 0x92A8FC17U, 0xA24BB5A6U, 0xB21572C9U,
    0xC38D26C4U, 0xD3D3E1ABU, 0xE330A81AU, 0xF36E6F75U,
};

uint32_t emp_crc32c_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ crc_tbl[crc & 0x0Fu];
        crc = (crc >> 4) ^ crc_tbl[crc & 0x0Fu];
    }
    return crc;
}

uint32_t emp_crc32c(const uint8_t *data, uint32_t len)
{
    return ~emp_crc32c_update(0xFFFFFFFFU, data, len);
}

/* ---------------------------------------------------------------- header */

/* Everything is little-endian and nothing is aligned. All access goes through byte stores, and
 * no implementation may cast a buffer pointer to a struct — padding to keep an f64 8-aligned
 * would cost wire bytes on the transport where bytes are scarcest, and an unaligned struct
 * access is undefined behaviour regardless of what this core tolerates. */
static void put_u16(uint8_t *d, uint16_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }
static uint16_t get_u16(const uint8_t *d)   { return (uint16_t)(d[0] | ((uint16_t)d[1] << 8)); }

static void put_u32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

void emp_header_write(uint8_t *dst, const emp_header_t *h)
{
    dst[0] = EMP_MAGIC;
    dst[1] = (uint8_t)(((h->version_major & 0x0Fu) << 4) | (h->channel & 0x0Fu));
    dst[2] = h->opcode;
    dst[3] = h->flags;
    put_u16(dst + 4, h->seq);
    put_u16(dst + 6, h->payload_len);
}

int emp_header_read(const uint8_t *src, uint32_t avail, emp_header_t *out)
{
    if (avail < EMP_HEADER_BYTES) return EMP_ERR_SHORT;
    if (src[0] != EMP_MAGIC)      return EMP_ERR_BAD_MAGIC;

    out->version_major = (uint8_t)(src[1] >> 4);
    out->channel       = (uint8_t)(src[1] & 0x0Fu);
    out->opcode        = src[2];
    out->flags         = src[3];
    out->seq           = get_u16(src + 4);
    out->payload_len   = get_u16(src + 6);

    /* Checked on the FIRST fragment, before any payload is trusted — earlier and cheaper than
     * discovering a version mismatch inside HELLO. */
    if (out->version_major != EMP_VERSION_MAJOR) return EMP_ERR_BAD_VERSION;

    if ((uint32_t)out->payload_len + EMP_HEADER_BYTES > avail) return EMP_ERR_SHORT;
    return EMP_OK;
}

int emp_is_padding(const uint8_t *src, uint32_t avail)
{
    for (uint32_t i = 0; i < avail; i++) {
        if (src[i] != 0) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- writing */

void emp_writer_init(emp_writer_t *w, uint8_t *out, uint32_t cap, uint16_t mtu)
{
    w->out = out;
    w->cap = cap;
    w->used = 0;
    w->mtu = mtu;
    w->seq = 0;
}

static int write_fragment(emp_writer_t *w, uint8_t channel, uint8_t opcode, uint8_t flags,
                          const uint8_t *a, uint32_t alen,
                          const uint8_t *b, uint32_t blen)
{
    uint32_t total = alen + blen;
    if (w->used + EMP_HEADER_BYTES + total > w->cap) return EMP_ERR_NO_SPACE;

    emp_header_t h;
    h.version_major = EMP_VERSION_MAJOR;
    h.channel       = channel;
    h.opcode        = opcode;
    h.flags         = flags;
    h.seq           = w->seq++;
    h.payload_len   = (uint16_t)total;

    emp_header_write(w->out + w->used, &h);
    w->used += EMP_HEADER_BYTES;

    if (alen) { memcpy(w->out + w->used, a, alen); w->used += alen; }
    if (blen) { memcpy(w->out + w->used, b, blen); w->used += blen; }
    return EMP_OK;
}

int emp_write_message(emp_writer_t *w, uint8_t channel, uint8_t opcode,
                      const uint8_t *payload, uint32_t len)
{
    if (len > EMP_MAX_MESSAGE) return EMP_ERR_TOO_LONG;

    if (len <= w->mtu) {
        return write_fragment(w, channel, opcode,
                              (uint8_t)(EMP_FLAG_FIRST | EMP_FLAG_LAST),
                              payload, len, 0, 0);
    }

    /* F1: control must never be split. Rejecting here rather than fragmenting keeps the
     * guarantee structural instead of relying on every caller to remember it. */
    if (channel == EMP_CH_CONTROL) return EMP_ERR_BAD_CHANNEL;

    /* First fragment carries the message prefix: total length so the receiver can bound its
     * copy before taking a single byte, and the CRC so a corrupted reassembly is detected
     * rather than delivered. */
    uint8_t prefix[EMP_PREFIX_BYTES];
    put_u32(prefix + 0, len);
    put_u32(prefix + 4, emp_crc32c(payload, len));

    uint32_t first_payload = w->mtu - EMP_PREFIX_BYTES;
    if (first_payload > len) first_payload = len;

    int rc = write_fragment(w, channel, opcode, EMP_FLAG_FIRST,
                            prefix, EMP_PREFIX_BYTES, payload, first_payload);
    if (rc != EMP_OK) return rc;

    uint32_t off = first_payload;
    while (off < len) {
        uint32_t n = len - off;
        uint8_t flags = 0;
        if (n <= w->mtu) { flags = EMP_FLAG_LAST; } else { n = w->mtu; }

        rc = write_fragment(w, channel, opcode, flags, payload + off, n, 0, 0);
        if (rc != EMP_OK) return rc;
        off += n;
    }
    return EMP_OK;
}

/* ---------------------------------------------------------------- reading */

void emp_reasm_init(emp_reasm_t *r)
{
    memset(r, 0, sizeof(*r));
}

static int fail(emp_reasm_t *r, int err)
{
    r->rx_decode_errors++;
    r->in_progress = 0;
    r->len = 0;
    return err;
}

int emp_reasm_fragment(emp_reasm_t *r, const uint8_t *frag, uint32_t avail,
                       uint32_t *consumed, emp_header_t *hdr_out,
                       const uint8_t **msg, uint32_t *msg_len)
{
    emp_header_t h;
    int rc = emp_header_read(frag, avail, &h);
    if (rc != EMP_OK) {
        if (consumed) *consumed = avail;
        /* A short or non-magic remainder is the caller's business to classify as padding; only
         * a structurally invalid header counts against the error stats. */
        if (rc == EMP_ERR_SHORT || rc == EMP_ERR_BAD_MAGIC) return rc;
        return fail(r, rc);
    }

    if (consumed) *consumed = EMP_HEADER_BYTES + h.payload_len;
    if (hdr_out)  *hdr_out = h;

    r->rx_fragments++;

    /* A sequence gap means something structural — a ring overrun or a pipe reset — not a wire
     * error, because USB is lossless while the pipe is up. It is counted and reported, never
     * repaired: at 10-140 ms per descriptor, resyncing is cheaper than retransmission
     * machinery, and a gap that is merely retried would hide the fault that caused it. */
    if (r->have_seq && h.seq != r->next_seq) r->rx_seq_gaps++;
    r->next_seq = (uint16_t)(h.seq + 1);
    r->have_seq = 1;

    const uint8_t *body = frag + EMP_HEADER_BYTES;
    uint32_t blen = h.payload_len;

    /* Single fragment: delivered in place, no copy and no CRC. Most messages on this device
     * take this path, and it is deliberately the cheapest one. */
    if ((h.flags & EMP_FLAG_FIRST) && (h.flags & EMP_FLAG_LAST)) {
        if (r->in_progress) return fail(r, EMP_ERR_FRAGMENT_UNEXPECTED);
        if (msg) *msg = body;
        if (msg_len) *msg_len = blen;
        return 1;
    }

    if (h.flags & EMP_FLAG_FIRST) {
        if (h.channel == EMP_CH_CONTROL) return fail(r, EMP_ERR_BAD_CHANNEL);
        if (blen < EMP_PREFIX_BYTES)     return fail(r, EMP_ERR_SHORT);

        uint32_t total = get_u32(body);
        if (total > EMP_MAX_MESSAGE_RX) return fail(r, EMP_ERR_TOO_LONG);

        r->expect_total = total;
        r->expect_crc   = get_u32(body + 4);
        r->opcode       = h.opcode;
        r->channel      = h.channel;
        r->in_progress  = 1;
        r->len          = 0;

        body += EMP_PREFIX_BYTES;
        blen -= EMP_PREFIX_BYTES;
    } else {
        /* A continuation whose opcode disagrees with the message in progress is the detector
         * for interleaved-transfer corruption. Rejecting it is what makes the old truncation
         * bug impossible rather than merely unlikely. */
        if (!r->in_progress)        return fail(r, EMP_ERR_FRAGMENT_UNEXPECTED);
        if (h.opcode != r->opcode)  return fail(r, EMP_ERR_FRAGMENT_UNEXPECTED);
        if (h.channel != r->channel) return fail(r, EMP_ERR_FRAGMENT_UNEXPECTED);
    }

    if (r->len + blen > r->expect_total)  return fail(r, EMP_ERR_TOO_LONG);
    if (r->len + blen > sizeof(r->buf))   return fail(r, EMP_ERR_TOO_LONG);

    memcpy(r->buf + r->len, body, blen);
    r->len += blen;

    if (!(h.flags & EMP_FLAG_LAST)) return 0;

    if (r->len != r->expect_total) return fail(r, EMP_ERR_TOO_LONG);
    if (emp_crc32c(r->buf, r->len) != r->expect_crc) return fail(r, EMP_ERR_CRC);

    r->in_progress = 0;
    if (msg) *msg = r->buf;
    if (msg_len) *msg_len = r->len;
    return 1;
}
