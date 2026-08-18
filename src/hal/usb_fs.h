/* Minimal USB full-speed device on the S7G2's USBFS peripheral.
 *
 * Presents as CDC-ACM, so Windows binds usbser.sys with no driver install and the device
 * appears as a COM port. That gives us a debug console and a command channel over the same
 * pipe, which is the fastest route out of "flash blind, reboot, squint at the screen".
 *
 * Deliberately POLLED. The bootloader has nothing else to do, and polling removes the NVIC,
 * the ICU event-link matrix and all reentrancy from the picture — three things that fail
 * silently on this part. usb_poll() must be called often enough to service control transfers
 * during enumeration; a few hundred microseconds is ample.
 */

#ifndef ELECTRA_USB_FS_H
#define ELECTRA_USB_FS_H

#include <stdint.h>

/* Bring up the peripheral and attach (D+ pull-up on, if VBUS is present). */
void usb_init(void);

/* Electrically detach from the bus (drop the D+ pull-up) and hold long enough for the host
 * to notice. Required before handing control between images: without it the host keeps a
 * stale device node that answers nothing. */
void usb_detach(void);

/* Service the peripheral. Drives enumeration and moves bulk data. Call in a loop. */
void usb_poll(void);

/* True once the host has issued SET_CONFIGURATION. */
int  usb_configured(void);

/* Bulk OUT: copy up to `max` received bytes. Returns the count, 0 if nothing waiting. */
uint32_t usb_read(uint8_t *buf, uint32_t max);

/* Bulk IN: queue bytes for the host. Returns bytes accepted; may be short if the TX
 * buffer is full, in which case call usb_poll() and retry. */
uint32_t usb_write(const uint8_t *buf, uint32_t len);

/* What a host asked for during enumeration, and what we refused.
 *
 * A device whose descriptors are wrong has no driver, and a device with no driver has no
 * console -- so the usual way to find out what happened is missing exactly when it is needed.
 * These survive a warm reset in SRAM, and the application packs them into the breadcrumb word
 * the bootloader prints, which is still reachable over CDC. */
typedef struct {
    uint16_t total;           /* SETUP packets seen at all */
    uint16_t bos;             /* GET_DESCRIPTOR(BOS) -- MS OS 2.0 begins here or not at all */
    uint16_t vendor;          /* vendor requests answered, i.e. the MS OS 2.0 fetch */
    uint16_t vendor_asked;    /* wLength of the last one; 178 means the host read the BOS */
    uint16_t stalls;
    uint16_t bulk_rx;         /* bulk OUT packets: a host that has a driver and is using it */
    uint8_t  bos_len;
    uint8_t  last_stall_req;
    uint8_t  last_stall_type;
} usb_setup_stats_t;

extern usb_setup_stats_t g_usb_setup;

/* Blocking convenience for diagnostics. Drops output if no host is reading, rather than
 * hanging the bootloader — an unread console must never wedge the device. */
extern uint32_t g_usb_tx_dropped;   /* console bytes lost; non-zero means output was cut */
void usb_write_block(const uint8_t *buf, uint32_t len);
void usb_puts(const char *s);
void usb_printf_u32(const char *label, uint32_t v);

#endif /* ELECTRA_USB_FS_H */
