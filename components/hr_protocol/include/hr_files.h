/*
 * hr_files - client for the dryer's file protocol: FDFILES / FILEREAD and the
 * FDFILELIST / FDFILEBLOCK replies. Reads only. Pure C, no ESP-IDF, host
 * testable.
 *
 * The wire format below was first reconstructed from the 6.0.641041 image
 * (G0641041.h6r.bin, Renesas RA, base 0x18000 - the addresses are into that
 * image) and then CONFIRMED LIVE on 2026-09-17 against a 6.0.644170 machine:
 * listing, 400+ blocks with matching checksums, a 276 KB batch log read
 * whole. Two things the image reading got wrong and the live machine put
 * right are called out below. The spec is also in decoded/PROTOCOL_NOTES.md.
 *
 * The dryer is the file server, the adapter asks (space-delimited, CR):
 *
 *     FDFILES <pattern> <index>        -> FDFILELIST,<name>,<index>,<size>\r
 *                                         FDFILELIST,NULL,<index>,0\r  past the end
 *     FILEREAD <name> <block>          -> FDFILEBLOCK,<name>,<nbytes>,<block>,<size>,<data...>XX\r
 *
 * FDFILES (executor 0x2b868 -> 0x20eb0 -> directory walk 0x20df8): walks the
 * root of the dryer's internal drive, skips names starting with '.', skips
 * directories, keeps entries whose name CONTAINS <pattern> (strstr), and
 * answers with the <index>-th match. Off the end the name literal is "NULL"
 * (0x84980). One entry per request. Matches the earlier live capture
 * "FDFILES .dat 0 -> FDFILELIST,HH.37935.dat,0,4341" and, on 6.0.644170,
 * "FDFILES .csv 0" -> the batch logs, seven of them, then the NULL entry.
 *
 * FILEREAD (executor 0x2b960 -> 0x20f30): opens <name> (data-flash table of
 * 33 named files first - HRTempFC.txt is one - then the filesystem), reads
 * 0x400 = 1024 bytes at offset <block>*1024 into 0x20006be0, replaces every
 * 0x0D in the data with 0x07 (BEL), sums the bytes mod 256, then sends
 *
 *     "FDFILEBLOCK,%s,%d,%d,%ld," (name, BYTES READ, block, file size)
 *     + the raw bytes (CR -> BEL, LF kept, commas kept)
 *     + "%02X\r"                  (the sum, upper-case hex)
 *
 * A missing file gives "FDFILEBLOCK,,0,0,0,00". The last block is short
 * (bytes read < 1024) or empty. The two corrections from the live machine:
 * the block frame arrives in PLAINTEXT even on 6.0.644170 - only the
 * dryer's formatted frames (STAT, FDFILELIST, ...) are encoded - and the
 * field order is bytes-then-block, not block-then-bytes. The lone "A1" that
 * an earlier live FILEREAD seemed to answer with (PROTOCOL_NOTES.md) was
 * this trailer, arriving after the data had been shredded by a CR/LF line
 * parser.
 *
 * Because the data carries LF and BEL and can exceed HR_MAX_FRAME, the
 * ordinary line reassembler cannot carry a block: hr_stream_t grows a
 * "big frame" side buffer (hr_stream_set_big) that collects a block frame
 * whole, on both the plaintext and the 6.0.644170 encoded transport.
 */
#ifndef HR_FILES_H
#define HR_FILES_H

#include "hr_protocol.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The dryer's block size: mov.w r2, 0x400 at 0x20f4e. */
#define HR_FDBLOCK_SIZE 1024
/* Longest file name we accept from the dryer (FatFs LFN on its side is
 * longer; the batch CSVs are 24 characters). */
#define HR_FILES_NAME_MAX 48
/* "FDFILEBLOCK," + name + ",%d,%d,%ld," */
#define HR_FDBLOCK_HDR_MAX (12 + HR_FILES_NAME_MAX + 36)
/* A whole plaintext block frame without its CR: header + data + "XX". */
#define HR_FDBLOCK_FRAME_MAX (HR_FDBLOCK_HDR_MAX + HR_FDBLOCK_SIZE + 2)
/*
 * The side buffer a block needs. On 6.0.644170 the frame arrives base64 in
 * the ")S" envelope: 4 + ceil((3 nonce + HR_FDBLOCK_FRAME_MAX + 1 CR) / 3) * 4
 * = 4 + 1504 = 1508 characters for a full block; the plaintext form is
 * smaller. Rounded up.
 */
#define HR_FILES_BIGBUF 1600

/* ------------------------------------------------------------------ */
/* FDFILELIST                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    char name[HR_FILES_NAME_MAX];
    long index;
    long size;
    bool end;   /* the "NULL" entry: no file at this index */
} hr_fdlist_entry_t;

/* Parse an FDFILELIST frame. False if it is not one or is malformed. */
bool hr_fdlist_parse(const hr_frame_t *f, hr_fdlist_entry_t *e);

/* ------------------------------------------------------------------ */
/* FDFILEBLOCK                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    char name[HR_FILES_NAME_MAX];
    long block;         /* block number echoed by the dryer */
    long nbytes;        /* bytes of data in this block, 0..1024 */
    long size;          /* whole file size as the dryer reports it */
    const char *data;   /* points into the caller's frame; nbytes long */
    unsigned sum;       /* checksum carried by the frame */
    bool sum_ok;        /* sum of data bytes mod 256 == sum */
} hr_fdblock_t;

/*
 * Is `buf` (the first `len` bytes of a plaintext line, no terminator) the
 * complete header of an FDFILEBLOCK frame ("FDFILEBLOCK,<name>,<nbytes>,
 * <block>,<size>,")?
 *   > 0  header complete, this many bytes; *nbytes = declared data length
 *   0    could still become one; need more bytes
 *  -1    not an FDFILEBLOCK header (or one with impossible numbers)
 * Cheap enough to call on every comma while a line is being collected.
 */
int hr_fdblock_header_len(const char *buf, size_t len, long *nbytes);

/*
 * Parse a whole block frame (header + data + 2 hex digits, no CR). Fails on
 * a malformed header or when the frame is not exactly header + nbytes + 2
 * long. A checksum mismatch is NOT a failure: `sum_ok` says, and the caller
 * decides (retry the block).
 */
bool hr_fdblock_parse(const char *frame, size_t len, hr_fdblock_t *out);

/* Undo the dryer's CR -> BEL substitution in place. Returns n. */
size_t hr_fdblock_unbel(char *data, size_t n);

/* ------------------------------------------------------------------ */
/* Transfer state machine                                              */
/* ------------------------------------------------------------------ */
typedef enum {
    HR_FILES_IDLE = 0,
    HR_FILES_LISTING,
    HR_FILES_READING,
    HR_FILES_DONE,
    HR_FILES_ERROR,
} hr_files_state_t;

typedef enum {
    HR_FILES_ERR_NONE = 0,
    HR_FILES_ERR_TIMEOUT,    /* no reply after the retries */
    HR_FILES_ERR_LINK,       /* link went down mid-transfer */
    HR_FILES_ERR_CANCEL,     /* hr_files_cancel() */
    HR_FILES_ERR_CHECKSUM,   /* same block failed its sum every retry */
    HR_FILES_ERR_SINK,       /* the block consumer refused (no storage) */
    HR_FILES_ERR_SEND,       /* the transport refused the request */
    HR_FILES_ERR_TOO_BIG,    /* file larger than the caller's limit */
    HR_FILES_ERR_NOTFOUND,   /* FDFILEBLOCK with an empty name and size 0 */
    HR_FILES_ERR_MISMATCH,   /* block for another name or block number */
    HR_FILES_ERR_BUSY,       /* a transfer is already running */
} hr_files_err_t;

/* Sink verdicts. */
#define HR_FILES_SINK_OK    0   /* stored; ask for the next block */
#define HR_FILES_SINK_WAIT  1   /* taken; call hr_files_resume() when stored */
#define HR_FILES_SINK_FAIL  2   /* cannot store; abort with ERR_SINK */

/* Send "<verb> <a1> <a2>" (a2 may be NULL). True if it went out. */
typedef bool (*hr_files_send_fn)(const char *verb, const char *a1,
                                 const char *a2, void *user);
/* A verified block, in order, exactly once. Data is still BEL-substituted. */
typedef int (*hr_files_sink_fn)(const hr_fdblock_t *b, void *user);
/* One list entry; the final call has e->end == true. */
typedef void (*hr_files_list_fn)(const hr_fdlist_entry_t *e, void *user);
/* Transfer finished, well or badly. */
typedef void (*hr_files_done_fn)(hr_files_state_t st, hr_files_err_t err,
                                 void *user);

/* Defaults. The dryer answered STATUS in 79-100 ms; a block is a FatFs read
 * plus ~1.1 KB over full-speed USB, well under a second. */
#define HR_FILES_TIMEOUT_MS   3000UL
#define HR_FILES_RETRIES      3
#define HR_FILES_LIST_MAX     64      /* entries we will ask for */
#define HR_FILES_MAX_SIZE     (512L * 1024L)
/* Most FILEREADs the machine will have unanswered at once (see `depth`). */
#define HR_FILES_DEPTH_MAX    4

typedef struct {
    hr_files_state_t state;
    hr_files_err_t err;

    char pattern[16];
    char name[HR_FILES_NAME_MAX];
    long index;         /* next list index to ask for */
    long block;         /* next block EXPECTED (accepted so far = block) */
    long next_req;      /* next block to ASK for; > block while pipelining */
    long size;          /* file size (from the list, then from blocks) */
    long received;      /* data bytes accepted so far */
    unsigned entries;   /* list entries received */
    int retries;        /* of the request in flight */
    int inflight;       /* FILEREADs out and unanswered, 0..depth */
    bool eof;           /* the last block has been accepted */
    bool first_sent;    /* the transfer's first request has gone out */

    bool awaiting;      /* a request is out, waiting for its reply (inflight > 0) */
    bool paused;        /* sink said WAIT; resume() clears */
    bool pending;       /* a request is due; tick() sends it */
    unsigned long sent_ms;      /* oldest unanswered request (or last event) */
    unsigned long last_send_ms; /* most recent request, for `gap_ms` */
    unsigned long paused_ms;
    unsigned long started_ms;
    unsigned long finished_ms;

    unsigned long timeout_ms;
    int max_retries;
    long max_size;
    /*
     * Send the next request from on_frame()/on_block()/resume() as soon as
     * the reply is in, instead of waiting for the next tick(). Off by
     * default (tick-paced, one request per caller tick); on, a transfer
     * runs at the dryer's own reply latency (~70-100 ms a block, measured).
     * The caller's send callback then runs on whichever task feeds the
     * session - the same task that already answers REQINFO with WIFIINFO.
     */
    bool send_inline;
    /*
     * PIPELINING. How many FILEREADs may be unanswered at once, 1 (the
     * default: strictly request-reply) to HR_FILES_DEPTH_MAX. With 2 the
     * request for block n+1 is on the wire while the dryer is still sending
     * block n. Only blocks below ceil(size/1024) are ever asked for, so the
     * depth is 1 until the size is known (from the listing or the first
     * block). Replies are accepted in order only: a block other than the
     * expected one (lost request, duplicate) resets the window and re-asks
     * from the expected block. Listing is never pipelined.
     */
    int depth;
    /* Minimum spacing between two requests, ms; 0 = as fast as replies come.
     * A caller that wants to go easy on the dryer's link sets this; requests
     * held back by it go out from tick(). */
    unsigned long gap_ms;

    /* Per-transfer timing, for the honest "where did the time go" line:
     * t_first_ms - start_read() to the first request on the wire;
     * t_dryer_ms - sum of request->reply waits (exact at depth 1);
     * t_paused_ms - sum of time the sink held the machine (WAIT..resume). */
    unsigned long t_first_ms;
    unsigned long t_dryer_ms;
    unsigned long t_paused_ms;

    hr_files_send_fn send;
    void *send_user;
    hr_files_sink_fn sink;
    void *sink_user;
    hr_files_list_fn list;
    void *list_user;
    hr_files_done_fn done;
    void *done_user;

    /* lifetime counters */
    unsigned long requests;
    unsigned long timeouts;
    unsigned long blocks_ok;
    unsigned long blocks_bad;   /* checksum failures */
    unsigned long transfers;
} hr_files_t;

void hr_files_init(hr_files_t *fs, hr_files_send_fn send, void *user);
void hr_files_set_sink(hr_files_t *fs, hr_files_sink_fn fn, void *user);
void hr_files_set_list_cb(hr_files_t *fs, hr_files_list_fn fn, void *user);
void hr_files_set_done_cb(hr_files_t *fs, hr_files_done_fn fn, void *user);

/* True when a transfer is in progress (LISTING or READING). */
bool hr_files_busy(const hr_files_t *fs);

/*
 * Start enumerating files whose name contains `pattern` (".csv", ".dat";
 * "" for everything). The first request goes out on the next tick(). False
 * (and ERR_BUSY) if a transfer is running.
 */
bool hr_files_start_list(hr_files_t *fs, const char *pattern,
                         unsigned long now_ms);

/*
 * Start reading `name` block by block. `expected_size` from the listing, or
 * -1 if unknown; a file over max_size is refused up front (and, if the dryer
 * reports a bigger size in the first block, aborted then).
 */
bool hr_files_start_read(hr_files_t *fs, const char *name, long expected_size,
                         unsigned long now_ms);

/* Abort whatever is running. `why` is reported to the done callback. */
void hr_files_cancel(hr_files_t *fs, hr_files_err_t why, unsigned long now_ms);

/* Feed an ordinary inbound frame; only FDFILELIST is looked at. */
void hr_files_on_frame(hr_files_t *fs, const hr_frame_t *f,
                       unsigned long now_ms);

/* Feed a whole plaintext FDFILEBLOCK frame (from the stream's big buffer). */
void hr_files_on_block(hr_files_t *fs, const char *frame, size_t len,
                       unsigned long now_ms);

/* The sink has stored the block it answered WAIT for. */
void hr_files_resume(hr_files_t *fs, unsigned long now_ms);

/*
 * Drive requests, timeouts and the link rule. Call regularly (the main loop's
 * 250 ms tick is fine). Requests are only ever sent from here, so a caller
 * that wants to keep the wire quiet simply stops ticking.
 */
void hr_files_tick(hr_files_t *fs, unsigned long now_ms, bool link_up);

/*
 * Put the pending request(s) on the wire NOW, from the caller's own task -
 * what tick() would do at its next call, minus timeouts and the link rule.
 * For the first request of a transfer: a main loop that has just started a
 * flash write can be seconds away from its next tick, and a 6 KB read
 * measured 6.2 s of which 5.2 s were spent waiting for that first tick.
 */
void hr_files_kick(hr_files_t *fs, unsigned long now_ms);

/* Set the pipelining depth (clamped to 1..HR_FILES_DEPTH_MAX) and the
 * minimum request spacing. Safe at any time; takes effect at the next send. */
void hr_files_set_pacing(hr_files_t *fs, int depth, unsigned long gap_ms);

/* 0..100 for a read with a known size, -1 otherwise. */
int hr_files_progress_pct(const hr_files_t *fs);

const char *hr_files_state_str(hr_files_state_t st);
const char *hr_files_err_str(hr_files_err_t err);

/* ------------------------------------------------------------------ */
/* Batch CSV                                                           */
/* ------------------------------------------------------------------ */
/*
 * The dryer's per-batch log (writer at 0x2bfa0 / 0x2c2cc in the image; read
 * live from a 6.0.644170 machine, 276 KB, 2986 rows):
 *
 *   header  "<batch name>,TStamp,mTorr,HtrReq,HtrOn,Top-J20,Mid-J17,Bot-J19,
 *            Room-J18,Process,Master,TTT,HPP,mT~Hr,mT~Mid,LowF,<firmware>,
 *            SN=<n>,<settings...>,<machine id>,"
 *           (two more thermocouple orders exist in the image: "Mid-J17,
 *           Bot-J19,J20,Room-J18" and "Trays-J17,J19,J20,Room-J18"; the
 *           tail after the column names is metadata, not columns)
 *   row     "11,M/D/YYYY H:MM,<mTorr>,<HtrReq>,<HtrOn>,<t>,<t>,<t>,<t>,<Process>,..."
 *           a constant state code, the dryer's clock, mTorr (capped 500000),
 *           HtrReq / HtrOn 0..150, four thermocouples in the header's order
 *           ("--" when absent), the phase name, then free-form columns whose
 *           count varies by phase. Lines end "\r\n"; one row a minute.
 *
 * Column positions are therefore taken from the header BY NAME, not assumed.
 * Temperatures are whole degrees F (the dryer's unit everywhere else);
 * pressure is mTorr = microns, uncapped (60245 at atmosphere).
 */
#define HR_CSV_COLS_MAX 24

typedef struct {
    int ncols;
    int tstamp;     /* column indexes, -1 when absent */
    int mtorr;
    int htrreq;
    int htron;
    int top;        /* J20 */
    int mid;        /* J17 - the shelf/trays thermocouple STAT reports */
    int bot;        /* J19 */
    int room;       /* J18 */
    int process;
} hr_csv_cols_t;

typedef struct {
    long counter;
    int month, day, year, hour, minute;   /* from TStamp; 0 when unparsed */
    bool have_time;
    long mtorr;                            /* -1 when absent */
    int htrreq, htron;                     /* -1 when absent */
    int top_f, mid_f, bot_f, room_f;       /* HR_CSV_NO_TEMP when absent/"--" */
    const char *process;                   /* pointer into the line; may be "" */
} hr_csv_row_t;

#define HR_CSV_NO_TEMP (-999)

/* Read the header line (no terminator). False if it has no TStamp column. */
bool hr_csv_header_parse(const char *line, hr_csv_cols_t *cols);

/*
 * Parse one data row against the header. `line` is modified in place (commas
 * become NULs). False for blank lines and lines with too few columns.
 */
bool hr_csv_row_parse(char *line, const hr_csv_cols_t *cols, hr_csv_row_t *row);

/* Minutes since 00:00 1/1/2000 for a row with a time, else -1. Good enough
 * to space rows on a chart; not a calendar library. */
long hr_csv_row_minutes(const hr_csv_row_t *row);

#ifdef __cplusplus
}
#endif

#endif /* HR_FILES_H */
