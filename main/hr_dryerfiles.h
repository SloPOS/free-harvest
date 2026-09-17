/*
 * hr_dryerfiles - the dryer's own files (batch CSV logs) in the web panel.
 *
 * ESP-side glue around hr_files (components/hr_protocol): owns the transfer
 * state machine, the side buffer the stream collects FDFILEBLOCK frames in,
 * an 8 KB chunk buffer between the USB RX task and the HTTP handler that is
 * streaming the file to the browser, the last file list, and the on/off
 * switch in NVS.
 *
 * Files are STREAMED, not cached. The first version wrote them to the
 * capture partition: a 1 KB fwrite() on an 11.9 MB SPIFFS measured
 * 0.7-10 s, every flash write stalls the whole chip (code runs from flash),
 * the capture writer dropped 160 lines and the trend save failed while a
 * 90 KB file was being written, and the write itself then failed with EIO -
 * the failure mode /api/storage/format exists for. The dryer keeps the
 * files; the browser gets the bytes as the blocks arrive.
 *
 * Threads:
 *   USB RX task   hr_dryerfiles_on_frame() / the block sink - copy into the
 *                 chunk buffer, ask for the next block, never flash.
 *   main loop     hr_dryerfiles_tick() - timeouts and the link rule.
 *   httpd         refresh / fetch+chunks / cancel / snapshot - starts a
 *                 transfer and consumes chunks; the dryer is only ever
 *                 spoken to through hr_files' send callback.
 *
 * Safety: every request to the dryer is a read (FDFILES / FILEREAD, both
 * in the SAFE allow-list). They still go out only when the feature is
 * switched on, the link is up, and the dryer is NOT running a batch unless
 * the caller says `force` - ten requests a second on the USB link is not
 * something to do to a running machine without meaning to.
 *
 * Compiled only with CONFIG_HR_BATCH_HISTORY.
 */
#ifndef HR_DRYERFILES_H
#define HR_DRYERFILES_H

#include "hr_files.h"
#include "hr_session.h"

#include <stdbool.h>
#include <stddef.h>

/* List entries remembered from the last FDFILES sweep. */
#define HR_DF_LIST_MAX  40
/*
 * The chunk buffer between the USB RX task and the HTTP handler: 8 KB of
 * heap for the transfer. It is a RING in effect - the handler takes what
 * has arrived (from HR_DF_CHUNK_MIN_BLOCKS up) while the dryer keeps
 * filling the rest, and the dryer is only asked to wait when fewer than
 * `depth` blocks of room are left. The first version handed the buffer
 * out only when FULL and asked for nothing while the browser took it: on
 * the 276 KB file that idled the dryer for every 8 KB sent over a -95 dBm
 * Wi-Fi link, ~17 % of the 55 s.
 */
#define HR_DF_CHUNK_BLOCKS 8
#define HR_DF_CHUNK_MIN_BLOCKS 2

typedef enum {
    HR_DF_OK = 0,
    HR_DF_DISABLED,   /* switched off (settings) */
    HR_DF_BUSY,       /* a transfer is running */
    HR_DF_LINK,       /* no dryer link */
    HR_DF_RUNNING,    /* dryer is running a batch and force was not given */
    HR_DF_BADNAME,    /* not a name we will put on the wire */
    HR_DF_STORAGE,    /* no heap for the chunk buffer */
    HR_DF_TOOBIG,     /* over HR_FILES_MAX_SIZE */
} hr_df_result_t;

typedef struct {
    char name[HR_FILES_NAME_MAX];
    long size;
} hr_df_entry_t;

/* A consistent copy of everything the UI shows, taken under the lock. */
typedef struct {
    bool enabled;
    bool link_up;
    bool dryer_running;
    hr_files_state_t state;
    hr_files_err_t err;
    char name[HR_FILES_NAME_MAX];     /* file being read / last read */
    long block;
    long received;
    long size;
    int pct;
    unsigned long age_ms;             /* since the transfer finished; 0 if running */
    unsigned long elapsed_ms;         /* last/current transfer's duration */
    unsigned long list_age_ms;        /* since the list completed */
    bool list_valid;
    unsigned nlist;
    hr_df_entry_t list[HR_DF_LIST_MAX];
    /* lifetime counters, for the log and the live probe */
    unsigned long requests, timeouts, blocks_ok, blocks_bad, transfers,
                  blocks_in, big_dropped;
    /* pacing in force */
    int depth;
    unsigned long gap_ms;
    /* the last/current transfer's timing (hr_files_t) */
    unsigned long t_first_ms, t_dryer_ms, t_paused_ms;
} hr_df_snapshot_t;

void hr_dryerfiles_init(hr_session_t *s);

bool hr_dryerfiles_enabled(void);
bool hr_dryerfiles_set_enabled(bool on);   /* persists; false if NVS failed */

/* From the frame observer (USB RX task). Looks only at FDFILELIST. */
void hr_dryerfiles_on_frame(const hr_frame_t *f);

/* From the main loop. `dryer_running` is the phase tracker's verdict. */
void hr_dryerfiles_tick(unsigned long now_ms, bool link_up, bool dryer_running);

/* Ask the dryer for its file list (names containing `pattern`; NULL = ".csv"). */
hr_df_result_t hr_dryerfiles_refresh(const char *pattern, bool force);

/*
 * Start reading `name` from the dryer. Blocks are handed out by
 * hr_dryerfiles_take_chunk(); the caller MUST keep calling it until it
 * reports done or error, or call hr_dryerfiles_cancel(). A caller that
 * stops asking is noticed: the machine aborts (ERR_SINK) after a few
 * seconds without a consumer.
 */
hr_df_result_t hr_dryerfiles_fetch(const char *name, bool force);

/*
 * Next chunk of the file being read, up to HR_DF_CHUNK_BLOCKS * 1024 bytes
 * (CRs restored). Returns the number of bytes copied, 0 when nothing is
 * ready yet. *done is set once the whole file has been handed out or the
 * transfer failed (*err says which); after that the transfer's buffer is
 * released and a new fetch may start.
 */
size_t hr_dryerfiles_take_chunk(char *out, size_t cap, bool *done,
                                hr_files_err_t *err);

void hr_dryerfiles_cancel(void);

void hr_dryerfiles_snapshot(hr_df_snapshot_t *out);

/* One line for /api/state: "files_state":"idle","files_requests":N,... .
 * Cheap, takes the lock briefly. Returns bytes written. */
int hr_dryerfiles_state_json(char *out, size_t cap);

const char *hr_df_result_str(hr_df_result_t r);

/*
 * Pacing: FILEREADs in flight (1..HR_FILES_DEPTH_MAX) and the minimum gap
 * between requests. Kconfig gives the defaults, NVS (hrfiles/depth, /gap)
 * overrides them; set() persists and applies at the next request.
 */
void hr_dryerfiles_pacing(int *depth, unsigned long *gap_ms);
bool hr_dryerfiles_set_pacing(int depth, unsigned long gap_ms);

#endif /* HR_DRYERFILES_H */
