/*
 * hr_temp - the one place temperatures change units.
 *
 * The dryer reports whole degrees FAHRENHEIT in every frame that carries a
 * temperature (STAT field [4], the type-15 extras, the recipe fields), on
 * both the 6.0.641041 and the 6.0.644170 builds, and NOTHING on the wire
 * says which unit the operator has chosen on the panel: CFG is
 * "device-id, model, serial", SNM is the name, UID is identity + build,
 * SYSPREF (which the adapter never asks for) is dry-time settings. So the
 * adapter keeps every temperature it stores in degrees F - telemetry, trend,
 * logbook, capture - and converts ONLY when it presents a number to a human
 * or to Home Assistant. That is what this module is for.
 *
 * A preference of AUTO exists so that, should a firmware ever turn out to
 * report the panel's unit, following it is a one-line change here rather
 * than a new setting. Until then AUTO resolves to the fallback the caller
 * passes (Fahrenheit, what the dryer itself displays by default).
 *
 * Pure C11, no ESP-IDF; tested in test/test_hr_temp.c.
 */
#ifndef HR_TEMP_H
#define HR_TEMP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HR_TEMP_F = 0,
    HR_TEMP_C = 1,
} hr_temp_unit_t;

/* What the owner asked for. */
typedef enum {
    HR_TEMP_PREF_AUTO = 0, /* mirror the dryer if it ever says; else fallback */
    HR_TEMP_PREF_F,
    HR_TEMP_PREF_C,
} hr_temp_pref_t;

/* No unit has been reported by the dryer (always, as of this writing). */
#define HR_TEMP_DRYER_UNKNOWN (-1)

/*
 * Pick the unit to present in. `dryer_unit` is an hr_temp_unit_t the dryer
 * reported, or HR_TEMP_DRYER_UNKNOWN. An explicit F/C preference always wins;
 * AUTO follows the dryer when known and `fallback` otherwise.
 */
hr_temp_unit_t hr_temp_resolve(hr_temp_pref_t pref, int dryer_unit,
                               hr_temp_unit_t fallback);

/*
 * Whole degrees F -> tenths of a degree C, rounded to nearest (half away
 * from zero): 24 F -> -44 (-4.4 C), 32 -> 0, 212 -> 1000, -40 -> -400.
 * Integer arithmetic so the host tests and the target agree bit for bit.
 */
long hr_temp_f_to_c_x10(long f);

/* Whole degrees F -> whole degrees C, rounded to nearest (half away from 0). */
long hr_temp_f_to_c(long f);

/* Degrees F as a double in `unit`, for callers that format with %.1f. */
double hr_temp_from_f(long f, hr_temp_unit_t unit);

/* "F" or "C". Never NULL. */
const char *hr_temp_unit_letter(hr_temp_unit_t unit);

/* "f" / "c" / "auto", the spelling used in the API and in storage. */
const char *hr_temp_pref_str(hr_temp_pref_t pref);

/*
 * Parse "f", "F", "c", "C", "auto" (case-insensitive; also "fahrenheit" /
 * "celsius" / "imperial" / "metric"). Returns false and leaves *out alone
 * for anything else.
 */
bool hr_temp_pref_parse(const char *s, hr_temp_pref_t *out);

/*
 * Write the value of `f` degrees F in `unit` into `out`, as a JSON-safe
 * number: whole degrees for F ("24"), one decimal for C ("-4.4"). Returns
 * the length written, 0 if `cap` was too small.
 */
size_t hr_temp_fmt_num(long f, hr_temp_unit_t unit, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HR_TEMP_H */
