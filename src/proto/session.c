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
#include "txq.h"

/* What this device can RECEIVE in one fragment. Bulk endpoints, so the bulk MTU; both are
 * 64-byte endpoints on this full-speed port, and the MTU is the payload we are willing to put
 * in one fragment, not the packet size. rx_frag is sized from this, so it is also a hard limit
 * rather than a preference, and it is what READY advertises. */
#define SESSION_MTU        EMP_MTU_BULK

/* The floor for a negotiated transmit MTU. A host that asks for less than one HID report's
 * worth is either confused or broken, and honouring it would fragment every message into
 * uselessness -- so the floor is a refusal to co-operate with nonsense, not a preference. */
#define SESSION_MIN_MTU    EMP_MTU_HID

/* How long a reassembly may go without gaining a byte before it is abandoned. Generous on
 * purpose: an 8 KB message at the measured throughput takes about 30 ms, so two seconds is
 * never reached by a transfer that is merely slow, and is reached immediately by one whose
 * final fragment was lost. */
#define REASM_IDLE_MS      2000u

#define HEARTBEAT_MS       1000u

/* Queued bytes above which unsolicited traffic is skipped rather than added to the pile. Well
 * under the driver's buffer, so that an answer always has somewhere to go. */
#define HEARTBEAT_BACKPRESSURE 192u
#define READY_REPEAT_MS    1000u
#define READY_BACKOFF_MS   5000u

/* Identity. Kept short: every byte is on the wire in READY. */
#define FW_VERSION         0x00000100u        /* 0.1.0 */

/* A build number, in the fixed prefix so a host can compare two devices without parsing a
 * string. Zero until something generates one: the honest value for "this build is not
 * numbered" is not a fabricated number, and BUILD_ID below carries the real information in a
 * form a human can read. */
#define FW_BUILD           0u
static const char MODEL[]    = "Electra One Mini";
static const char SERIAL[]   = "EMB-0001";
static const char BUILD_ID[] = __DATE__ " " __TIME__;

static emp_stats_t   stats;
static emp_reasm_t   reasm;
static uint32_t      next_heartbeat_ms;
static uint32_t      next_ready_ms;
static uint32_t      boot_ms;

/* Negotiated in HELLO, which is the whole point of HELLO. docs/protocol.md 0 calls the MTU
 * "negotiated, never assumed", and it was assumed: this used to be the SESSION_MTU constant at
 * every use, so a host with a smaller window was simply talked over. */
static uint16_t      tx_mtu = SESSION_MTU;
static uint8_t       peer_minor;           /* min(host, device), per the minor-version rule */

/* The last time the service loop reported. emp_session_feed() is reached through a
 * function-pointer sink whose signature carries no clock, and PONG has to answer with a device
 * timestamp -- it used to answer with a literal zero, which made every round-trip measurement
 * on the host meaningless. One service period of staleness is far better than that. */
static uint32_t      last_now_ms;

/* Worst repaint seen, in microseconds, reported in HEARTBEAT. Set from the render loop rather
 * than measured here: this file has no idea when a frame starts. */
static uint32_t      render_us_max;

/* Progress watch for the reassembly timeout above. */
static uint32_t      reasm_last_len;
static uint32_t      reasm_idle_ms;

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

/* ---------------------------------------------------------------- transmit
 *
 * The queueing and coalescing rules live in txq.c, which knows nothing about USB or framing --
 * see the header there for why each queue exists. This is the part that cannot be tested off
 * the device: turning a queued message into fragments and pushing them at the endpoint.
 */

static txq_t txq;

/* A message part-way out of the door. usb_write() takes what fits and no more, so a fragment
 * stream has to survive being handed over in pieces -- which it does, because the receiving
 * side is byte-oriented and payload_len is authoritative. Framing happens once; writing may
 * take several visits, and neither ever blocks. */
static uint32_t tx_pending_len, tx_pending_sent;

static int tx_flush_pending(void)
{
    while (tx_pending_sent < tx_pending_len) {
        uint32_t n = usb_write(tx_buf + tx_pending_sent, tx_pending_len - tx_pending_sent);
        if (!n) return 0;                       /* full; try again next visit, never spin */
        tx_pending_sent += n;
    }
    if (tx_pending_len) {
        tx_pending_len = tx_pending_sent = 0;
        stats.tx_fragments++;
    }
    return 1;
}

static int tx_emit(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len,
                   void *ctx)
{
    (void)ctx;

    /* Anything already framed goes first, or fragments would arrive interleaved with the
     * message that displaced them -- which rule F4 forbids for exactly this reason. */
    if (!tx_flush_pending()) return 0;

    emp_writer_t w;
    emp_writer_init(&w, tx_buf, sizeof(tx_buf), tx_mtu);
    w.seq = tx_seq;

    if (emp_write_message(&w, channel, opcode, payload, len) != EMP_OK) {
        /* Unframeable, so it will never become frameable. Dropping it is the only alternative
         * to blocking the queue behind it forever. */
        stats.tx_dropped++;
        return 1;
    }
    tx_seq = w.seq;

    tx_pending_len  = w.used;
    tx_pending_sent = 0;
    stats.tx_messages++;

    /* Always accepted once framed, whether or not it all fits right now: the queue has handed
     * this message over, and what is left of it drains from tx_pending on the next visit. */
    (void)tx_flush_pending();
    return 1;
}

static void tx_drop(uint8_t channel, uint8_t opcode, const char *why, void *ctx)
{
    (void)channel;
    (void)ctx;
    stats.tx_dropped++;
    emp_diag(EMP_SEV_WARN, EMP_DIAG_TX_DROPPED, opcode, why);
}

/* Exposed so the surface layer can send without owning framing or the endpoint. Policy lives
 * in surface.c, transport lives here, and neither reaches into the other. */
void emp_send(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    txq_push(&txq, channel, opcode, payload, len);
}

static void send_message(uint8_t channel, uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    emp_send(channel, opcode, payload, len);
}

static void send_ready(void)
{
    uint8_t buf[256];
    enc_t e;
    enc_init(&e, buf, sizeof(buf));

    /* The FIXED 44-BYTE PREFIX. docs/protocol.md 6 promises these bytes stable across major
     * versions -- it is the one back-compatibility guarantee in the protocol, and it is what
     * lets a host that cannot talk to this device still report WHICH device it cannot talk to.
     * It was 32 bytes and missing max_descriptor_bytes entirely, which shifted every field
     * after it against a decoder written from the document. */
    put8(&e, EMP_VERSION_MAJOR);               /*  0 */
    put8(&e, peer_minor);                      /*  1  negotiated, so the host sees what we agreed */
    put16(&e, SESSION_MTU);                    /*  2  what we can RECEIVE, not what we send with */
    put32(&e, stats.session_id);               /*  4 */
    put32(&e, 0);                              /*  8  capability flags; none yet */
    put32(&e, EMP_MAX_MESSAGE_RX);             /* 12 */
    put32(&e, EMP_MAX_DESCRIPTOR_BYTES);       /* 16 */
    put16(&e, SURF_MAX_FIELDS);                /* 20 */
    put16(&e, SURF_POTS);                      /* 22  eight knobs, so eight fields at once */
    put64(&e, surf_applied_revision());        /* 24 */
    put32(&e, FW_VERSION);                     /* 32 */
    put32(&e, FW_BUILD);                       /* 36 */
    put32(&e, 0);                              /* 40  reserved, zero: room for one more
                                                *     capability word without spending the
                                                *     stability promise to get it */
    /* 44 bytes to here. Everything below is variable-length and NOT covered by the promise. */
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
    put32(&e, render_us_max);

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

/* HELLO: version u8+u8, host_mtu u16, host_epoch_ms u64, rx_window u32, host_id String.
 *
 * This was not parsed at all -- the device answered READY and threw the message away, which
 * made "negotiated, never assumed" a claim about a negotiation that did not happen.
 *
 * Nothing here can fail the link. A version we do not like still gets a READY, because READY
 * carries the build_id the host needs in order to say WHICH firmware it cannot talk to; that
 * decision is the host's to make and it cannot make it from silence. */
static void handle_hello(const uint8_t *msg, uint32_t len)
{
    if (len < 2) return;

    uint8_t major = msg[0];
    uint8_t minor = msg[1];

    if (major != EMP_VERSION_MAJOR) {
        emp_diag(EMP_SEV_ERROR, EMP_DIAG_VERSION_MISMATCH, major,
                 "major version differs; this link cannot be trusted");
        /* Deliberately still answers, below. */
    }

    /* Minor is min(host, device): a minor version may only ADD things, so agreeing to the lower
     * of the two means neither side sends the other something it has never heard of. */
    peer_minor = EMP_VERSION_MINOR;
    if (minor < peer_minor) peer_minor = minor;

    if (len >= 4) {
        uint16_t host_mtu = (uint16_t)(msg[2] | (msg[3] << 8));
        uint16_t want = host_mtu;

        if (want > SESSION_MTU)     want = SESSION_MTU;
        if (want < SESSION_MIN_MTU) want = SESSION_MIN_MTU;

        if (host_mtu && host_mtu < SESSION_MIN_MTU) {
            emp_diag(EMP_SEV_WARN, EMP_DIAG_MTU_REFUSED, host_mtu,
                     "host asked for an MTU below the floor");
        }
        if (host_mtu) tx_mtu = want;
    }

    /* host_epoch_ms and rx_window are read but not yet acted on: the first is for the host's own
     * correlation, and the second belongs to the credit scheme, which is deliberately deferred
     * until HEARTBEAT.render_us_max gives us something real to size it against. */
}

/* ------------------------------------------------------------------ dispatch */

static void handle_control(const emp_header_t *h, const uint8_t *msg, uint32_t len,
                           uint32_t now_ms)
{
    switch (h->opcode) {
    case EMP_OP_HELLO:
        stats.hellos++;
        stats.host_seen = 1;
        handle_hello(msg, len);
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

    case EMP_OP_DIAG:
        /* The host is allowed to send these too. Nothing to do with one, but saying so beats
         * having it counted as an unknown opcode. */
        break;

    default:
        emp_diag(EMP_SEV_INFO, EMP_DIAG_UNKNOWN_OPCODE,
                 (uint32_t)(EMP_CH_CONTROL << 8) | h->opcode, "unknown control opcode");
        break;
    }
}

/* Dispatch on CHANNEL FIRST, then opcode.
 *
 * It used to be the other way round, with the channel consulted only in the default arm. So a
 * SURFACE message that happened to carry opcode 0x01 was answered with a full READY, and every
 * surface opcode that collided with a control one was routed to the wrong handler entirely.
 * The channel is what says which namespace the opcode is drawn from; reading the opcode first
 * is reading a word before knowing its language. */
static void handle(const emp_header_t *h, const uint8_t *msg, uint32_t len, uint32_t now_ms)
{
    stats.rx_messages++;

    switch (h->channel) {
    case EMP_CH_CONTROL:
        handle_control(h, msg, len, now_ms);
        break;

    case EMP_CH_SURFACE:
        stats.host_seen = 1;
        surf_handle(h->opcode, msg, len);
        break;

    case EMP_CH_INPUT:
        /* Device to host only. A host sending one is confused about which end it is, and that
         * is worth saying rather than ignoring -- it is the kind of mistake that otherwise
         * presents as "the device ignores my messages". */
        emp_diag(EMP_SEV_WARN, EMP_DIAG_UNKNOWN_OPCODE,
                 (uint32_t)(EMP_CH_INPUT << 8) | h->opcode,
                 "INPUT is device to host; nothing to receive here");
        break;

    default:
        emp_diag(EMP_SEV_INFO, EMP_DIAG_UNKNOWN_CHANNEL, h->channel, "unknown channel");
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
        if (rc == 1) handle(&h, msg, msg_len, last_now_ms);
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
    tx_mtu = SESSION_MTU;
    peer_minor = EMP_VERSION_MINOR;
    render_us_max = 0;
    reasm_last_len = 0;
    reasm_idle_ms = 0;
    txq_init(&txq, tx_emit, tx_drop, 0);
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

void emp_session_note_render_us(uint32_t us)
{
    if (us > render_us_max) render_us_max = us;
}

uint16_t emp_session_tx_mtu(void) { return tx_mtu; }

void emp_session_poll(uint32_t now_ms)
{
    if (muted) return;

    last_now_ms = now_ms;

    /* Nothing queued survives losing the host. Input events describe a panel state that will
     * have moved on by the time anyone reconnects, and delivering them late is worse than not
     * delivering them: a knob position from before the cable was pulled would arrive as a fresh
     * edit and overwrite whatever the host has since decided. Diagnostics go the same way, for
     * the same reason -- they are about a link that no longer exists. */
    if (!usb_configured()) {
        txq_clear(&txq);
        emp_diag_drop_pending();
        return;
    }

    /* A reassembly that has stopped arriving must not wedge the receiver.
     *
     * Judged by PROGRESS, not by age: a large descriptor at a small MTU legitimately takes a
     * while, and a deadline started when the message began would kill exactly the transfers
     * that need the time. So the test is "no new bytes for REASM_IDLE_MS", which a transfer
     * that is merely slow never trips and a transfer whose LAST fragment was lost always does.
     *
     * Before this, a single lost LAST left in_progress set forever: every subsequent FIRST was
     * then an F2 violation and every continuation belonged to a message whose sender had long
     * since given up -- one lost fragment cost the link permanently. */
    if (reasm.in_progress) {
        if (reasm.len != reasm_last_len) {
            reasm_last_len = reasm.len;
            reasm_idle_ms  = now_ms;
        } else if ((uint32_t)(now_ms - reasm_idle_ms) >= REASM_IDLE_MS) {
            emp_reasm_abort(&reasm);
            emp_diag(EMP_SEV_WARN, EMP_DIAG_REASSEMBLY_TIMEOUT, reasm_last_len,
                     "reassembly abandoned: no fragments arriving");
            reasm_last_len = 0;
        }
    } else {
        reasm_last_len = 0;
        reasm_idle_ms  = now_ms;
    }

    /* Flush diagnostics here rather than where they are raised: this is the one place that is
     * outside every decoder AND already knows not to talk during a raw pixel stream. */
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
    } else if ((int32_t)(now_ms - next_heartbeat_ms) >= 0) {
        next_heartbeat_ms = now_ms + HEARTBEAT_MS;

        /* Not if the host has stopped collecting. Unsolicited traffic exists so that silence is
         * informative; queueing it for a host that is not reading achieves the opposite, because
         * the buffer fills with stale state and the next thing written -- an answer somebody IS
         * waiting for -- is what gets dropped instead.
         *
         * This never mattered on CDC, where usbser.sys drains the endpoint continuously whether
         * an application is listening or not. Under WinUSB nothing reads unless asked, and the
         * symptom was a console that returned nothing at all until the pipe had been drained by
         * hand. A dropped heartbeat costs nothing: the next one carries the same state, and it
         * is counted. */
        if (usb_tx_pending() < HEARTBEAT_BACKPRESSURE) send_heartbeat(now_ms);
        else stats.tx_dropped++;
    }

    /* Last, so everything raised above goes out on this visit rather than the next one. This is
     * the ONLY place that writes to the endpoint on the protocol's behalf. */
    (void)txq_pump(&txq);
}

const emp_stats_t *emp_session_stats(void)
{
    stats.rx_fragments     = reasm.rx_fragments;
    stats.rx_decode_errors = reasm.rx_decode_errors;
    stats.rx_seq_gaps      = reasm.rx_seq_gaps;
    return &stats;
}
