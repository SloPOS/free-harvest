/*
 * The owner's temperature-unit preference: Fahrenheit or Celsius, for the web
 * UI, /api/state, MQTT / Home Assistant and (on boards that have one) the
 * display. One setting, applied everywhere a temperature is shown.
 *
 * The dryer itself always sends degrees F and never reports the unit its own
 * panel is set to (hr_temp.h has the wire evidence), so there is nothing to
 * mirror: this preference is the source of truth, stored in NVS so it
 * survives a reboot, and Fahrenheit until the owner changes it - the numbers
 * then match the dryer's panel out of the box. Stored values stay in F; only
 * presentation converts (hr_temp.h).
 *
 * Never sends anything to the dryer: the setting is the adapter's alone.
 */
#ifndef HR_UNITS_H
#define HR_UNITS_H

#include "hr_temp.h"

#include <stdbool.h>

/* Read the stored preference (or the default). Call once at boot. */
void hr_units_init(void);

/* What the owner asked for (f / c; auto is accepted but resolves to F). */
hr_temp_pref_t hr_units_pref(void);

/* The unit to present temperatures in, right now. */
hr_temp_unit_t hr_units_temp(void);

/* Shorthand for the display: true when presenting in Celsius. */
bool hr_units_metric(void);

/*
 * Store and apply. Returns false if NVS refused the write; the in-memory
 * value is still updated so the UI follows the request immediately.
 */
bool hr_units_set_pref(hr_temp_pref_t pref);

/* True once after every change, for the main loop (MQTT rediscovery). */
bool hr_units_take_changed(void);

#endif /* HR_UNITS_H */
