/* USBFS device driver — polled CDC-ACM.
 *
 * Every register offset, bit position and sequence below was recovered from the stock
 * firmware's USBX device controller driver. Names are the Renesas convention; the offsets
 * and values are what the shipping firmware does, and that firmware enumerates on this board.
 *
 * All peripheral access is 16-bit. The only exception is the FIFO data ports, which are
 * accessed 16-bit or 8-bit depending on buffer alignment.
 */

#include "s7g2.h"
#include "bsp.h"
#include "usb_fs.h"
#include "usb_desc.h"

#define USB_BASE   0x40090000UL

#define SYSCFG     (USB_BASE + 0x00)
#define SYSSTS0    (USB_BASE + 0x04)
#define DVSTCTR0   (USB_BASE + 0x08)
#define CFIFO      (USB_BASE + 0x14)
#define CFIFOSEL   (USB_BASE + 0x20)
#define CFIFOCTR   (USB_BASE + 0x22)
#define INTENB0    (USB_BASE + 0x30)
#define BRDYENB    (USB_BASE + 0x36)
#define NRDYENB    (USB_BASE + 0x38)
#define BEMPENB    (USB_BASE + 0x3A)
#define INTSTS0    (USB_BASE + 0x40)
#define BRDYSTS    (USB_BASE + 0x46)
#define NRDYSTS    (USB_BASE + 0x48)
#define BEMPSTS    (USB_BASE + 0x4A)
#define USBADDR    (USB_BASE + 0x50)
#define USBREQ     (USB_BASE + 0x54)
#define USBVAL     (USB_BASE + 0x56)
#define USBINDX    (USB_BASE + 0x58)
#define USBLENG    (USB_BASE + 0x5A)
#define DCPCFG     (USB_BASE + 0x5C)
#define DCPMAXP    (USB_BASE + 0x5E)
#define DCPCTR     (USB_BASE + 0x60)
#define PIPESEL    (USB_BASE + 0x64)
#define PIPECFG    (USB_BASE + 0x68)
#define PIPEMAXP   (USB_BASE + 0x6C)
/* PIPEnCTR is at 0x6E + 2*n, NOT 0x6E + 2*(n-1): offset 0x6E itself is PIPEPERI.
 * Getting this wrong put pipe 1's control accesses onto PIPEPERI, and the PBUSY spin in
 * pipe_config() then waited forever on a bit that means nothing there — hanging the device
 * at SET_CONFIGURATION, right after enumeration succeeds and right before the host tries
 * SET_LINE_CODING. */
#define PIPEPERI   (USB_BASE + 0x6E)
#define PIPECTR(n) (USB_BASE + 0x6E + 2UL * (n))

/* SYSCFG */
#define SYSCFG_USBE   0x0001
#define SYSCFG_DMRPU  0x0008
#define SYSCFG_DPRPU  0x0010
#define SYSCFG_DCFM   0x0040
#define SYSCFG_SCKE   0x0400
/* INTSTS0 */
#define INTSTS_VBSTS  0x0080
#define INTSTS_BRDY   0x0100
#define INTSTS_BEMP   0x0400
#define INTSTS_CTRT   0x0800
#define INTSTS_DVST   0x1000
#define INTSTS_VBINT  0x8000
/* xFIFOCTR */
#define FIFOCTR_DTLN  0x0FFF
#define FIFOCTR_FRDY  0x2000
#define FIFOCTR_BCLR  0x4000
#define FIFOCTR_BVAL  0x8000
/* DCPCTR / PIPExCTR */
#define CTR_PID_NAK   0x0000
#define CTR_PID_BUF   0x0001
#define CTR_PID_STALL 0x0002
#define CTR_CCPL      0x0004
#define CTR_PBUSY     0x0020
#define CTR_SQCLR     0x0100
#define CTR_ACLRM     0x0200
#define CTR_INBUFM    0x4000

#define EP0_MPS   64
#define BULK_MPS  64
#define PIPE_OUT  1        /* EP 0x01 OUT — CDC data out */
#define PIPE_IN   2        /* EP 0x81 IN  — CDC data in  */

/* ------------------------------------------------------------------ state */

static volatile int      cfg_set;
static const uint8_t    *ep0_src;
static uint32_t          ep0_left;

/* Control-OUT data stage. SET_LINE_CODING carries 7 bytes and MUST be received before the
 * status stage is acknowledged; completing it early is exactly what made Windows fail to
 * open the port with "the semaphore timeout period has expired" — the device enumerated
 * perfectly and then refused the one request usbser.sys issues on open. */
static uint32_t          ep0_out_left;
static uint8_t           ep0_out_sink[16];

static uint8_t  rx_buf[256];
static volatile uint32_t rx_len, rx_rd;
static uint8_t  tx_buf[512];
static volatile uint32_t tx_head, tx_tail;

/* ------------------------------------------------------------------ helpers */

static inline uint16_t rd(uint32_t r)            { return REG16(r); }
static inline void     wr(uint32_t r, uint16_t v){ REG16(r) = v; }
static inline void     set(uint32_t r, uint16_t m){ REG16(r) = (uint16_t)(REG16(r) | m); }
static inline void     clr(uint32_t r, uint16_t m){ REG16(r) = (uint16_t)(REG16(r) & (uint16_t)~m); }

/* Status registers are write-0-to-clear over 10 bits. */
static inline void clrsts(uint32_t r, uint16_t bit) { REG16(r) = (uint16_t)(~bit & 0x03FF); }

/* The peripheral needs settling time after a FIFOSEL write. The stock driver spends it on
 * eight dummy 16-bit reads rather than a delay loop, which is both faster and immune to
 * being optimised into nothing. */
static void fifo_settle(void)
{
    for (int i = 0; i < 4; i++) { (void)rd(SYSCFG); (void)rd(SYSSTS0); }
}

static void cfifo_select(uint16_t isel, uint16_t pipe)
{
    wr(CFIFOSEL, (uint16_t)((rd(CFIFOSEL) & 0xF3D0) | isel | pipe));
    fifo_settle();
}

static int cfifo_wait_ready(void)
{
    for (int i = 0; i < 32; i++) {
        if (rd(CFIFOCTR) & FIFOCTR_FRDY) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ EP0 */

static void ep0_send_chunk(void)
{
    uint32_t n = ep0_left < EP0_MPS ? ep0_left : EP0_MPS;

    cfifo_select(0x0020, 0);                 /* ISEL = 1 (write), CURPIPE = DCP */
    if (!cfifo_wait_ready()) return;

    set(CFIFOSEL, 0x0400);                   /* MBW = 16-bit */
    uint32_t i = 0;
    for (; i + 1 < n; i += 2) {
        REG16(CFIFO) = (uint16_t)(ep0_src[i] | ((uint16_t)ep0_src[i + 1] << 8));
    }
    if (i < n) REG8(CFIFO) = ep0_src[i];

    if (n < EP0_MPS) wr(CFIFOCTR, FIFOCTR_BVAL);   /* commit a short/zero packet */

    ep0_src  += n;
    ep0_left -= n;

    set(BEMPENB, 1);
    set(DCPCTR, CTR_PID_BUF);
}

static void ep0_stall(void) { set(DCPCTR, CTR_PID_STALL); }

/* Arm the control endpoint to receive a data stage. The status stage is only acknowledged
 * once the data has actually arrived, in the BRDY handler. */
static void ep0_recv(uint32_t len)
{
    ep0_out_left = len;

    clr(DCPCTR, 0x0003);                 /* PID = NAK while we set up */
    clrsts(BEMPSTS, 1);
    clrsts(BRDYSTS, 1);
    wr(CFIFOCTR, FIFOCTR_BCLR);
    cfifo_select(0x0000, 0);             /* ISEL = 0 -> read direction, CURPIPE = DCP */
    set(BRDYENB, 1);
    set(DCPCTR, CTR_PID_BUF);
}

/* Drain whatever arrived on the control endpoint. We do not care about the contents —
 * line coding is meaningless for a device that is not a real UART — but the transfer must
 * be consumed and acknowledged or the host gives up on us. */
static void ep0_out_drain(void)
{
    /* Re-select the DCP. CFIFOSEL is global state and the bulk services rewrite it on
     * every poll, so whatever ep0_recv() set is long gone by the time the data arrives.
     * ep0_send_chunk() re-selects for the same reason, which is precisely why control-IN
     * transfers worked while control-OUT ones silently read from the wrong pipe. */
    cfifo_select(0x0000, 0);

    if (!cfifo_wait_ready()) return;

    uint32_t n = rd(CFIFOCTR) & FIFOCTR_DTLN;
    set(CFIFOSEL, 0x0400);               /* 16-bit */
    for (uint32_t i = 0; i < n; i += 2) {
        uint16_t w = REG16(CFIFO);
        if (i     < sizeof(ep0_out_sink)) ep0_out_sink[i]     = (uint8_t)(w & 0xFF);
        if (i + 1 < sizeof(ep0_out_sink)) ep0_out_sink[i + 1] = (uint8_t)(w >> 8);
    }
    wr(CFIFOCTR, FIFOCTR_BCLR);

    ep0_out_left = (n >= ep0_out_left) ? 0 : ep0_out_left - n;

    if (ep0_out_left == 0) {
        clr(BRDYENB, 1);
        set(DCPCTR, CTR_PID_BUF | CTR_CCPL);   /* now acknowledge the status stage */
    } else {
        set(DCPCTR, CTR_PID_BUF);              /* more to come */
    }
}

/* Reply to a control-read request, truncated to what the host asked for. */
static void ep0_reply(const uint8_t *data, uint32_t len, uint32_t requested)
{
    ep0_src  = data;
    ep0_left = len < requested ? len : requested;
    ep0_send_chunk();
}

static void pipes_init(void);

static uint8_t strbuf[64];

/* Which of our two bulk pipes an endpoint address refers to, or 0 for one we do not have. */
static unsigned pipe_of_endpoint(uint16_t ep)
{
    if ((ep & 0x7Fu) != 1u) return 0;
    return (ep & 0x80u) ? PIPE_IN : PIPE_OUT;
}

/* Enumeration diagnostics.
 *
 * A device with no bound driver has no console, so the ordinary way of finding out what a host
 * asked for is unavailable exactly when the descriptors are the thing in question. These
 * counters are the substitute: they are written here and read back through the bootloader,
 * which is still CDC and still reachable. Cheap enough to leave in permanently -- four words,
 * and `usb` prints them. */
usb_setup_stats_t g_usb_setup;

static void handle_setup(void)
{
    uint16_t req_type_req = rd(USBREQ);
    uint16_t value        = rd(USBVAL);
    uint16_t windex       = rd(USBINDX);
    uint16_t length       = rd(USBLENG);

    uint8_t bmRequestType = (uint8_t)(req_type_req & 0xFF);
    uint8_t bRequest      = (uint8_t)(req_type_req >> 8);

    g_usb_setup.total++;

    /* Standard device requests. */
    if ((bmRequestType & 0x60) == 0x00) {
        switch (bRequest) {
        case 0x06: {                                   /* GET_DESCRIPTOR */
            uint8_t type = (uint8_t)(value >> 8);
            uint8_t idx  = (uint8_t)(value & 0xFF);
            uint32_t n = 0;
            const uint8_t *d;

            if (type == 0x01) { d = usb_desc_device(&n); ep0_reply(d, n, length); return; }
            if (type == 0x02) { d = usb_desc_config(&n); ep0_reply(d, n, length); return; }
            if (type == 0x0F) {                        /* BOS, which is how MS OS 2.0 starts */
                g_usb_setup.bos++;
                d = usb_desc_bos(&n);
                if (d) {
                    g_usb_setup.bos_len = (uint8_t)(n < length ? n : length);
                    ep0_reply(d, n, length);
                    return;
                }
            }
            if (type == 0x03) {
                n = usb_desc_string(idx, strbuf);
                if (n) { ep0_reply(strbuf, n, length); return; }
            }
            ep0_stall();
            return;
        }
        case 0x05:                                     /* SET_ADDRESS — hardware does it */
            set(DCPCTR, CTR_PID_BUF | CTR_CCPL);
            return;
        case 0x09:                                     /* SET_CONFIGURATION */
            cfg_set = (value != 0);
            /* Configure the bulk pipes HERE rather than relying on the DVST/DVSQ=Configured
             * transition. DVST is edge-reported and a polled driver can miss it between
             * calls; SET_CONFIGURATION is the request that actually defines when the pipes
             * must exist, so doing it here is both earlier and not dependent on catching an
             * edge. Without this the endpoints stay unconfigured and every OUT token is
             * NAKed, which the host reports only as a write timeout. */
            if (cfg_set) pipes_init();
            set(DCPCTR, CTR_PID_BUF | CTR_CCPL);
            return;
        case 0x08: {                                   /* GET_CONFIGURATION */
            static uint8_t c; c = (uint8_t)(cfg_set ? 1 : 0);
            ep0_reply(&c, 1, length);
            return;
        }
        case 0x0B:                                     /* SET_INTERFACE */
            /* One interface with one setting, so the only legal request is "setting 0", and
             * anything else is the host asking for something we do not have. A raw host stack
             * issues this where usbser.sys never did; it used to fall through to a stall, which
             * WinUSB reports as a failure to open the device. */
            if (value == 0) { set(DCPCTR, CTR_PID_BUF | CTR_CCPL); return; }
            ep0_stall();
            return;

        case 0x0A: {                                   /* GET_INTERFACE */
            static const uint8_t alt = 0;
            ep0_reply(&alt, 1, length);
            return;
        }

        case 0x01:                                     /* CLEAR_FEATURE */
        case 0x03: {                                   /* SET_FEATURE */
            /* ENDPOINT_HALT, recipient endpoint. This is how a WinUSB application recovers a
             * stalled pipe, and without it the only recovery is to unplug the device. Clearing
             * the halt must also reset the data toggle, or the first packet after the recovery
             * is silently discarded by the host as a duplicate -- a failure that looks like the
             * device ignoring one message and then working perfectly. */
            unsigned pipe = ((bmRequestType & 0x1Fu) == 0x02u && value == 0x0000u)
                          ? pipe_of_endpoint(windex) : 0;
            if (!pipe) { ep0_stall(); return; }

            if (bRequest == 0x01) {
                clr(PIPECTR(pipe), 0x0003);
                set(PIPECTR(pipe), CTR_SQCLR);
                set(PIPECTR(pipe), CTR_PID_BUF);
            } else {
                set(PIPECTR(pipe), CTR_PID_STALL);
            }
            set(DCPCTR, CTR_PID_BUF | CTR_CCPL);
            return;
        }

        case 0x00: {                                   /* GET_STATUS */
            static uint8_t st[2];
            st[1] = 0;
            if ((bmRequestType & 0x1Fu) == 0x02u) {
                unsigned pipe = pipe_of_endpoint(windex);
                st[0] = (uint8_t)((pipe && (rd(PIPECTR(pipe)) & 0x0003u) == CTR_PID_STALL) ? 1 : 0);
            } else {
                st[0] = 0;
            }
            ep0_reply(st, 2, length);
            return;
        }
        default:
            break;
        }
    }

    /* Whatever the linked descriptor set understands: CDC line coding in the bootloader, the
     * MS OS 2.0 fetch in the application. Neither image knows about the other's requests. */
    {
        const uint8_t *data = 0;
        uint32_t dlen = 0;
        switch (usb_desc_class_request(bmRequestType, bRequest, value, windex, length,
                                       &data, &dlen)) {
        case USB_REQ_DATA_IN:
            g_usb_setup.vendor++;
            g_usb_setup.vendor_asked = length;
            ep0_reply(data, dlen, length);
            return;
        case USB_REQ_ACK:
            set(DCPCTR, CTR_PID_BUF | CTR_CCPL);
            return;
        case USB_REQ_DATA_OUT:
            ep0_recv(dlen);                            /* status stage waits for the data */
            return;
        default:
            break;
        }
    }

    /* Whatever we did not understand, kept for the report. The LAST one is the useful one: a
     * host that gives up does so on the request it could not get an answer to. */
    g_usb_setup.stalls++;
    g_usb_setup.last_stall_req  = bRequest;
    g_usb_setup.last_stall_type = bmRequestType;

    ep0_stall();
}

/* ------------------------------------------------------------------ bulk */

static void pipe_config(unsigned pipe, uint16_t cfg)
{
    clr(PIPECTR(pipe), 0x0003);

    /* Bounded. An unbounded spin here is how a wrong register offset turns into a device
     * that enumerates and then goes silent forever, with no way to tell the two apart from
     * the host. If PBUSY never clears we would rather configure the pipe anyway and have
     * the transfer fail visibly than hang the whole bootloader. */
    for (uint32_t t = 0; t < 100000UL; t++) {
        if (!(rd(PIPECTR(pipe)) & CTR_PBUSY)) break;
    }

    wr(PIPESEL, (uint16_t)pipe);
    wr(PIPECFG, cfg);
    wr(PIPEMAXP, BULK_MPS);
    wr(PIPESEL, 0);

    set(PIPECTR(pipe), CTR_SQCLR);
    set(PIPECTR(pipe), CTR_PID_BUF);
}

static void pipes_init(void)
{
    /* 0x4082 = TYPE bulk | SHTNAK | DIR=OUT | EPNUM 1
     * 0x4091 = TYPE bulk | SHTNAK | DIR=IN  | EPNUM 1 */
    pipe_config(PIPE_OUT, 0x4081);
    pipe_config(PIPE_IN,  0x4091);
}

static void bulk_rx_service(void)
{
    if (rx_len > rx_rd) {
        /* Still holding an unconsumed packet, so we cannot take another — but keep the
         * pipe armed regardless. Letting it sit in NAK while we are merely busy is how a
         * transient backlog turns into a permanently dead endpoint. */
        set(PIPECTR(PIPE_OUT), CTR_PID_BUF);
        return;
    }

    wr(CFIFOSEL, (uint16_t)((rd(CFIFOSEL) & 0xF3D0) | PIPE_OUT));
    fifo_settle();
    if (!(rd(CFIFOCTR) & FIFOCTR_FRDY)) return;

    uint32_t n = rd(CFIFOCTR) & FIFOCTR_DTLN;
    if (n > sizeof(rx_buf)) n = sizeof(rx_buf);

    set(CFIFOSEL, 0x0400);                       /* 16-bit */
    uint32_t i = 0;
    for (; i + 1 < n; i += 2) {
        uint16_t w = REG16(CFIFO);
        rx_buf[i]     = (uint8_t)(w & 0xFF);
        rx_buf[i + 1] = (uint8_t)(w >> 8);
    }
    if (i < n) rx_buf[i] = REG8(CFIFO);

    wr(CFIFOCTR, FIFOCTR_BCLR);
    rx_len = n;
    rx_rd  = 0;
    g_usb_setup.bulk_rx++;

    /* Re-arm. PIPECFG has SHTNAK set, which makes the hardware drop the pipe to NAK as soon
     * as it receives a short packet — and every console command is a short packet. Without
     * this the endpoint accepts exactly one transfer and then NAKs forever, which the host
     * reports only as a write timeout on the SECOND command. That is a nasty shape: it looks
     * like an intermittent fault rather than a missing re-arm. */
    set(PIPECTR(PIPE_OUT), CTR_PID_BUF);
}

static void bulk_tx_service(void)
{
    if (tx_head == tx_tail) return;
    if (rd(PIPECTR(PIPE_IN)) & CTR_INBUFM) return;   /* previous packet still going out */

    /* ISEL is only meaningful for the DCP: for every other pipe the direction comes from
     * PIPECFG.DIR, and setting ISEL here selects nothing useful. Mask 0xF3F0 (not 0xF3D0)
     * so ISEL is left alone rather than being asserted on a bulk pipe. */
    wr(CFIFOSEL, (uint16_t)((rd(CFIFOSEL) & 0xF3F0) | PIPE_IN));
    fifo_settle();
    if (!(rd(CFIFOCTR) & FIFOCTR_FRDY)) return;

    uint32_t avail = tx_tail - tx_head;
    uint32_t n = avail < BULK_MPS ? avail : BULK_MPS;

    set(CFIFOSEL, 0x0400);
    uint32_t i = 0;
    for (; i + 1 < n; i += 2) {
        uint32_t k = (tx_head + i) % sizeof(tx_buf);
        uint32_t k2 = (tx_head + i + 1) % sizeof(tx_buf);
        REG16(CFIFO) = (uint16_t)(tx_buf[k] | ((uint16_t)tx_buf[k2] << 8));
    }
    if (i < n) REG8(CFIFO) = tx_buf[(tx_head + i) % sizeof(tx_buf)];

    if (n < BULK_MPS) wr(CFIFOCTR, FIFOCTR_BVAL);

    tx_head += n;
    if (tx_head == tx_tail) { tx_head = 0; tx_tail = 0; }
}

/* ------------------------------------------------------------------ public */

void usb_init(void)
{
    REG32(MSTPCRB) = REG32(MSTPCRB) & ~MSTPB_USBFS;

    set(SYSCFG, SYSCFG_SCKE);
    /* Bounded, like every other wait in this file. A USB clock that never comes up should
     * leave the bootloader running with dead USB, not hang it — the GPIO blink is then the
     * only diagnostic left and it must still get a chance to run. */
    for (uint32_t t = 0; t < 1000000UL; t++) {
        if ((rd(SYSCFG) & SYSCFG_SCKE) == SYSCFG_SCKE) break;
    }

    clr(SYSCFG, SYSCFG_USBE);
    clr(SYSCFG, SYSCFG_DCFM);            /* device mode — the only bit that selects it */

    /* VBUS debounce, as stock: three reads, require two consecutive agreements — but bounded.
     * Stock's version is two nested unbounded loops, and every other wait in this file was
     * bounded precisely because an unbounded one is indistinguishable from a dead device.
     * If VBSTS never settles we attach anyway; a wrong guess costs an enumeration, a spin
     * costs the whole image. */
    uint16_t a = 0, b = 0, c = 0;
    for (uint32_t t = 0; t < 100000UL; t++) {
        a = (uint16_t)(rd(INTSTS0) & INTSTS_VBSTS);
        b = (uint16_t)(rd(INTSTS0) & INTSTS_VBSTS);
        c = (uint16_t)(rd(INTSTS0) & INTSTS_VBSTS);
        if (a == b && a == c) break;
    }

    if (a) set(SYSCFG, SYSCFG_DPRPU);    /* VBUS present: attach now */
    clr(SYSCFG, SYSCFG_DMRPU);

    set(SYSCFG, SYSCFG_USBE);

    wr(DCPMAXP, EP0_MPS);

    /* Polled: we never enable INTENB0. Status bits still set in INTSTS0 and we read them
     * directly, which is the whole point of not using interrupts here. */
    wr(INTENB0, 0);
    wr(BRDYENB, 0);
    wr(NRDYENB, 0);
    wr(BEMPENB, 0);

    cfg_set = 0;
    rx_len = rx_rd = 0;
    tx_head = tx_tail = 0;
}

/* Drop off the bus and stay off long enough for the host to notice.
 *
 * Handing control from the bootloader to the application resets the USB peripheral, but the
 * D+ pull-up stays asserted throughout, so the host never sees a disconnect: it keeps the
 * old device node, the old address and the old configuration, while the device has forgotten
 * all of it. The port then exists but answers nothing.
 *
 * Clearing DPRPU is an electrical detach. 120 ms is comfortably longer than the ~2.5 ms a
 * host needs to register it, and costs nothing on a path that only runs once per launch. */
void usb_detach(void)
{
    clr(SYSCFG, SYSCFG_DPRPU);
    clr(SYSCFG, SYSCFG_USBE);
    cfg_set = 0;
    bsp_delay_ms(120);
}

void usb_poll(void)
{
    uint16_t s = rd(INTSTS0);

    if (s & INTSTS_VBINT) {
        wr(INTSTS0, (uint16_t)0x7FFF);
        if (rd(INTSTS0) & INTSTS_VBSTS) set(SYSCFG, SYSCFG_DPRPU);
        else { clr(SYSCFG, SYSCFG_DPRPU); cfg_set = 0; }
    }

    if (s & INTSTS_DVST) {
        wr(INTSTS0, (uint16_t)0xEFFF);
        uint16_t dvsq = (uint16_t)(s & 0x0070);
        if (dvsq == 0x0010) {                 /* Default — bus reset completed */
            cfg_set = 0;
            wr(DCPMAXP, EP0_MPS);
        } else if (dvsq == 0x0030) {          /* Configured */
            pipes_init();
        }
    }

    if (s & INTSTS_CTRT) {
        uint16_t ctsq = (uint16_t)(s & 0x0007);
        if (ctsq == 1 || ctsq == 3 || ctsq == 5) {
            wr(INTSTS0, (uint16_t)0xFFF7);    /* clear VALID before reading the packet */
            handle_setup();
        }
        wr(INTSTS0, (uint16_t)0xF7FF);        /* clear CTRT after processing */
    }

    /* Control-OUT data arriving (SET_LINE_CODING and friends). Checked before BEMP so a
     * transfer that sets both is drained rather than being mistaken for a finished IN. */
    if (s & INTSTS_BRDY) {
        if (rd(BRDYSTS) & 1) {
            clrsts(BRDYSTS, 1);
            if (ep0_out_left) ep0_out_drain();
        }
    }

    if (s & INTSTS_BEMP) {
        if (rd(BEMPSTS) & 1) {
            clrsts(BEMPSTS, 1);
            if (ep0_left) {
                ep0_send_chunk();
            } else {
                clr(BEMPENB, 1);
                set(DCPCTR, CTR_PID_BUF | CTR_CCPL);   /* status stage */
            }
        }
    }

    /* Bulk services rewrite CFIFOSEL, so they must not run while a control transfer is
     * mid-flight — otherwise they move the FIFO window out from under it. The re-select in
     * each EP0 path already makes this safe, but not running them at all during a control
     * transfer removes a whole class of ordering bug rather than papering over it. */
    if (cfg_set && ep0_left == 0 && ep0_out_left == 0) {
        bulk_rx_service();
        bulk_tx_service();
    }
}

int usb_configured(void) { return cfg_set; }

uint32_t usb_read(uint8_t *buf, uint32_t max)
{
    uint32_t avail = rx_len - rx_rd;
    if (!avail) return 0;
    uint32_t n = avail < max ? avail : max;
    for (uint32_t i = 0; i < n; i++) buf[i] = rx_buf[rx_rd + i];
    rx_rd += n;
    if (rx_rd >= rx_len) { rx_len = 0; rx_rd = 0; }
    return n;
}

uint32_t usb_write(const uint8_t *buf, uint32_t len)
{
    uint32_t space = sizeof(tx_buf) - tx_tail;
    uint32_t n = len < space ? len : space;
    for (uint32_t i = 0; i < n; i++) tx_buf[tx_tail + i] = buf[i];
    tx_tail += n;
    return n;
}

/* Bytes the console gave up on. Reported by `id`, because silent truncation is worse than
 * loss: a diagnostic that quietly drops half its output makes the DEVICE look broken. */
uint32_t g_usb_tx_dropped;

/* Pace against the transmit buffer instead of overrunning it.
 *
 * usb_write() only ever accepts what currently fits, and the original console path ignored the
 * shortfall — so any command that printed more than one buffer's worth silently lost the rest.
 * A long register dump came back truncated mid-line, which reads as the device dying partway
 * through rather than as a console limitation.
 *
 * Still bounded, and still drops in the end: an unread console must never wedge the device.
 * That property is why this cannot simply spin until the host catches up. */
static void usb_write_all(const uint8_t *buf, uint32_t len)
{
    uint32_t sent = 0;
    for (uint32_t attempt = 0; attempt < 20000U && sent < len; attempt++) {
        sent += usb_write(buf + sent, len - sent);
        if (sent < len) usb_poll();
    }
    if (sent < len) g_usb_tx_dropped += (len - sent);
}

/* Raw block write for throughput measurement — no string handling, same pacing. */
void usb_write_block(const uint8_t *buf, uint32_t len) { usb_write_all(buf, len); }

void usb_puts(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    usb_write_all((const uint8_t *)s, n);
    for (int i = 0; i < 4; i++) usb_poll();
}

void usb_printf_u32(const char *label, uint32_t v)
{
    static const char hexd[] = "0123456789ABCDEF";
    char line[48];
    uint32_t i = 0;
    while (label[i] && i < 32) { line[i] = label[i]; i++; }
    line[i++] = ' '; line[i++] = '0'; line[i++] = 'x';
    for (int b = 7; b >= 0; b--) line[i++] = hexd[(v >> (b * 4)) & 0xF];
    line[i++] = '\r'; line[i++] = '\n';
    usb_write_all((const uint8_t *)line, i);
    for (int k = 0; k < 4; k++) usb_poll();
}
