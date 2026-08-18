/* CDC-ACM descriptors. Linked into the BOOTLOADER only.
 *
 * The application has moved to a vendor-specific interface (usb_desc_vendor.c). This one stays
 * exactly as it was, deliberately: the bootloader is reachable only over USB and replaceable
 * only from an SD card, so it is the last thing that should be carrying a change. A host that
 * cannot talk to a broken application must still be able to talk to the thing that can replace
 * it, using tools that were working before any of this started.
 */

#include "usb_desc.h"

#define EP0_MPS   64
#define BULK_MPS  64

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

const uint8_t *usb_desc_device(uint32_t *len) { *len = sizeof(dev_desc); return dev_desc; }
const uint8_t *usb_desc_config(uint32_t *len) { *len = sizeof(cfg_desc); return cfg_desc; }
const uint8_t *usb_desc_bos(uint32_t *len)    { (void)len; return 0; }

uint32_t usb_desc_string(unsigned idx, uint8_t *buf)
{
    if (idx == 0) {
        for (unsigned i = 0; i < sizeof(str0); i++) buf[i] = str0[i];
        return sizeof(str0);
    }
    if (idx >= sizeof(strings) / sizeof(strings[0]) || !strings[idx]) return 0;

    const char *s = strings[idx];
    uint32_t n = 0;
    while (s[n]) n++;
    if (n > 30) n = 30;

    buf[0] = (uint8_t)(2 + 2 * n);
    buf[1] = 0x03;
    for (uint32_t i = 0; i < n; i++) {
        buf[2 + 2 * i]     = (uint8_t)s[i];
        buf[2 + 2 * i + 1] = 0;
    }
    return buf[0];
}

int usb_desc_class_request(uint8_t bmRequestType, uint8_t bRequest,
                           uint16_t value, uint16_t index, uint16_t length,
                           const uint8_t **data, uint32_t *len)
{
    (void)value;
    (void)index;

    /* CDC class requests. We do not implement a real UART, so line coding is accepted and
     * discarded — but it MUST be answered, or Windows fails to open the port. */
    if ((bmRequestType & 0x60) != 0x20) return USB_REQ_UNHANDLED;

    switch (bRequest) {
    case 0x20:                                     /* SET_LINE_CODING — 7 data bytes */
        if (length) { *len = length; return USB_REQ_DATA_OUT; }
        return USB_REQ_ACK;

    case 0x22:                                     /* SET_CONTROL_LINE_STATE — no data */
    case 0x23:                                     /* SEND_BREAK — no data */
        return USB_REQ_ACK;

    case 0x21: {                                   /* GET_LINE_CODING */
        static const uint8_t lc[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };  /* 115200 8N1 */
        *data = lc;
        *len  = sizeof(lc);
        return USB_REQ_DATA_IN;
    }

    default:
        return USB_REQ_UNHANDLED;
    }
}
