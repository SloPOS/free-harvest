/*
 * Runtime switch for the 6.0.644170 handshake variant (hr_session_set_compat).
 *
 * Stored in NVS so it can be flipped from the web UI without a reflash and
 * survives a reboot; CONFIG_HR_COMPAT_644170 is only the value used while
 * nothing has been stored. The main loop applies changes to the session and
 * restarts the handshake, so an A/B test on a live dryer is one POST.
 */
#ifndef HR_COMPAT_H
#define HR_COMPAT_H

#include <stdbool.h>

/* Read the stored value (or the Kconfig default). Call once at boot. */
void hr_compat_init(void);

/* Current setting. */
bool hr_compat_644170(void);

/* Store and apply. Returns false if NVS refused the write; the in-memory
 * value is still updated so the running session follows the request. */
bool hr_compat_set_644170(bool on);

/* True once after every change, for the main loop to pick up. */
bool hr_compat_take_changed(void);

#endif /* HR_COMPAT_H */
