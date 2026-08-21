#include "hr_recipe.h"

#include <stdio.h>
#include <string.h>

/*
 * Verbs the dryer's dispatcher checks BEFORE SENDCANDY / SENDCUSTOM.
 *
 * Taken from the dispatcher's own chain order (see PROTOCOL_NOTES: the verb
 * to command-ID map is in source order, and these all carry lower IDs than
 * SENDBATCH at 0x1A). Matching is strstr over the whole line, so any of these
 * appearing inside a recipe name wins before the SEND* verb is ever tested.
 *
 * Short ones are the dangerous ones: ADD, DIR, DEL and XW are easy to hit by
 * accident in ordinary English written in capitals.
 */
static const char *const k_earlier_verbs[] = {
    "MEMTEST", "PRINT", "BEEP", "DIR", "MEMSIZE", "RMOLD", "XWIFI", "XW",
    "DUTY", "SERIAL", "FUZZY", "DUMP", "COPY", "DEL", "DIRC", "GETR", "GETP",
    "ADV", "ADD", "UNIQUE", "FDNAME", "STATUS", "CLICK", "STATE", "FDRENAME",
    "SENDBATCH",
};

uint8_t hr_recipe_numerics(hr_family_t f)
{
    switch (f) {
    case HR_FAM_CANDY:  return HR_CANDY_NUMERICS;
    case HR_FAM_CUSTOM: return HR_CUSTOM_NUMERICS;
    default:            return 0;
    }
}

hr_recipe_err_t hr_recipe_check_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return HR_RECIPE_BAD_NAME;
    }
    size_t n = strlen(name);
    if (n >= HR_RECIPE_NAME_MAX) {
        return HR_RECIPE_BAD_NAME;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        /*
         * The name rides inside a quoted, comma-separated payload, so a comma
         * or a quote does not corrupt the name - it silently shifts every
         * field after it, which lands as a different recipe rather than an
         * error. Control characters would break the frame outright.
         */
        if (c == ',' || c == '"' || c < 0x20 || c > 0x7e) {
            return HR_RECIPE_BAD_NAME;
        }
    }
    for (size_t i = 0; i < sizeof(k_earlier_verbs) / sizeof(k_earlier_verbs[0]);
         i++) {
        if (strstr(name, k_earlier_verbs[i]) != NULL) {
            return HR_RECIPE_NAME_HAS_VERB;
        }
    }
    return HR_RECIPE_OK;
}

/*
 * Range checks.
 *
 * These bounds are deliberately generous. Every value we have seen is real,
 * but we have seen only a handful, and rejecting a legitimate recipe because
 * it sits outside our small sample would be its own kind of wrong. The job
 * here is to catch a field that is obviously not a temperature or obviously
 * not a duration - a units mix-up, a sign error, a shifted field - not to
 * second-guess the machine's own limits.
 */
#define TEMP_MIN  (-80)
#define TEMP_MAX  (250)
#define TIME_MAX  (24 * 3600)

static bool temp_ok(int32_t v) { return v >= TEMP_MIN && v <= TEMP_MAX; }
static bool time_ok(int32_t v) { return v >= 0 && v <= TIME_MAX; }

hr_recipe_err_t hr_recipe_validate(const hr_recipe_t *r)
{
    if (r == NULL) {
        return HR_RECIPE_BAD_FIELDS;
    }
    uint8_t want = hr_recipe_numerics(r->family);
    if (want == 0) {
        return HR_RECIPE_BAD_FAMILY;
    }
    if (r->nnum != want) {
        return HR_RECIPE_BAD_FIELDS;
    }
    hr_recipe_err_t e = hr_recipe_check_name(r->name);
    if (e != HR_RECIPE_OK) {
        return e;
    }
    /* The family byte must agree with the family, or the dryer is told one
     * thing by the verb and another by the payload. */
    if (r->num[0] != (int32_t)r->family) {
        return HR_RECIPE_BAD_FIELDS;
    }

    if (r->family == HR_FAM_CANDY) {
        if (!temp_ok(r->num[HR_CANDY_TRAY_TEMP]) ||
            !temp_ok(r->num[HR_CANDY_PREWARM_TEMP]) ||
            !temp_ok(r->num[HR_CANDY_DRY_TEMP]) ||
            !time_ok(r->num[HR_CANDY_PREWARM_S]) ||
            !time_ok(r->num[HR_CANDY_DRY_S])) {
            return HR_RECIPE_BAD_FIELDS;
        }
    } else {
        if (!temp_ok(r->num[HR_CUSTOM_FREEZE_TEMP]) ||
            !temp_ok(r->num[HR_CUSTOM_DRY_TEMP]) ||
            !time_ok(r->num[HR_CUSTOM_EXTRA_FREEZE_S])) {
            return HR_RECIPE_BAD_FIELDS;
        }
    }
    return HR_RECIPE_OK;
}

size_t hr_recipe_build(const hr_recipe_t *r, bool start, uint32_t counter,
                       char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (hr_recipe_validate(r) != HR_RECIPE_OK) {
        return 0;
    }

    const char *verb = (r->family == HR_FAM_CANDY) ? "SENDCANDY" : "SENDCUSTOM";

    /*
     * Build into a scratch buffer first. Writing straight into `out` and
     * bailing halfway would leave a truncated payload that still parses -
     * a recipe with the tail missing is a plausible-looking wrong recipe,
     * which is worse than no frame at all.
     */
    char csv[256];
    size_t at = 0;
    for (uint8_t i = 0; i < r->nnum; i++) {
        int w = snprintf(csv + at, sizeof(csv) - at, "%ld,",
                         (long)r->num[i]);
        if (w < 0 || (size_t)w >= sizeof(csv) - at) {
            return 0;
        }
        at += (size_t)w;
    }
    int w = snprintf(csv + at, sizeof(csv) - at, "%s,%d,", r->name,
                     start ? 1 : 0);
    if (w < 0 || (size_t)w >= sizeof(csv) - at) {
        return 0;
    }

    /* The payload is quoted and the counter follows outside the quotes. */
    int n = snprintf(out, cap, "%s \"%s\" %lu", verb, csv,
                     (unsigned long)counter);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

const char *hr_recipe_err_str(hr_recipe_err_t e)
{
    switch (e) {
    case HR_RECIPE_OK:            return "ok";
    case HR_RECIPE_BAD_FAMILY:    return "unknown recipe family";
    case HR_RECIPE_BAD_NAME:      return "name is empty, too long, or has a comma or quote";
    case HR_RECIPE_NAME_HAS_VERB: return "name contains a protocol verb and would be misrouted";
    case HR_RECIPE_BAD_FIELDS:    return "wrong field count or a value out of range";
    default:                      return "unknown";
    }
}

/* ---- extra-dry learning ------------------------------------------------ */

#define HR_SCREEN_DRYING    5
#define HR_SCREEN_FINAL_DRY 6
#define HR_SCREEN_COMPLETE  7
#define HR_EXTRA_DRY_STEP_S (2 * 3600)   /* the button adds two hours */

void hr_dry_reset(hr_dry_tracker_t *t)
{
    if (t != NULL) {
        t->last_screen = -1;
        t->events = 0;
    }
}

void hr_dry_observe(hr_dry_tracker_t *t, int screen)
{
    if (t == NULL || screen < 0) {
        return;
    }
    /*
     * Going BACKWARDS from Complete into drying is the signature of "More Dry
     * Time" - the batch had finished and someone asked for more. Watching the
     * screen rather than our own outbound commands means this also counts
     * presses made by hand on the panel, which is how most of them happen.
     */
    if (t->last_screen == HR_SCREEN_COMPLETE &&
        (screen == HR_SCREEN_DRYING || screen == HR_SCREEN_FINAL_DRY)) {
        t->events++;
    }
    t->last_screen = screen;
}

int32_t hr_dry_extra_s(const hr_dry_tracker_t *t)
{
    if (t == NULL) {
        return 0;
    }
    return (int32_t)t->events * HR_EXTRA_DRY_STEP_S;
}

int32_t hr_recipe_suggested_dry_s(const hr_recipe_t *r,
                                  const hr_dry_tracker_t *t)
{
    if (r == NULL) {
        return 0;
    }
    /* Only Candy carries an explicit dry time; Custom's drying is governed by
     * fields we have not identified, so there is nothing honest to suggest. */
    if (r->family != HR_FAM_CANDY || r->nnum <= HR_CANDY_DRY_S) {
        return 0;
    }
    int32_t base = r->num[HR_CANDY_DRY_S];
    int32_t add = hr_dry_extra_s(t);
    if (add <= 0) {
        return base;
    }
    int32_t want = base + add;
    return (want > TIME_MAX) ? TIME_MAX : want;
}
