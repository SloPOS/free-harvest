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

/* True once the host has asserted DTR (port opened). */
bool hr_usb_host_present(void);

#endif /* HR_USB_H */
