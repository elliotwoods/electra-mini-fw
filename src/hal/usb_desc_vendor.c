/* Vendor-specific descriptors with MS OS 2.0, so Windows binds WinUSB unaided.
 *
 * WHY LEAVE CDC. usbser.sys is a serial-port emulation with a control plane we do not implement
 * and do not want: it issues SET_LINE_CODING on open and refuses the port if the answer is not
 * to its liking, it gates writes on flow-control signals meaningless to a device that is not a
 * UART, and it hands the application a stream with no notion of a transfer. Every Python tool
 * here already bypasses pyserial with raw Win32 for exactly that reason. A vendor interface
 * removes the emulation rather than continuing to work around it.
 *
 * WHY NOT HID, which needs no driver at all: an interrupt endpoint is polled once per interval
 * whatever its size, which caps a full-speed device at 64 KB/s per direction. A bulk transfer
 * may span many packets in one frame. docs/protocol.md 0 has the numbers.
 *
 * TWO TRAPS, both paid for in advance here.
 *
 * The PRODUCT ID IS NEW. Windows caches "this device has no MS OS descriptors" per VID/PID after
 * the first failed probe, and this device previously stalled string index 0xEE -- which is
 * exactly that probe. Reusing the old PID would mean debugging a cached negative result that has
 * nothing to do with the descriptors below.
 *
 * A RAW HOST STACK ISSUES REQUESTS usbser.sys never did. SET_INTERFACE and CLEAR_FEATURE
 * (ENDPOINT_HALT) are how a WinUSB application resets a stalled pipe, and both used to stall.
 * They are implemented in usb_fs.c alongside the other standard requests.
 */

#include "usb_desc.h"

#define EP0_MPS   64
#define BULK_MPS  64

/* bcdUSB 2.10, which is what makes a host ask for the BOS descriptor at all. */
static const uint8_t dev_desc[18] = {
    18, 0x01, 0x10, 0x02,
    0x00, 0x00, 0x00, EP0_MPS,       /* class defined at the interface, not the device */
    0xC9, 0x1F,                      /* idVendor  0x1FC9 */
    0xD0, 0x82,                      /* idProduct 0x82D0 — new; see the note above about 0xEE */
    0x03, 0x00,                      /* bcdDevice 0.03 -- Windows caches its MS OS decision per
                                      * VID/PID/REV, so a revision bump is how you get a fresh
                                      * probe instead of yesterday's answer replayed */
    1, 2, 0,                         /* iManufacturer, iProduct, NO iSerial -- see below */
    1
};

/* iSerial is deliberately 0.
 *
 * It used to be the constant "EMB-0001" on every unit, which is worse than absent: Windows uses
 * the serial number as instance identity, so two of these plugged into one machine would claim
 * to be the same device. With no serial, Windows keys on the port path instead and two units
 * coexist correctly.
 *
 * A real per-unit serial would be better than either. This part has no readable unique ID that
 * we can find -- the factory information region at 0x01000000 reads as 8 KB of zeros on this
 * board -- so inventing one that is really a constant would just restore the collision while
 * looking like it had been solved. */

#define CFG_TOTAL 32
static const uint8_t cfg_desc[CFG_TOTAL] = {
    9, 0x02, CFG_TOTAL, 0x00, 1, 1, 0, 0x80, 250,       /* one interface, bus powered, 500 mA */

    /* interface 0: vendor specific. The interrupt endpoint the CDC configuration declared was
     * never configured by the driver and never carried a byte; it is simply gone. */
    9, 0x04, 0, 0, 2, 0xFF, 0x00, 0x00, 0,
    7, 0x05, 0x01, 0x02, BULK_MPS, 0x00, 0,             /* EP 0x01 bulk OUT */
    7, 0x05, 0x81, 0x02, BULK_MPS, 0x00, 0,             /* EP 0x81 bulk IN  */
};

static const uint8_t str0[] = { 4, 0x03, 0x09, 0x04 };  /* en-US */
static const char *const strings[] = { 0, "Kimchi and Chips", "Electra Mini FW" };

/* ---------------------------------------------------------------- MS OS 2.0
 *
 * The mechanism: bcdUSB 2.10 makes Windows fetch the BOS descriptor; the platform capability
 * below names a vendor request code; Windows issues that request and gets the descriptor set,
 * which says "bind WinUSB to this function" and gives it a device interface GUID. No INF, no
 * Zadig, no user step.
 *
 * Every length here is spelled out with its arithmetic. A wrong wTotalLength is the classic
 * failure: Windows reads what it was told to read, finds it malformed, and silently binds
 * nothing -- and the only symptom is a device with no driver and no error anywhere.
 */

#define MSOS20_VENDOR_CODE  0x21u
#define MSOS20_DESC_INDEX   0x0007u

#define COMPAT_ID_LEN      20u
#define FUNC_SUBSET_LEN    (8u + COMPAT_ID_LEN)                         /* 28 */
#define CONFIG_SUBSET_LEN  (8u + FUNC_SUBSET_LEN)                       /* 36 */
#define MSOS20_TOTAL_LEN   (10u + CONFIG_SUBSET_LEN)                    /* 46 */

/* NO REGISTRY PROPERTY, and the omission is deliberate.
 *
 * A DeviceInterfaceGUIDs property is the usual way to give a WinUSB device its own interface
 * GUID, and carrying one pushed this descriptor set to 178 bytes -- three packets on a 64-byte
 * control endpoint. Without it the set is 46 bytes and goes out in ONE, which removes the
 * multi-packet EP0 path from the list of things that have to be right for a device to get a
 * driver at all. That path had never carried more than two packets before; making the very
 * first thing that depends on it also be the thing you cannot debug without it is a poor trade.
 *
 * Nothing is lost. WinUSB binds on the compatible ID alone, and the host finds the device
 * through the generic USB device interface class filtered by VID and PID -- which is what
 * tools/deploy/winusb.py does, and is more robust than a GUID both sides have to agree on. */
static const uint8_t msos20_desc[MSOS20_TOTAL_LEN] = {
    /* Set header */
    0x0A, 0x00,                     /* wLength 10 */
    0x00, 0x00,                     /* MS_OS_20_SET_HEADER_DESCRIPTOR */
    0x00, 0x00, 0x03, 0x06,         /* dwWindowsVersion: 8.1 (0x06030000) */
    (uint8_t)MSOS20_TOTAL_LEN, (uint8_t)(MSOS20_TOTAL_LEN >> 8),

    /* Configuration subset, for configuration 0 (the index, not bConfigurationValue) */
    0x08, 0x00,
    0x01, 0x00,                     /* MS_OS_20_SUBSET_HEADER_CONFIGURATION */
    0x00,                           /* bConfigurationValue - 1 */
    0x00,                           /* bReserved */
    (uint8_t)CONFIG_SUBSET_LEN, (uint8_t)(CONFIG_SUBSET_LEN >> 8),

    /* Function subset, starting at interface 0 */
    0x08, 0x00,
    0x02, 0x00,                     /* MS_OS_20_SUBSET_HEADER_FUNCTION */
    0x00,                           /* bFirstInterface */
    0x00,                           /* bReserved */
    (uint8_t)FUNC_SUBSET_LEN, (uint8_t)(FUNC_SUBSET_LEN >> 8),

    /* Compatible ID: bind WinUSB */
    0x14, 0x00,
    0x03, 0x00,                     /* MS_OS_20_FEATURE_COMPATIBLE_ID */
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     /* no sub-compatible ID */
};

#define BOS_TOTAL_LEN 33u

static const uint8_t bos_desc[BOS_TOTAL_LEN] = {
    0x05, 0x0F,                     /* bLength, BOS */
    (uint8_t)BOS_TOTAL_LEN, 0x00,
    0x01,                           /* one device capability */

    0x1C, 0x10, 0x05, 0x00,         /* 28 bytes, DEVICE_CAPABILITY, PLATFORM, reserved */
    /* {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F}, in the mixed-endian form the spec uses */
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06,         /* dwWindowsVersion: 8.1 */
    (uint8_t)MSOS20_TOTAL_LEN, (uint8_t)(MSOS20_TOTAL_LEN >> 8),
    MSOS20_VENDOR_CODE,
    0x00                            /* bAltEnumCode: no alternate enumeration */
};

const uint8_t *usb_desc_device(uint32_t *len) { *len = sizeof(dev_desc); return dev_desc; }
const uint8_t *usb_desc_config(uint32_t *len) { *len = sizeof(cfg_desc); return cfg_desc; }
const uint8_t *usb_desc_bos(uint32_t *len)    { *len = sizeof(bos_desc); return bos_desc; }

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
    (void)length;

    /* Vendor requests only; this interface has no class. */
    if ((bmRequestType & 0x60) != 0x40) return USB_REQ_UNHANDLED;

    /* The MS OS 2.0 fetch. Windows sends it to the DEVICE for a device-level descriptor set,
     * but accepting it at the interface too costs one comparison and removes a whole class of
     * "works on that machine" difference between host stacks. */
    if (bRequest == MSOS20_VENDOR_CODE && index == MSOS20_DESC_INDEX) {
        *data = msos20_desc;
        *len  = sizeof(msos20_desc);
        return USB_REQ_DATA_IN;
    }

    return USB_REQ_UNHANDLED;
}
