/*
 * hr_files - reading the dryer's own files: the FDFILES / FILEREAD client.
 *
 * The dryer keeps a CSV log of every batch on its internal drive, a minute a
 * row, including the ambient thermocouple that no live frame carries. It will
 * hand those files over if asked, one kilobyte at a time. Nothing here can
 * change the machine: both verbs read.
 *
 * Pure C11, no ESP-IDF; the ESP side lives in main/hr_dryerfiles.[ch].
 *
 * THE WIRE (confirmed live on 6.0.644170, 2026-09-17, reported in PR #11;
 * our own 6.0.641041 capture agrees on the listing half)
 *
 *   FDFILES <pattern> <index>
 *       -> FDFILELIST,<name>,<index>,<size>         the index-th match
 *       -> FDFILELIST,NULL,<index>,0                past the last match
 *
 *   FILEREAD <name> <block>
 *       -> FDFILEBLOCK,<name>,<bytes>,<block>,<size>,<data...>XX
 *
 * Both requests are ordinary space-delimited, CR-terminated frames and go out
 * in plaintext on either firmware; a 6.0.644170 machine encodes only what it
 * sends. The dryer matches <pattern> anywhere in the name and answers with one
 * entry per request, so a listing is a walk: ask for 0, 1, 2 ... until the
 * NULL entry. <block> is an offset in kilobytes; a block carries up to 1024
 * bytes and a short one means end of file.
 *
 * A block frame is not a line. Its data carry LF, commas, and any byte the
 * file happens to hold, with the file's own CRs sent as BEL (0x07) so they do
 * not end the frame early, and the whole thing runs past HR_MAX_FRAME. It is
 * therefore collected whole in a side buffer the owner lends the stream
 * (hr_stream_set_long), not by the line reassembler - which is what shredded
 * it into rubbish and a stray "A1" when FILEREAD was first tried. "A1" was the
 * checksum: two upper-case hex digits, the data bytes summed mod 256, that
 * close every block frame.
 *
 * WHAT WE REFUSE TO SEND, AND WHY
 *
 * The dryer picks a command by searching the whole line for a verb, in its own
 * order, and acts on the first hit - so text we put in an ARGUMENT can be read
 * as a command. A carriage return is worse: it ends our frame and turns the
 * rest into a second command. hr_files_arg_ok() is the one gate every name and
 * pattern passes before it reaches the wire.
 */
#ifndef HR_FILES_H
#define HR_FILES_H

#include "hr_protocol.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The dryer's block size, and the longest name we will handle. */
#define HR_FILE_BLOCK      1024
#define HR_FILE_NAME_MAX   48
/* Frame overhead: the verb, four fields, the separators and the checksum. */
#define HR_FILE_HDR_MAX    (12 + HR_FILE_NAME_MAX + 36)
#define HR_FILE_FRAME_MAX  (HR_FILE_HDR_MAX + HR_FILE_BLOCK + 2)
/*
 * The side buffer a whole block needs. On 6.0.644170 it arrives base64 in the
 * ")S" envelope, four characters per three bytes plus the header, so the
 * encoded form is the larger: 4 + ceil((3 + HR_FILE_FRAME_MAX + 1) / 3) * 4,
 * rounded up.
 */
#define HR_FILE_LONGBUF    1600
/* Biggest file we will fetch. The longest batch log seen is 276 KB. */
#define HR_FILE_SIZE_MAX   (512L * 1024L)

/* ------------------------------------------------------------------ */
/* What we are willing to say to the dryer                             */
/* ------------------------------------------------------------------ */
/*
 * True if `s` is safe to send as an argument of FDFILES or FILEREAD.
 *
 * Required: 1..HR_FILE_NAME_MAX-1 printable characters, none of them a space,
 * quote, comma, slash, backslash or wildcard, and no dryer verb appearing
 * anywhere inside (see hr_files_arg_verb). A leading dot is fine here - ".csv"
 * is the usual pattern - but hr_files_read() refuses one, since a name that
 * begins with a dot cannot have come from a listing.
 *
 * This is why: a control character would end our frame and make the remainder
 * a command of the sender's choosing, and a verb inside an argument is matched
 * before the verb we meant, because the dryer searches the line rather than
 * comparing the first word.
 */
bool hr_files_arg_ok(const char *s);

/*
 * The verb `s` would collide with, or NULL if none does. Exposed so callers
 * can say WHICH word was the problem instead of "invalid name".
 */
const char *hr_files_arg_verb(const char *s);

/* ------------------------------------------------------------------ */
/* Replies                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    char name[HR_FILE_NAME_MAX];
    long index;      /* the index we asked for, echoed */
    long size;       /* bytes, as the dryer reports them */
    bool last;       /* the NULL entry: nothing at this index */
} hr_file_entry_t;

/* Parse an FDFILELIST frame. False if it is not one, or is malformed. */
bool hr_file_entry_parse(const hr_frame_t *f, hr_file_entry_t *e);

typedef struct {
    char name[HR_FILE_NAME_MAX];
    long block;        /* block number, echoed */
    long nbytes;       /* data bytes in this block, 0..HR_FILE_BLOCK */
    long size;         /* whole-file size */
    const char *data;  /* into the caller's frame; nbytes long */
    unsigned sum;      /* the checksum the frame carried */
    bool sum_ok;       /* the data actually sum to it */
    bool missing;      /* the dryer's "no such file" answer */
} hr_file_block_t;

/*
 * Measure a block frame from the head of a line, for hr_stream_set_long().
 *
 * `buf`/`len` are the bytes of a plaintext line so far, no terminator.
 *   1   this is a block frame; *total is its whole length, header included
 *   0   might still become one; ask again when more has arrived
 *  -1   not a block frame
 * Matches the signature hr_long_measure_fn expects (hr_protocol.h).
 */
int hr_file_block_measure(const char *buf, size_t len, size_t *total);

/*
 * Parse a whole block frame (header + data + two checksum digits, no CR).
 * False on a malformed frame or a length that disagrees with the header. A
 * checksum MISMATCH is not a failure: sum_ok reports it and the caller
 * decides, because the fix is to ask for the block again.
 */
bool hr_file_block_parse(const char *frame, size_t len, hr_file_block_t *out);

/*
 * Put the file's own carriage returns back: the dryer sends each 0x0D as BEL
 * so it cannot end the frame. In place; returns n.
 */
size_t hr_file_restore_cr(char *data, size_t n);

/* ------------------------------------------------------------------ */
/* One transfer at a time                                              */
/* ------------------------------------------------------------------ */
typedef enum {
    HR_FILES_IDLE = 0,
    HR_FILES_LISTING,
    HR_FILES_READING,
    HR_FILES_DONE,
    HR_FILES_FAILED,
} hr_files_state_t;

typedef enum {
    HR_FILES_OK = 0,
    HR_FILES_ERR_TIMEOUT,   /* the dryer stopped answering */
    HR_FILES_ERR_LINK,      /* the USB link went down under us */
    HR_FILES_ERR_CHECKSUM,  /* a block kept arriving corrupt */
    HR_FILES_ERR_MISSING,   /* the dryer has no such file */
    HR_FILES_ERR_TOO_BIG,   /* larger than HR_FILE_SIZE_MAX */
    HR_FILES_ERR_SINK,      /* the consumer stopped taking the bytes */
    HR_FILES_ERR_SEND,      /* the frame could not be handed to the link */
    HR_FILES_ERR_CANCELLED,
} hr_files_err_t;

/* Hand one request to the link. False if it could not be sent. */
typedef bool (*hr_files_send_fn)(const char *verb, const char *arg,
                                 const char *index, void *user);
/* One listing entry; `last` marks the end of the walk. */
typedef void (*hr_files_entry_fn)(const hr_file_entry_t *e, void *user);
/*
 * Data from one block, in file order, CRs already restored. Return false to
 * say "not now" - the transfer pauses and resumes at hr_files_resume().
 */
typedef bool (*hr_files_data_fn)(const char *data, size_t n, void *user);
/* The transfer ended, one way or the other. */
typedef void (*hr_files_done_fn)(hr_files_state_t st, hr_files_err_t err,
                                 void *user);

/* No answer for this long and the request is repeated; after this many
 * repeats the transfer fails. Deliberately generous: a block takes the dryer
 * about 170 ms, but it answers a batch's STAT first. */
#define HR_FILES_REPLY_MS   3000UL
#define HR_FILES_RETRIES    3

typedef struct {
    hr_files_state_t state;
    hr_files_err_t err;

    char arg[HR_FILE_NAME_MAX];   /* the pattern, or the file name */
    long index;                   /* listing: which entry we are asking for */
    long block;                   /* reading: which block we are asking for */
    long size;                    /* file size once known, else -1 */
    long received;                /* bytes handed to the consumer */
    long entries;                 /* listing: entries seen */

    bool waiting;                 /* a request is out and unanswered */
    bool paused;                  /* the consumer said "not now" */
    bool pending;                 /* a request is due to go out */
    int retries;
    unsigned long sent_ms;        /* when the outstanding request went */
    unsigned long started_ms;

    /* Since boot, for the status page. */
    unsigned long requests;
    unsigned long timeouts;
    unsigned long blocks_ok;
    unsigned long blocks_bad;
    unsigned long transfers;

    hr_files_send_fn send;
    void *send_user;
    hr_files_entry_fn on_entry;
    void *entry_user;
    hr_files_data_fn on_data;
    void *data_user;
    hr_files_done_fn on_done;
    void *done_user;
} hr_files_t;

void hr_files_init(hr_files_t *fs, hr_files_send_fn send, void *user);
void hr_files_set_entry_cb(hr_files_t *fs, hr_files_entry_fn fn, void *user);
void hr_files_set_data_cb(hr_files_t *fs, hr_files_data_fn fn, void *user);
void hr_files_set_done_cb(hr_files_t *fs, hr_files_done_fn fn, void *user);

/* Busy = a request is in flight or due. */
bool hr_files_busy(const hr_files_t *fs);

/*
 * Start a listing or a read. False if one is already running, or if the
 * argument is not something we will send (hr_files_arg_ok) - for a read,
 * also if `size` is larger than we will fetch.
 */
bool hr_files_list(hr_files_t *fs, const char *pattern, unsigned long now_ms);
bool hr_files_read(hr_files_t *fs, const char *name, long size,
                   unsigned long now_ms);

/* Give up on the current transfer. */
void hr_files_cancel(hr_files_t *fs, unsigned long now_ms);

/* The consumer has room again after refusing data. */
void hr_files_resume(hr_files_t *fs, unsigned long now_ms);

/*
 * Feed a decoded frame. Ignores everything that is not an answer we are
 * waiting for.
 */
void hr_files_on_frame(hr_files_t *fs, const hr_frame_t *f,
                       unsigned long now_ms);

/*
 * Feed a whole block frame from the stream's side buffer (hr_long_cb), still
 * in wire form. Written to in place - the file's carriage returns are put
 * back where the dryer substituted BEL - so it must be the caller's own
 * buffer, which is exactly what the side buffer is.
 */
void hr_files_on_block(hr_files_t *fs, char *frame, size_t len,
                       unsigned long now_ms);

/*
 * Drive timeouts and send whatever is due. Call it regularly and whenever a
 * reply has been handled; `link_up` false ends a transfer at once.
 */
void hr_files_tick(hr_files_t *fs, unsigned long now_ms, bool link_up);

/* Short words for the API and the log. Never NULL. */
const char *hr_files_state_str(hr_files_state_t st);
const char *hr_files_err_str(hr_files_err_t e);

#ifdef __cplusplus
}
#endif

#endif /* HR_FILES_H */
