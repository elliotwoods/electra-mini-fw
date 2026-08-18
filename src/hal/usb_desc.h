/* The descriptors and the class- or vendor-specific control requests that go with them.
 *
 * Split out of usb_fs.c because the two images that share that driver must NOT enumerate the
 * same way. The application moves to a vendor-specific interface so Windows binds WinUSB; the
 * BOOTLOADER MUST STAY CDC, because it is the one component that cannot be replaced over USB --
 * changing how it enumerates puts its own recovery path behind the change being recovered from.
 *
 * Keeping that as a note in a file both images compile would have made it a matter of nobody
 * editing the wrong branch. Two implementations of this header, one linked into each image,
 * makes it a matter of which file the linker was given.
 */

#ifndef ELECTRA_USB_DESC_H
#define ELECTRA_USB_DESC_H

#include <stdint.h>

const uint8_t *usb_desc_device(uint32_t *len);
const uint8_t *usb_desc_config(uint32_t *len);

/* Binary Object Store, for MS OS 2.0. Returns 0 for a device that has none -- which also means
 * its bcdUSB must be below 0x0201, since a host that sees 2.1 will ask. */
const uint8_t *usb_desc_bos(uint32_t *len);

/* Build string descriptor `idx` into `buf` (at least 64 bytes). Returns its length, or 0 if
 * there is no such string -- which the caller must answer with a stall. */
uint32_t usb_desc_string(unsigned idx, uint8_t *buf);

/* Class- or vendor-specific SETUP, i.e. anything with bmRequestType type bits non-zero.
 *
 * Returns 1 if handled with a data-in stage, in which case *data and *len describe the reply;
 * 2 if handled with no data stage (the caller acknowledges); 3 if handled with a data-OUT stage
 * of *len bytes to be received and discarded; 0 if unrecognised, which the caller stalls. */
#define USB_REQ_UNHANDLED  0
#define USB_REQ_DATA_IN    1
#define USB_REQ_ACK        2
#define USB_REQ_DATA_OUT   3

int usb_desc_class_request(uint8_t bmRequestType, uint8_t bRequest,
                           uint16_t value, uint16_t index, uint16_t length,
                           const uint8_t **data, uint32_t *len);

#endif /* ELECTRA_USB_DESC_H */
