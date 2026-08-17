/*
 * USB CDC-ACM device transport.
 *
 * The freeze dryer is the USB *host*; this firmware presents itself as a
 * CDC-ACM device, which is what the stock HarvestRight adapter does. All
 * protocol bytes flow over this pipe.
 */
#ifndef HR_USB_H
#define HR_USB_H

#include "hr_session.h"

/* Install the TinyUSB driver and bind RX to `session`. */
void hr_usb_init(hr_session_t *session);

/* hr_tx_fn implementation - pass as the session's transmit callback. */
void hr_usb_tx(const char *data, size_t len, void *user);

/*
 * True once the host has asserted DTR (port opened).
 *
 * NOTE: this dryer never asserts DTR - every capture taken from real hardware
 * shows dtr=0 rts=0 - so do NOT gate reception on it. Reception is driven
 * purely by the TinyUSB RX callback. Diagnostics only.
 */
bool hr_usb_host_present(void);

/*
 * USB-level diagnostics, deliberately independent of frame parsing. These let
 * a boot log distinguish three otherwise identical-looking failures:
 *
 *   not mounted            -> the dryer never enumerated us (cable, port, VBUS,
 *                             or the dryer isn't scanning USB right now)
 *   mounted, 0 rx bytes    -> it sees a CDC device but sends us nothing
 *   rx bytes but no frames -> bytes arrive and the parser rejects them
 */
/*
 * Force a USB detach then re-attach (~1.5s gap), without rebooting. Mimics what
 * the dryer sees during an adapter reboot, which is known to re-establish the
 * CDC link, so a dropped link can be retried without power-cycling the machine
 * mid-batch. Returns false only if the worker task could not be created.
 */
bool hr_usb_bus_reattach(void);

bool hr_usb_mounted(void);            /* host completed SET_CONFIGURATION */
bool hr_usb_suspended(void);          /* bus suspended by the host */
unsigned long hr_usb_rx_bytes(void);  /* raw bytes received, pre-parser */
unsigned hr_usb_mount_events(void);   /* mount count; >1 means re-enumeration */

#endif /* HR_USB_H */
