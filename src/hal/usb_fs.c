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

/* ------------------------------------------------------------------ descriptors */

static const uint8_t dev_desc[18] = {
    18, 0x01, 0x00, 0x02,            /* bcdUSB 2.00 (still full speed; 2.00 is fine and
                                        avoids Windows treating us as a 1.1 oddity) */
    0x02, 0x00, 0x00, EP0_MPS,       /* class 0x02 = CDC at device level */
    0xC9, 0x1F,                      /* idVendor  0x1FC9 (NXP, as the hardware ships) */
    0xCE, 0x82,                      /* idProduct 0x82CE — deliberately NOT the stock 0x82CF,
                                        so this never binds against the MIDI driver */
    0x01, 0x00,                      /* bcdDevice 0.01 */
    1, 2, 3,                         /* iManufacturer, iProduct, iSerial */
    1                                /* bNumConfigurations */
};

#define CFG_TOTAL 67
static const uint8_t cfg_desc[CFG_TOTAL] = {
    /* configuration */
    9, 0x02, CFG_TOTAL, 0x00, 2, 1, 0, 0x80, 250,      /* bus powered, 500 mA */

    /* interface 0: CDC communications */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01,                          /* CDC header */
    4, 0x24, 0x02, 0x02,                                /* ACM, supports Set_Line_Coding */
    5, 0x24, 0x06, 0, 1,                                /* union: master 0, slave 1 */
    5, 0x24, 0x01, 0x00, 1,                             /* call management */
    7, 0x05, 0x82, 0x03, 8, 0x00, 16,                   /* EP 0x82 interrupt IN */

    /* interface 1: CDC data */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 0x05, 0x01, 0x02, BULK_MPS, 0x00, 0,             /* EP 0x01 bulk OUT */
    7, 0x05, 0x81, 0x02, BULK_MPS, 0x00, 0,             /* EP 0x81 bulk IN  */
};

static const uint8_t str0[] = { 4, 0x03, 0x09, 0x04 };  /* en-US */

/* ASCII expanded to UTF-16LE on the fly, so the tables stay readable. */
static const char *const strings[] = { 0, "Kimchi and Chips", "Electra Mini FW", "EMB-0001" };

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

static uint32_t build_string(unsigned idx)
{
    if (idx == 0) {
        for (unsigned i = 0; i < sizeof(str0); i++) strbuf[i] = str0[i];
        return sizeof(str0);
    }
    if (idx >= sizeof(strings) / sizeof(strings[0]) || !strings[idx]) return 0;

    const char *s = strings[idx];
    uint32_t n = 0;
    while (s[n]) n++;
    if (n > 30) n = 30;

    strbuf[0] = (uint8_t)(2 + 2 * n);
    strbuf[1] = 0x03;
    for (uint32_t i = 0; i < n; i++) {
        strbuf[2 + 2 * i]     = (uint8_t)s[i];
        strbuf[2 + 2 * i + 1] = 0;
    }
    return strbuf[0];
}

static void handle_setup(void)
{
    uint16_t req_type_req = rd(USBREQ);
    uint16_t value        = rd(USBVAL);
    uint16_t length       = rd(USBLENG);

    uint8_t bmRequestType = (uint8_t)(req_type_req & 0xFF);
    uint8_t bRequest      = (uint8_t)(req_type_req >> 8);

    /* Standard device requests. */
    if ((bmRequestType & 0x60) == 0x00) {
        switch (bRequest) {
        case 0x06: {                                   /* GET_DESCRIPTOR */
            uint8_t type = (uint8_t)(value >> 8);
            uint8_t idx  = (uint8_t)(value & 0xFF);
            if (type == 0x01) { ep0_reply(dev_desc, sizeof(dev_desc), length); return; }
            if (type == 0x02) { ep0_reply(cfg_desc, sizeof(cfg_desc), length); return; }
            if (type == 0x03) {
                uint32_t n = build_string(idx);
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
        case 0x00: {                                   /* GET_STATUS */
            static const uint8_t st[2] = { 0, 0 };
            ep0_reply(st, 2, length);
            return;
        }
        default:
            break;
        }
    }

    /* CDC class requests. We do not implement a real UART, so line coding is accepted and
     * discarded — but it MUST be answered, or Windows fails to open the port. */
    if ((bmRequestType & 0x60) == 0x20) {
        switch (bRequest) {
        case 0x20:                                     /* SET_LINE_CODING — 7 data bytes */
            if (length) { ep0_recv(length); }          /* status stage waits for the data */
            else        { set(DCPCTR, CTR_PID_BUF | CTR_CCPL); }
            return;
        case 0x22:                                     /* SET_CONTROL_LINE_STATE — no data */
        case 0x23:                                     /* SEND_BREAK — no data */
            set(DCPCTR, CTR_PID_BUF | CTR_CCPL);
            return;
        case 0x21: {                                   /* GET_LINE_CODING */
            static const uint8_t lc[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };  /* 115200 8N1 */
            ep0_reply(lc, sizeof(lc), length);
            return;
        }
        default:
            break;
        }
    }

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
