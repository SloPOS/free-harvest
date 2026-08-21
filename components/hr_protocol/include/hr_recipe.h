/*
 * hr_recipe - stored recipes, their wire format, and extra-dry learning.
 *
 * The dryer accepts a whole recipe in one frame:
 *
 *   SENDCANDY  "4,70,140,150,160,300,7200,300,CANDY,0,"     <counter>
 *   SENDCUSTOM "5,-10,31500,150,7200,500,1,100,54000,0,CUSTOM,1," <counter>
 *
 * Leading field is the family (4 candy, 5 custom), the name sits second from
 * last, and the final field is a start-now flag - a recipe sent with it set
 * begins a batch without anyone touching the panel.
 *
 * Times are SECONDS in these frames and MINUTES in the STAT telemetry that
 * reports them back. That is a property of the direction, not of the recipe
 * family; both families follow it.
 */
#ifndef HR_RECIPE_H
#define HR_RECIPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HR_RECIPE_NAME_MAX  24
#define HR_RECIPE_NOTES_MAX 192
#define HR_RECIPE_MAX_NUM   10   /* numeric fields before the name */

typedef enum {
    HR_FAM_CANDY  = 4,
    HR_FAM_CUSTOM = 5,
} hr_family_t;

/*
 * Field positions within num[], per family. Only the entries we have actually
 * derived by changing one control and diffing are named; the rest are carried
 * verbatim because we do not know what they mean and inventing a meaning is
 * how a recipe becomes a ruined batch.
 */
/* Candy: 8 numerics. num[0] is the family byte. */
#define HR_CANDY_TRAY_TEMP    1
#define HR_CANDY_PREWARM_TEMP 2
#define HR_CANDY_DRY_TEMP     3
#define HR_CANDY_PREWARM_S    5
#define HR_CANDY_DRY_S        6
#define HR_CANDY_NUMERICS     8

/* Custom: 10 numerics. */
#define HR_CUSTOM_FREEZE_TEMP   1
#define HR_CUSTOM_EXTRA_FREEZE_S 2
#define HR_CUSTOM_DRY_TEMP      3
#define HR_CUSTOM_NUMERICS      10

typedef struct {
    bool        used;
    hr_family_t family;
    char        name[HR_RECIPE_NAME_MAX];
    char        notes[HR_RECIPE_NOTES_MAX];
    int32_t     num[HR_RECIPE_MAX_NUM];
    uint8_t     nnum;
    uint32_t    runs;          /* completed batches using this recipe        */
    int32_t     extra_dry_s;   /* extra drying added on the last run, seconds */
} hr_recipe_t;

typedef enum {
    HR_RECIPE_OK = 0,
    HR_RECIPE_BAD_FAMILY,
    HR_RECIPE_BAD_NAME,       /* empty, too long, or illegal characters      */
    HR_RECIPE_NAME_HAS_VERB,  /* would be misrouted by the dryer's parser    */
    HR_RECIPE_BAD_FIELDS,     /* wrong count, or a value out of range        */
} hr_recipe_err_t;

/* How many numeric fields this family carries. 0 if the family is unknown. */
uint8_t hr_recipe_numerics(hr_family_t f);

/*
 * Check a name is safe to put on the wire.
 *
 * The dryer's dispatcher matches verbs with strstr over the WHOLE line, and
 * checks DEL, ADV, ADD, DIR, DUMP, COPY, HCS and SPC (among others) BEFORE
 * SENDCANDY. So a recipe called "ADDED SUGAR" or "RED DIRT" contains a verb
 * that matches first, and the frame is routed somewhere nobody intended.
 *
 * The match is case-sensitive, so only an uppercase collision is dangerous -
 * "Added" is fine and "ADDED" is not. Quotes and commas are rejected too,
 * since the name travels inside a quoted, comma-separated payload.
 */
hr_recipe_err_t hr_recipe_check_name(const char *name);

/* Full validation, including field count and plausible ranges. */
hr_recipe_err_t hr_recipe_validate(const hr_recipe_t *r);

/*
 * Build the complete frame, including the trailing counter.
 *
 * `start` sets the final payload field: true begins a batch immediately.
 * Returns bytes written, or 0 on failure - a short buffer never yields a
 * truncated but syntactically plausible recipe.
 */
size_t hr_recipe_build(const hr_recipe_t *r, bool start, uint32_t counter,
                       char *out, size_t cap);

const char *hr_recipe_err_str(hr_recipe_err_t e);

/* ---- extra-dry learning ------------------------------------------------ */

/*
 * Watches the screen and counts how often drying was extended.
 *
 * Pressing "More Dry Time" on the complete screen adds two hours and returns
 * the machine to drying, so the tell is a transition from screen 7 back to 5
 * or 6. Detecting it from the SCREEN rather than from our own commands means
 * it works when the button is pressed on the panel by hand, which is how it
 * usually will be.
 */
typedef struct {
    int      last_screen;
    uint32_t events;
} hr_dry_tracker_t;

void hr_dry_reset(hr_dry_tracker_t *t);
void hr_dry_observe(hr_dry_tracker_t *t, int screen);

/* Seconds of extra drying observed: two hours per extension. */
int32_t hr_dry_extra_s(const hr_dry_tracker_t *t);

/*
 * The dry time this recipe should carry next time, given what the last run
 * needed. Returns the unchanged value when nothing was added.
 */
int32_t hr_recipe_suggested_dry_s(const hr_recipe_t *r,
                                  const hr_dry_tracker_t *t);

#ifdef __cplusplus
}
#endif

#endif /* HR_RECIPE_H */
