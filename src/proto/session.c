/* EMP/1 control channel, device side.
 *
 * Implements section 3.1 of docs/protocol.md: HELLO/READY, PING/PONG, HEARTBEAT, ECHO.
 *
 * Three obligations here are not conveniences, they are the scars of the integration this
 * protocol replaces, and they are worth restating where the code lives:
 *
 *  - **The device identifies itself.** READY carries the firmware's own version, session id and
 *    limits. The old integration inferred the running firmware by reading a script back and
 *    comparing bytes, which was abandoned as unreliable — so the device now says what it is,
 *    unprompted, and keeps saying it until a host answers.
 *
 *  - **`session_id` changes on every boot.** A device that reset in the middle of a descriptor
 *    transfer is then detectable within one heartbeat, with no probe. Without it, a host can
 *    talk confidently to a device that has forgotten everything.
 *
 *  - **Liveness is earned by hearing back.** PONG echoes the host's own timestamp so round-trip
 *    needs no clock agreement, and the heartbeat is unsolicited so silence is itself
 *    information. `SurfacePhase` is sticky by nature — once provisioning succeeds nothing
 *    demotes it — so a crashed device would otherwise keep reporting success.
 */

#include <string.h>
#include "s7g2.h"
#include "usb_fs.h"
#include "session.h"
#include "surface.h"
#include "diag.h"

/* Bulk endpoints, so the bulk MTU. Both are 64-byte endpoints on this full-speed port; the MTU
 * is the payload we are willing to put in one fragment, not the packet size. */
#define SESSION_MTU        EMP_MTU_BULK

#define HEARTBEAT_MS       1000u
#define READY_REPEAT_MS    1000u
#define READY_BACKOFF_MS   5000u

/* Identity. Kept short: every byte is on the wire in READY. */
#define FW_VERSION         0x00000100u        /* 0.1.0 */
static const char MODEL[]    = "Electra One Mini";
static const char SERIAL[]   = "EMB-0001";
static const char BUILD_ID[] = __DATE__ " " __TIME__;

static emp_stats_t   stats;
static emp_reasm_t   reasm;
static uint32_t      next_heartbeat_ms;
static uint32_t      next_ready_ms;
static uint32_t      boot_ms;

/* Inbound fragment assembly from a byte stream.
 *
 * usb_read() hands back whatever arrived, which has nothing to do with fragment boundaries, so
 * a fragment is collected a byte at a time until payload_len says it is complete. `payload_len`
 * is authoritative: a short USB packet is never a delimiter. */
static uint8_t  rx_frag[EMP_HEADER_BYTES + SESSION_MTU + EMP_PREFIX_BYTES];
static uint32_t rx_have;
static uint32_t rx_need;

/* ------------------------------------------------------------------ encoding */

typedef struct { uint8_t *p; uint32_t cap; uint32_t len; } enc_t;

static void enc_init(enc_t *e, uint8_t *buf, uint32_t cap) { e->p = buf; e->cap = cap; e->len = 0; }

static void put8(enc_t *e, uint8_t v)
{
    if (e->len < e->cap) e->p[e->len] = v;
    e->len++;
}
static void put16(enc_t *e, uint16_t v) { put8(e, (uint8_t)v); put8(e, (uint8_t)(v >> 8)); }
static void put32(enc_t *e, uint32_t v) { put16(e, (uint16_t)v); put16(e, (uint16_t)(v >> 16)); }
static void put64(enc_t *e, uint64_t v) { put32(e, (uint32_t)v); put32(e, (uint32_t)(v >> 32)); }

/* String = u16 length + UTF-8 bytes, no terminator. No truncation happens here; if it ever
 * needs to, the spec requires cutting on a codepoint boundary and saying so out loud. */
static void put_str(enc_t *e, const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    put16(e, (uint16_t)n);
    for (uint32_t i = 0; i < n; i++) put8(e, (uint8_t)s[i]);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* ------------------------------------------------------------------ sending */

/* One writer buffer, one message at a time. Rule F4 forbids composing messages into a shared
 * partially-filled buffer — that shape was the old device-side bug — so each message is framed
 * and pushed to the endpoint before the next is built. */
static void send_message(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len);

static uint8_t tx_buf[EMP_HEADER_BYTES + SESSION_MTU + EMP_PREFIX_BYTES];
static uint16_t tx_seq;

/* Exposed so the surface layer can send without owning framing or the endpoint. Policy lives
 * in surface.c, transport lives here, and neither reaches into the other. */
void emp_send(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    send_message(channel, opcode, payload, len);
}

static void send_message(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    emp_writer_t w;
    emp_writer_init(&w, tx_buf, sizeof(tx_buf), SESSION_MTU);
    w.seq = tx_seq;

    if (emp_write_message(&w, channel, opcode, payload, len) != EMP_OK) {
        stats.tx_dropped++;
        return;
    }
    tx_seq = w.seq;

    usb_write_block(tx_buf, w.used);
    stats.tx_fragments++;
    stats.tx_messages++;
}

static void send_ready(void)
{
    uint8_t buf[256];
    enc_t e;
    enc_init(&e, buf, sizeof(buf));

    put8(&e, EMP_VERSION_MAJOR);
    put8(&e, 0);                               /* minor */
    put16(&e, SESSION_MTU);
    put32(&e, stats.session_id);
    put32(&e, 0);                              /* capability flags; none yet */
    put32(&e, EMP_MAX_MESSAGE_RX);
    put16(&e, SURF_MAX_FIELDS);
    put16(&e, SURF_POTS);                      /* eight knobs, so eight fields at once */
    put64(&e, surf_applied_revision());
    put32(&e, FW_VERSION);
    put_str(&e, MODEL);
    put_str(&e, SERIAL);
    put_str(&e, BUILD_ID);

    if (e.len <= sizeof(buf)) send_message(EMP_CH_CONTROL, EMP_OP_READY, buf, e.len);
    else stats.tx_dropped++;
}

static void send_heartbeat(uint32_t now_ms)
{
    uint8_t buf[64];
    enc_t e;
    enc_init(&e, buf, sizeof(buf));

    put32(&e, stats.session_id);
    put64(&e, surf_applied_revision());
    put32(&e, now_ms - boot_ms);               /* uptime */
    put16(&e, surf_page());
    put16(&e, surf_field_count());
    put8(&e, (uint8_t)(stats.host_seen ? 1 : 0));

    /* Every drop is reported. A silently degraded link is the failure this rewrite exists to
     * eliminate, so these counters are part of the protocol, not debug decoration. */
    put32(&e, reasm.rx_fragments);
    put32(&e, stats.tx_fragments);
    put32(&e, reasm.rx_decode_errors);
    put32(&e, reasm.rx_seq_gaps);
    put32(&e, stats.tx_dropped);
    put32(&e, 0);                              /* render_us_max, once there is rendering */

    send_message(EMP_CH_CONTROL, EMP_OP_HEARTBEAT, buf, e.len);
}

static void send_pong(const uint8_t *payload, uint32_t len, uint32_t now_ms)
{
    if (len < 12) return;

    uint8_t buf[32];
    enc_t e;
    enc_init(&e, buf, sizeof(buf));

    put32(&e, get32(payload));                 /* ping_id, echoed */
    put64(&e, get64(payload + 4));             /* the HOST's timestamp, echoed verbatim, so the
                                                * round trip needs no agreement about clocks */
    put64(&e, (uint64_t)now_ms * 1000u);       /* our own, microseconds */
    put32(&e, 0);                              /* credits_total */

    send_message(EMP_CH_CONTROL, EMP_OP_PONG, buf, e.len);
}

/* ------------------------------------------------------------------ dispatch */

static void handle(const emp_header_t *h, const uint8_t *msg, uint32_t len, uint32_t now_ms)
{
    stats.rx_messages++;

    switch (h->opcode) {
    case EMP_OP_HELLO:
        stats.hellos++;
        stats.host_seen = 1;
        /* Answered idempotently: a host that missed the first READY, or that restarted, gets
         * another without the device needing to track which host it is talking to. */
        send_ready();
        break;

    case EMP_OP_PING:
        stats.pings++;
        stats.host_seen = 1;
        send_pong(msg, len, now_ms);
        break;

    case EMP_OP_ECHO:
        send_message(EMP_CH_CONTROL, EMP_OP_ECHO_REPLY, msg, len);
        break;

    case EMP_OP_BYE:
        stats.host_seen = 0;
        break;

    default:
        if (h->channel == EMP_CH_SURFACE) {
            stats.host_seen = 1;
            surf_handle(h->opcode, msg, len);
        }
        /* Anything else is ignored, not an error. A newer host may send things a v1 device has
         * never heard of, and refusing to talk to it would be worse than skipping the message. */
        break;
    }
}

/* ------------------------------------------------------------------ receiving */

int emp_session_feed(uint8_t b)
{
    if (rx_have == 0) {
        if (b != EMP_MAGIC) return 0;          /* console traffic; not ours */
        rx_frag[rx_have++] = b;
        rx_need = EMP_HEADER_BYTES;
        return 1;
    }

    if (rx_have < sizeof(rx_frag)) rx_frag[rx_have] = b;
    rx_have++;

    if (rx_have == EMP_HEADER_BYTES) {
        /* payload_len is authoritative — never a short packet, never a delimiter. */
        uint32_t payload = (uint32_t)rx_frag[6] | ((uint32_t)rx_frag[7] << 8);
        if (payload > SESSION_MTU + EMP_PREFIX_BYTES) {
            stats.rx_decode_errors++;
            rx_have = 0;
            return 1;
        }
        rx_need = EMP_HEADER_BYTES + payload;
    }

    if (rx_have >= rx_need) {
        emp_header_t h;
        const uint8_t *msg = 0;
        uint32_t msg_len = 0, consumed = 0;

        int rc = emp_reasm_fragment(&reasm, rx_frag, rx_have, &consumed, &h, &msg, &msg_len);
        if (rc == 1) handle(&h, msg, msg_len, 0);
        rx_have = 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ public */

void emp_session_init(uint32_t session_id)
{
    memset(&stats, 0, sizeof(stats));
    emp_reasm_init(&reasm);
    emp_diag_reset();
    surf_init();
    stats.session_id = session_id;
    rx_have = 0;
    tx_seq = 0;
    boot_ms = 0;
    next_ready_ms = 0;
    next_heartbeat_ms = 0;
}

/* Unsolicited traffic has to be silenceable.
 *
 * The screenshot command streams three quarters of a megabyte of raw pixels down the same pipe.
 * A READY or a heartbeat landing in the middle of that lands in the middle of the IMAGE — the
 * host has no way to tell protocol bytes from pixel bytes once they are interleaved, because
 * raw pixels have no framing by definition. Anything that streams unframed data must be able
 * to ask for quiet first. */
static int muted;

void emp_session_mute(int on) { muted = on ? 1 : 0; }

void emp_session_poll(uint32_t now_ms)
{
    if (muted) return;

    /* Flush diagnostics here rather than where they are raised: this is the one place that is
     * outside every decoder AND already knows not to talk during a raw pixel stream. Flushing
     * unconditionally, even before a host has been seen, is deliberate -- the writes go nowhere
     * and the slots clear, so a host that attaches later gets what is happening NOW instead of
     * a backlog from a link it was not part of. */
    emp_diag_tick();

    if (!boot_ms) boot_ms = now_ms;

    if (!stats.host_seen) {
        /* Announce identity unprompted, once a second, until somebody answers. A host attaching
         * to an already-running device then learns what it is without asking, and a host that
         * starts first is not left waiting. */
        if ((int32_t)(now_ms - next_ready_ms) >= 0) {
            /* Fast at first, then back off. A host attaching to a running device still learns
             * what it is within a few seconds, but a HUMAN with a terminal open is not drowned
             * in binary frames for as long as the device is powered — which is what 1 Hz
             * forever actually meant in practice, and it made the console hard to read at
             * exactly the moments the console mattered most. */
            uint32_t age = now_ms - boot_ms;
            next_ready_ms = now_ms + (age < 5000u ? READY_REPEAT_MS : READY_BACKOFF_MS);
            send_ready();
        }
        return;
    }

    if ((int32_t)(now_ms - next_heartbeat_ms) >= 0) {
        next_heartbeat_ms = now_ms + HEARTBEAT_MS;
        send_heartbeat(now_ms);
    }
}

const emp_stats_t *emp_session_stats(void)
{
    stats.rx_fragments     = reasm.rx_fragments;
    stats.rx_decode_errors = reasm.rx_decode_errors;
    stats.rx_seq_gaps      = reasm.rx_seq_gaps;
    return &stats;
}
