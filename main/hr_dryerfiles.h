/*
 * hr_dryerfiles - the ESP side of reading the dryer's own files (hr_files.h).
 *
 * WHAT IT IS FOR
 *
 * The dryer keeps a CSV log of every batch it has run, a row a minute, with
 * the shelf and ambient thermocouples, the vacuum and the phase. That is the
 * history the adapter cannot reconstruct: it only ever sees a run it was
 * plugged in for, and no live frame carries the ambient reading at all.
 *
 * HOW A TRANSFER RUNS
 *
 * In the background, not inside an HTTP request. A POST starts it; the USB RX
 * task asks the dryer for one block, hands the bytes to a small ring here, and
 * asks for the next; the browser drains the ring with ordinary GETs and
 * assembles the file itself. Nothing is written to flash, and the web server
 * is never held for the length of a transfer - a 276 KB log takes the better
 * part of a minute at the dryer's own pace, and the dashboard stays live
 * throughout. It is also what makes progress visible while it happens.
 *
 * If the browser stops draining, the ring fills, the transfer pauses, and
 * after a while of nobody taking the bytes it is given up on.
 *
 * SAFETY
 *
 * Reads only: FDFILES and FILEREAD, both already in the safe verb set, and
 * every name or pattern goes through hr_files_arg_ok() before it can reach
 * the wire. Requests go out only while the feature is switched on, the link
 * is up, and the dryer is not running a batch - unless the caller says force.
 */
#ifndef HR_DRYERFILES_H
#define HR_DRYERFILES_H

#include "hr_files.h"
#include "hr_session.h"

#include <stdbool.h>
#include <stddef.h>

/* Most entries we keep from one listing. */
#define HR_DF_LIST_MAX 24
/* The ring between the USB RX task and the browser. One block is 1 KB, so
 * this is eight of them in hand: enough that an ordinary poll never finds it
 * empty, small enough to sit on a chip with no PSRAM. */
#define HR_DF_RING_CAP (8 * 1024)
/* Nobody draining for this long ends the transfer. */
#define HR_DF_IDLE_MS  30000UL

typedef enum {
    HR_DF_OK = 0,
    HR_DF_DISABLED,   /* the switch is off */
    HR_DF_BUSY,       /* one transfer at a time */
    HR_DF_LINK,       /* no dryer */
    HR_DF_RUNNING,    /* a batch is under way and force was not asked */
    HR_DF_BADNAME,    /* not something we will send (hr_files_arg_ok) */
    HR_DF_TOOBIG,     /* larger than HR_FILE_SIZE_MAX */
    HR_DF_NOMEM,
} hr_df_result_t;

const char *hr_df_result_str(hr_df_result_t r);

typedef struct {
    char name[HR_FILE_NAME_MAX];
    long size;
} hr_df_entry_t;

typedef struct {
    bool enabled;
    bool link_up;
    bool dryer_running;
    hr_files_state_t state;
    hr_files_err_t err;
    bool busy;
    char file[HR_FILE_NAME_MAX];  /* the pattern, or the file being read */
    long received;
    long size;                    /* -1 until the dryer says */
    int pct;                      /* -1 when the size is unknown */
    unsigned long elapsed_ms;
    bool list_valid;
    unsigned nlist;
    hr_df_entry_t list[HR_DF_LIST_MAX];
    /* counters since boot */
    unsigned long requests;
    unsigned long timeouts;
    unsigned long blocks_ok;
    unsigned long blocks_bad;
    unsigned long transfers;
    unsigned long blocks_in;      /* whole block frames the session delivered */
    unsigned long blocks_dropped; /* blocks the framer had to swallow */
} hr_df_snapshot_t;

/* Lend the session its side buffer and read the stored switch. Call before
 * the USB link comes up, so the first byte already meets a complete framer. */
void hr_dryerfiles_init(hr_session_t *s);

bool hr_dryerfiles_enabled(void);
bool hr_dryerfiles_set_enabled(bool on);

/* Ask the dryer for a listing, or for one file. Neither blocks. */
hr_df_result_t hr_dryerfiles_list(const char *pattern, bool force);
hr_df_result_t hr_dryerfiles_read(const char *name, bool force);

void hr_dryerfiles_cancel(void);

/*
 * Take what has arrived, up to `cap` bytes. `more` comes back true while the
 * caller should keep asking - the transfer is still running, or there are
 * bytes left in the ring. Draining is also what resumes a transfer that
 * paused for want of room.
 */
size_t hr_dryerfiles_take(char *out, size_t cap, bool *more);

/* A decoded frame from the RX task; only FDFILELIST is of interest. */
void hr_dryerfiles_on_frame(const hr_frame_t *f);

/* From the main loop: timeouts, the link rule, and the idle consumer. */
void hr_dryerfiles_tick(bool link_up, bool dryer_running);

void hr_dryerfiles_snapshot(hr_df_snapshot_t *out);

/* A few fields for /api/state. Returns bytes written, 0 if they did not fit. */
int hr_dryerfiles_state_json(char *out, size_t cap);

#endif /* HR_DRYERFILES_H */
