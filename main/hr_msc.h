/*
 * USB mass-storage volume.
 *
 * The genuine HarvestRight adapter is a COMPOSITE device: CDC-ACM on
 * interface 0 and mass storage on interface 1. Its volume carries
 * `esp/version.txt` plus the dryer firmware images, which is how updates
 * physically reach the machine.
 *
 * We shipped CDC only. That is tolerated by dryer firmware 6.0.641041 - it
 * completes the full handshake against a CDC-only adapter - but a 6.0.644170
 * machine answers UNIQUE and nothing else: no SNM, no CFG, no reply to a
 * REQSTAT sent entirely on its own, and never any telemetry. Its dispatcher is
 * alive while the task behind it is not.
 *
 * The dryer's own strings put mass storage UPSTREAM of the serial port:
 *
 *     Waiting on USB MSC Init thread
 *     USB MSC finished starting CDC thread
 *     Error in initializing FAT on USB_HMSC
 *
 * "USB MSC finished starting CDC thread" is the ordering - the CDC thread is
 * started by the MSC init path, not alongside it. A half-brought-up USB stack
 * producing a half-alive protocol layer is exactly the symptom.
 *
 * So this presents a small FAT volume with a version.txt in the captured
 * format. Whether 644170 requires it is UNPROVEN - this is the experiment.
 */
#ifndef HR_MSC_H
#define HR_MSC_H

#include <stdbool.h>

/*
 * Prepare the volume and register it with TinyUSB.
 *
 * MUST be called BEFORE tinyusb_driver_install(): the host expects the LUN to
 * exist at enumeration, and a device that grows one afterwards is not a shape
 * any host is required to cope with.
 *
 * That places FAT work on the boot path, which is the thing hr_capture
 * deliberately delays by 8 seconds because doing it there broke USB. This
 * volume is far smaller and is formatted only once, but if USB enumeration
 * regresses, this is the first place to look.
 */
bool hr_msc_init(void);

/* True once the volume is registered. */
bool hr_msc_ready(void);

#endif /* HR_MSC_H */
