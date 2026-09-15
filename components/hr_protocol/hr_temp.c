#include "hr_temp.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

hr_temp_unit_t hr_temp_resolve(hr_temp_pref_t pref, int dryer_unit,
                               hr_temp_unit_t fallback)
{
    switch (pref) {
    case HR_TEMP_PREF_F:
        return HR_TEMP_F;
    case HR_TEMP_PREF_C:
        return HR_TEMP_C;
    case HR_TEMP_PREF_AUTO:
    default:
        break;
    }
    if (dryer_unit == (int)HR_TEMP_F || dryer_unit == (int)HR_TEMP_C) {
        return (hr_temp_unit_t)dryer_unit;
    }
    return fallback;
}

/* (f - 32) * 50 / 9, rounded half away from zero. */
long hr_temp_f_to_c_x10(long f)
{
    long num = (f - 32) * 50;
    if (num >= 0) {
        return (num + 4) / 9;
    }
    return -((-num + 4) / 9);
}

long hr_temp_f_to_c(long f)
{
    long num = (f - 32) * 5;
    if (num >= 0) {
        return (num + 4) / 9;
    }
    return -((-num + 4) / 9);
}

double hr_temp_from_f(long f, hr_temp_unit_t unit)
{
    if (unit == HR_TEMP_C) {
        return (double)hr_temp_f_to_c_x10(f) / 10.0;
    }
    return (double)f;
}

const char *hr_temp_unit_letter(hr_temp_unit_t unit)
{
    return unit == HR_TEMP_C ? "C" : "F";
}

const char *hr_temp_pref_str(hr_temp_pref_t pref)
{
    switch (pref) {
    case HR_TEMP_PREF_F:
        return "f";
    case HR_TEMP_PREF_C:
        return "c";
    case HR_TEMP_PREF_AUTO:
    default:
        return "auto";
    }
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

bool hr_temp_pref_parse(const char *s, hr_temp_pref_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (ieq(s, "f") || ieq(s, "fahrenheit") || ieq(s, "imperial")) {
        *out = HR_TEMP_PREF_F;
        return true;
    }
    if (ieq(s, "c") || ieq(s, "celsius") || ieq(s, "metric")) {
        *out = HR_TEMP_PREF_C;
        return true;
    }
    if (ieq(s, "auto")) {
        *out = HR_TEMP_PREF_AUTO;
        return true;
    }
    return false;
}

size_t hr_temp_fmt_num(long f, hr_temp_unit_t unit, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    int n;
    if (unit == HR_TEMP_C) {
        /* Sign handled by hand so "-0.6" keeps its minus (whole part 0). */
        long x10 = hr_temp_f_to_c_x10(f);
        long mag = x10 < 0 ? -x10 : x10;
        n = snprintf(out, cap, "%s%ld.%ld", x10 < 0 ? "-" : "", mag / 10,
                     mag % 10);
    } else {
        n = snprintf(out, cap, "%ld", f);
    }
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}
