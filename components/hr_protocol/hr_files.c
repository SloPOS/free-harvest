/*
 * hr_files - see hr_files.h. Reads only; nothing here can change the dryer.
 */
#include "hr_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_VERB "FDFILEBLOCK,"
#define BLOCK_VERB_LEN 12

static void maybe_send(hr_files_t *fs, unsigned long now_ms);

/* ------------------------------------------------------------------ */
/* FDFILELIST                                                          */
/* ------------------------------------------------------------------ */
bool hr_fdlist_parse(const hr_frame_t *f, hr_fdlist_entry_t *e)
{
    if (f == NULL || e == NULL || strcmp(f->verb, "FDFILELIST") != 0) {
        return false;
    }
    /* FDFILELIST,%s,%d,%ld - three fields; a trailing empty one is
     * tolerated (the dryer's other frames end in a comma, this one does not). */
    if (f->nfields < 3) {
        return false;
    }
    const char *name = hr_frame_field(f, 0);
    memset(e, 0, sizeof(*e));
    if (name == NULL) {
        return false;
    }
    if (strlen(name) >= sizeof(e->name)) {
        return false;
    }
    strcpy(e->name, name);
    e->index = hr_frame_field_int(f, 1, -1);
    e->size = hr_frame_field_int(f, 2, -1);
    if (e->index < 0 || e->size < 0) {
        return false;
    }
    /* The directory walk hands back "NULL" (0x84980) when nothing matched
     * the index. An empty name is treated the same way. */
    e->end = (e->name[0] == '\0' || strcmp(e->name, "NULL") == 0);
    return true;
}

/* ------------------------------------------------------------------ */
/* FDFILEBLOCK                                                         */
/* ------------------------------------------------------------------ */
/* Decimal field between buf[from] and the next comma. Returns the index of
 * that comma, or -1 when the field is not a plain integer. */
static int int_field(const char *buf, size_t len, size_t from, long *out)
{
    size_t i = from;
    bool neg = false;
    long v = 0;
    if (i < len && buf[i] == '-') {
        neg = true;
        i++;
    }
    size_t digits = 0;
    for (; i < len && buf[i] != ','; i++) {
        if (buf[i] < '0' || buf[i] > '9' || digits >= 10) {
            return -1;
        }
        v = v * 10 + (buf[i] - '0');
        digits++;
    }
    if (i >= len || digits == 0) {
        return -1;
    }
    *out = neg ? -v : v;
    return (int)i;
}

int hr_fdblock_header_len(const char *buf, size_t len, long *nbytes)
{
    if (buf == NULL) {
        return -1;
    }
    /* Prefix check on what has arrived so far, so a line that is not a block
     * is rejected on its first differing byte. */
    size_t cmp = len < BLOCK_VERB_LEN ? len : BLOCK_VERB_LEN;
    if (memcmp(buf, BLOCK_VERB, cmp) != 0) {
        return -1;
    }
    if (len < BLOCK_VERB_LEN) {
        return 0;
    }
    /* name */
    size_t i = BLOCK_VERB_LEN;
    while (i < len && buf[i] != ',') {
        i++;
    }
    if (i >= len) {
        return (i - BLOCK_VERB_LEN >= HR_FILES_NAME_MAX) ? -1 : 0;
    }
    if (i - BLOCK_VERB_LEN >= HR_FILES_NAME_MAX) {
        return -1;
    }
    /* nbytes, block, size - each needs its terminating comma present.
     * THIS ORDER IS CONFIRMED LIVE (2026-09-17, 6.0.644170): the dryer
     * writes "FDFILEBLOCK,%s,%d,%d,%ld," as (name, bytes read, block, size)
     * - sendFileBlock at 0x20f30 passes r3 = bytes read, [sp] = block. */
    long block, n, size;
    int c = int_field(buf, len, i + 1, &n);
    if (c < 0) {
        /* either not there yet or not a number: look for the comma */
        return (memchr(buf + i + 1, ',', len - i - 1) == NULL) ? 0 : -1;
    }
    c = int_field(buf, len, (size_t)c + 1, &block);
    if (c < 0) {
        size_t from = (size_t)i + 1;
        const char *p = memchr(buf + from, ',', len - from);
        if (p == NULL) {
            return 0;
        }
        from = (size_t)(p - buf) + 1;
        return (memchr(buf + from, ',', len - from) == NULL) ? 0 : -1;
    }
    size_t after_n = (size_t)c + 1;
    c = int_field(buf, len, after_n, &size);
    if (c < 0) {
        return (memchr(buf + after_n, ',', len - after_n) == NULL) ? 0 : -1;
    }
    if (block < 0 || n < 0 || n > HR_FDBLOCK_SIZE || size < 0) {
        return -1;
    }
    if (nbytes != NULL) {
        *nbytes = n;
    }
    return c + 1; /* header ends just after the fifth comma */
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool hr_fdblock_parse(const char *frame, size_t len, hr_fdblock_t *out)
{
    if (frame == NULL || out == NULL) {
        return false;
    }
    long n = 0;
    int hdr = hr_fdblock_header_len(frame, len, &n);
    if (hdr <= 0) {
        return false;
    }
    if (len != (size_t)hdr + (size_t)n + 2) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* name: between the first and second comma */
    const char *p = frame + BLOCK_VERB_LEN;
    const char *q = memchr(p, ',', len - BLOCK_VERB_LEN);
    size_t nl = (size_t)(q - p);
    memcpy(out->name, p, nl);
    out->name[nl] = '\0';

    long block = 0, size = 0;
    int c = int_field(frame, len, (size_t)(q - frame) + 1, &n);
    c = int_field(frame, len, (size_t)c + 1, &block);
    c = int_field(frame, len, (size_t)c + 1, &size);
    (void)c;
    out->block = block;
    out->nbytes = n;
    out->size = size;
    out->data = frame + hdr;

    int h1 = hexval(frame[len - 2]);
    int h2 = hexval(frame[len - 1]);
    if (h1 < 0 || h2 < 0) {
        return false;
    }
    out->sum = (unsigned)(h1 * 16 + h2);

    unsigned acc = 0;
    for (long i = 0; i < n; i++) {
        acc += (unsigned char)out->data[i];
    }
    out->sum_ok = ((acc & 0xffu) == out->sum);
    return true;
}

size_t hr_fdblock_unbel(char *data, size_t n)
{
    if (data == NULL) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\a') {
            data[i] = '\r';
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */
static void finish(hr_files_t *fs, hr_files_state_t st, hr_files_err_t err,
                   unsigned long now)
{
    fs->state = st;
    fs->err = err;
    fs->awaiting = false;
    fs->inflight = 0;
    fs->pending = false;
    if (fs->paused) {
        fs->t_paused_ms += now - fs->paused_ms;
    }
    fs->paused = false;
    fs->finished_ms = now;
    if (fs->done != NULL) {
        fs->done(st, err, fs->done_user);
    }
}

/* Forget the requests in flight and ask again from the expected block. */
static void rewind_window(hr_files_t *fs)
{
    fs->inflight = 0;
    fs->awaiting = false;
    fs->next_req = fs->block;
    fs->pending = true;
}

/* Blocks the file has, once the size is known; LONG_MAX-ish otherwise. */
static long block_limit(const hr_files_t *fs)
{
    if (fs->size < 0) {
        return -1;
    }
    return (fs->size + HR_FDBLOCK_SIZE - 1) / HR_FDBLOCK_SIZE;
}

void hr_files_init(hr_files_t *fs, hr_files_send_fn send, void *user)
{
    if (fs == NULL) {
        return;
    }
    memset(fs, 0, sizeof(*fs));
    fs->send = send;
    fs->send_user = user;
    fs->timeout_ms = HR_FILES_TIMEOUT_MS;
    fs->max_retries = HR_FILES_RETRIES;
    fs->max_size = HR_FILES_MAX_SIZE;
    fs->depth = 1;
    fs->gap_ms = 0;
    fs->state = HR_FILES_IDLE;
}

void hr_files_set_pacing(hr_files_t *fs, int depth, unsigned long gap_ms)
{
    if (fs == NULL) {
        return;
    }
    if (depth < 1) {
        depth = 1;
    }
    if (depth > HR_FILES_DEPTH_MAX) {
        depth = HR_FILES_DEPTH_MAX;
    }
    fs->depth = depth;
    fs->gap_ms = gap_ms;
}

void hr_files_set_sink(hr_files_t *fs, hr_files_sink_fn fn, void *user)
{
    if (fs != NULL) {
        fs->sink = fn;
        fs->sink_user = user;
    }
}

void hr_files_set_list_cb(hr_files_t *fs, hr_files_list_fn fn, void *user)
{
    if (fs != NULL) {
        fs->list = fn;
        fs->list_user = user;
    }
}

void hr_files_set_done_cb(hr_files_t *fs, hr_files_done_fn fn, void *user)
{
    if (fs != NULL) {
        fs->done = fn;
        fs->done_user = user;
    }
}

bool hr_files_busy(const hr_files_t *fs)
{
    return fs != NULL &&
           (fs->state == HR_FILES_LISTING || fs->state == HR_FILES_READING);
}

bool hr_files_start_list(hr_files_t *fs, const char *pattern,
                         unsigned long now_ms)
{
    if (fs == NULL) {
        return false;
    }
    if (hr_files_busy(fs)) {
        fs->err = HR_FILES_ERR_BUSY;
        return false;
    }
    if (pattern == NULL) {
        pattern = "";
    }
    if (strlen(pattern) >= sizeof(fs->pattern) || strchr(pattern, ' ') ||
        strchr(pattern, '"')) {
        return false;
    }
    strcpy(fs->pattern, pattern);
    fs->name[0] = '\0';
    fs->index = 0;
    fs->entries = 0;
    fs->retries = 0;
    fs->awaiting = false;
    fs->inflight = 0;
    fs->eof = false;
    fs->first_sent = false;
    fs->paused = false;
    fs->pending = true;
    fs->started_ms = now_ms;
    fs->t_first_ms = fs->t_dryer_ms = fs->t_paused_ms = 0;
    fs->state = HR_FILES_LISTING;
    fs->err = HR_FILES_ERR_NONE;
    fs->transfers++;
    return true;
}

bool hr_files_start_read(hr_files_t *fs, const char *name, long expected_size,
                         unsigned long now_ms)
{
    if (fs == NULL || name == NULL || name[0] == '\0') {
        return false;
    }
    if (hr_files_busy(fs)) {
        fs->err = HR_FILES_ERR_BUSY;
        return false;
    }
    /* The name travels as one space-delimited argument; spaces and quotes
     * have no safe encoding, and a comma would corrupt the reply header. */
    if (strlen(name) >= sizeof(fs->name) || strpbrk(name, " \",\r\n") != NULL) {
        return false;
    }
    if (expected_size > fs->max_size) {
        fs->state = HR_FILES_ERROR;
        fs->err = HR_FILES_ERR_TOO_BIG;
        return false;
    }
    strcpy(fs->name, name);
    fs->block = 0;
    fs->next_req = 0;
    fs->size = expected_size;
    fs->received = 0;
    fs->retries = 0;
    fs->awaiting = false;
    fs->inflight = 0;
    fs->eof = false;
    fs->first_sent = false;
    fs->paused = false;
    fs->pending = true;
    fs->started_ms = now_ms;
    fs->t_first_ms = fs->t_dryer_ms = fs->t_paused_ms = 0;
    fs->state = HR_FILES_READING;
    fs->err = HR_FILES_ERR_NONE;
    fs->transfers++;
    return true;
}

void hr_files_cancel(hr_files_t *fs, hr_files_err_t why, unsigned long now_ms)
{
    if (fs == NULL || !hr_files_busy(fs)) {
        return;
    }
    finish(fs, HR_FILES_ERROR, why == HR_FILES_ERR_NONE ? HR_FILES_ERR_CANCEL : why,
           now_ms);
}

void hr_files_on_frame(hr_files_t *fs, const hr_frame_t *f, unsigned long now_ms)
{
    if (fs == NULL || f == NULL || fs->state != HR_FILES_LISTING) {
        return;
    }
    hr_fdlist_entry_t e;
    if (!hr_fdlist_parse(f, &e)) {
        return;
    }
    if (!fs->awaiting) {
        return; /* unsolicited or late; nothing to match it to */
    }
    fs->awaiting = false;
    if (e.end || e.index != fs->index) {
        /* Off the end - or the dryer answered a different index than asked,
         * which we treat as the end rather than guess. */
        e.end = true;
        if (fs->list != NULL) {
            fs->list(&e, fs->list_user);
        }
        finish(fs, HR_FILES_DONE, HR_FILES_ERR_NONE, now_ms);
        return;
    }
    fs->entries++;
    if (fs->list != NULL) {
        fs->list(&e, fs->list_user);
    }
    fs->index++;
    fs->retries = 0;
    if (fs->index >= HR_FILES_LIST_MAX) {
        hr_fdlist_entry_t end = {.end = true, .index = fs->index};
        if (fs->list != NULL) {
            fs->list(&end, fs->list_user);
        }
        finish(fs, HR_FILES_DONE, HR_FILES_ERR_NONE, now_ms);
        return;
    }
    fs->pending = true;
    if (fs->send_inline) {
        maybe_send(fs, now_ms);
    }
}

void hr_files_on_block(hr_files_t *fs, const char *frame, size_t len,
                       unsigned long now_ms)
{
    if (fs == NULL || frame == NULL || fs->state != HR_FILES_READING ||
        !fs->awaiting) {
        return;
    }
    hr_fdblock_t b;
    if (!hr_fdblock_parse(frame, len, &b)) {
        fs->blocks_bad++;
        return; /* unparsable: let the timeout re-ask */
    }

    /* "FDFILEBLOCK,,0,0,0,00": the dryer could not open the file. */
    if (b.name[0] == '\0' && b.size == 0 && b.nbytes == 0) {
        finish(fs, HR_FILES_ERROR, HR_FILES_ERR_NOTFOUND, now_ms);
        return;
    }
    if (strcmp(b.name, fs->name) != 0 || b.block != fs->block) {
        /*
         * Not the block we expect next: a stale reply, a duplicate, or -
         * while pipelining - the answer to a request the dryer skipped.
         * Whatever is in flight is now suspect: forget the window and ask
         * again from the expected block. Bounded, so a dryer answering
         * nonsense forever cannot keep the machine READING.
         */
        fs->blocks_bad++;
        if (++fs->retries > fs->max_retries * 3) {
            finish(fs, HR_FILES_ERROR, HR_FILES_ERR_MISMATCH, now_ms);
            return;
        }
        rewind_window(fs);
        if (fs->send_inline) {
            maybe_send(fs, now_ms);
        }
        return;
    }
    /* The reply we were waiting for. Account the wait to the dryer. */
    fs->t_dryer_ms += (long)(now_ms - fs->sent_ms) > 0 ? now_ms - fs->sent_ms : 0;
    if (fs->inflight > 0) {
        fs->inflight--;
    }
    fs->awaiting = (fs->inflight > 0);
    fs->sent_ms = now_ms; /* the next outstanding reply is timed from here */

    if (!b.sum_ok) {
        fs->blocks_bad++;
        if (++fs->retries > fs->max_retries) {
            finish(fs, HR_FILES_ERROR, HR_FILES_ERR_CHECKSUM, now_ms);
            return;
        }
        rewind_window(fs); /* same block again (and the window after it) */
        if (fs->send_inline) {
            maybe_send(fs, now_ms);
        }
        return;
    }
    fs->retries = 0;
    fs->blocks_ok++;
    if (b.size > fs->max_size) {
        finish(fs, HR_FILES_ERROR, HR_FILES_ERR_TOO_BIG, now_ms);
        return;
    }
    fs->size = b.size;

    int verdict = HR_FILES_SINK_OK;
    if (b.nbytes > 0 && fs->sink != NULL) {
        verdict = fs->sink(&b, fs->sink_user);
    }
    if (verdict == HR_FILES_SINK_FAIL) {
        finish(fs, HR_FILES_ERROR, HR_FILES_ERR_SINK, now_ms);
        return;
    }
    fs->received += b.nbytes;
    fs->block++;
    if (fs->next_req < fs->block) {
        fs->next_req = fs->block;
    }

    fs->eof = (b.nbytes < HR_FDBLOCK_SIZE) ||
              (fs->size > 0 && fs->received >= fs->size);
    if (verdict == HR_FILES_SINK_WAIT) {
        /*
         * No NEW request goes out until the sink has made room; replies
         * already in flight are still accepted (the sink promised room for
         * them when it chose a depth). With eof there is nothing more to
         * ask for: resume() then finishes.
         */
        if (!fs->paused) {
            fs->paused = true;
            fs->paused_ms = now_ms;
        }
        fs->pending = !fs->eof;
        return;
    }
    if (fs->eof) {
        finish(fs, HR_FILES_DONE, HR_FILES_ERR_NONE, now_ms);
        return;
    }
    fs->pending = true;
    if (fs->send_inline) {
        maybe_send(fs, now_ms);
    }
}

void hr_files_resume(hr_files_t *fs, unsigned long now_ms)
{
    if (fs == NULL || !fs->paused) {
        return;
    }
    fs->paused = false;
    fs->t_paused_ms += (long)(now_ms - fs->paused_ms) > 0
                           ? now_ms - fs->paused_ms : 0;
    if (fs->state == HR_FILES_READING && fs->eof && fs->inflight == 0) {
        /* the block the sink just stored was the last one */
        finish(fs, HR_FILES_DONE, HR_FILES_ERR_NONE, now_ms);
        return;
    }
    if (fs->send_inline) {
        maybe_send(fs, now_ms);
    }
}

static bool send_request(hr_files_t *fs, long block)
{
    char idx[16];
    bool ok;
    if (fs->state == HR_FILES_LISTING) {
        snprintf(idx, sizeof(idx), "%ld", fs->index);
        ok = fs->send != NULL &&
             fs->send("FDFILES", fs->pattern, idx, fs->send_user);
    } else {
        snprintf(idx, sizeof(idx), "%ld", block);
        ok = fs->send != NULL &&
             fs->send("FILEREAD", fs->name, idx, fs->send_user);
    }
    return ok;
}

/* One request on the wire; false (and the transfer failed) if it did not go. */
static bool put_request(hr_files_t *fs, long block, unsigned long now_ms)
{
    if (!fs->first_sent) {
        fs->first_sent = true;
        fs->t_first_ms = (long)(now_ms - fs->started_ms) > 0
                             ? now_ms - fs->started_ms : 0;
    }
    fs->requests++;
    if (!send_request(fs, block)) {
        finish(fs, HR_FILES_ERROR, HR_FILES_ERR_SEND, now_ms);
        return false;
    }
    if (fs->inflight == 0) {
        fs->sent_ms = now_ms; /* first of the window: timeouts count from it */
    }
    fs->inflight++;
    fs->awaiting = true;
    fs->last_send_ms = now_ms;
    return true;
}

/* Put the pending request(s) on the wire, as many as the depth, the file's
 * size and the pacing gap allow. */
static void maybe_send(hr_files_t *fs, unsigned long now_ms)
{
    if (!hr_files_busy(fs) || fs->paused || !fs->pending) {
        return;
    }
    if (fs->state == HR_FILES_LISTING) {
        if (fs->awaiting) {
            return;
        }
        if (fs->gap_ms && fs->requests &&
            (long)(now_ms - fs->last_send_ms) < (long)fs->gap_ms) {
            return; /* tick() will */
        }
        fs->pending = false;
        put_request(fs, 0, now_ms);
        return;
    }
    /* READING */
    if (fs->eof) {
        fs->pending = false;
        return;
    }
    while (fs->inflight < fs->depth) {
        const long limit = block_limit(fs);
        if (limit < 0) {
            /* size unknown: one at a time until the first block says */
            if (fs->inflight > 0) {
                break;
            }
        } else if (fs->next_req >= limit) {
            break; /* everything the file has is asked for */
        }
        if (fs->gap_ms && fs->requests &&
            (long)(now_ms - fs->last_send_ms) < (long)fs->gap_ms) {
            return; /* held back; tick() sends it, pending stays set */
        }
        if (!put_request(fs, fs->next_req, now_ms)) {
            return;
        }
        fs->next_req++;
    }
    /* pending stays true while there are blocks left to ask for */
    fs->pending = (block_limit(fs) < 0) ? (fs->inflight == 0)
                                        : (fs->next_req < block_limit(fs));
}

void hr_files_kick(hr_files_t *fs, unsigned long now_ms)
{
    if (fs == NULL) {
        return;
    }
    maybe_send(fs, now_ms);
}

void hr_files_tick(hr_files_t *fs, unsigned long now_ms, bool link_up)
{
    if (fs == NULL || !hr_files_busy(fs)) {
        return;
    }
    if (!link_up) {
        finish(fs, HR_FILES_ERROR, HR_FILES_ERR_LINK, now_ms);
        return;
    }
    if (fs->awaiting) {
        /* (long) so a tick clocked BEFORE an inline send - the caller's
         * loop read its clock, did other work, then ticked - reads as no
         * time passed instead of as an unsigned eternity: that exact
         * pattern re-asked one block in every twenty on the real board. */
        if ((long)(now_ms - fs->sent_ms) >= (long)fs->timeout_ms) {
            fs->timeouts++;
            if (++fs->retries > fs->max_retries) {
                finish(fs, HR_FILES_ERROR, HR_FILES_ERR_TIMEOUT, now_ms);
                return;
            }
            if (fs->state == HR_FILES_READING) {
                rewind_window(fs);
            } else {
                fs->awaiting = false;
                fs->inflight = 0;
                fs->pending = true;
            }
        } else if (fs->state == HR_FILES_READING && !fs->paused &&
                   fs->pending && fs->inflight < fs->depth) {
            maybe_send(fs, now_ms); /* a request the gap held back */
            return;
        } else {
            return;
        }
    }
    if (fs->paused) {
        /* A sink that never resumes must not leave the machine READING for
         * ever: storage that slow is storage that failed. */
        if ((long)(now_ms - fs->paused_ms) >= (long)(fs->timeout_ms * 4)) {
            finish(fs, HR_FILES_ERROR, HR_FILES_ERR_SINK, now_ms);
        }
        return;
    }
    maybe_send(fs, now_ms);
}

int hr_files_progress_pct(const hr_files_t *fs)
{
    if (fs == NULL || fs->state != HR_FILES_READING || fs->size <= 0) {
        return -1;
    }
    long pct = fs->received * 100 / fs->size;
    return pct > 100 ? 100 : (int)pct;
}

const char *hr_files_state_str(hr_files_state_t st)
{
    switch (st) {
    case HR_FILES_IDLE:    return "idle";
    case HR_FILES_LISTING: return "listing";
    case HR_FILES_READING: return "reading";
    case HR_FILES_DONE:    return "done";
    case HR_FILES_ERROR:   return "error";
    default:               return "?";
    }
}

const char *hr_files_err_str(hr_files_err_t err)
{
    switch (err) {
    case HR_FILES_ERR_NONE:     return "";
    case HR_FILES_ERR_TIMEOUT:  return "timeout";
    case HR_FILES_ERR_LINK:     return "link down";
    case HR_FILES_ERR_CANCEL:   return "cancelled";
    case HR_FILES_ERR_CHECKSUM: return "checksum";
    case HR_FILES_ERR_SINK:     return "storage";
    case HR_FILES_ERR_SEND:     return "send failed";
    case HR_FILES_ERR_TOO_BIG:  return "file too big";
    case HR_FILES_ERR_NOTFOUND: return "not found";
    case HR_FILES_ERR_MISMATCH: return "mismatch";
    case HR_FILES_ERR_BUSY:     return "busy";
    default:                    return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Batch CSV                                                           */
/* ------------------------------------------------------------------ */
static bool has(const char *s, const char *needle)
{
    return s != NULL && strstr(s, needle) != NULL;
}

/* Split `line` on commas in place; returns the count (capped). */
static int split(char *line, char **cols, int cap)
{
    int n = 0;
    char *p = line;
    while (n < cap) {
        cols[n++] = p;
        char *c = strchr(p, ',');
        if (c == NULL) {
            break;
        }
        *c = '\0';
        p = c + 1;
    }
    return n;
}

bool hr_csv_header_parse(const char *line, hr_csv_cols_t *cols)
{
    if (line == NULL || cols == NULL) {
        return false;
    }
    char tmp[512];
    size_t len = strlen(line);
    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
    }
    memcpy(tmp, line, len);
    tmp[len] = '\0';
    while (len > 0 && (tmp[len - 1] == '\r' || tmp[len - 1] == '\n')) {
        tmp[--len] = '\0';
    }

    char *c[HR_CSV_COLS_MAX];
    int n = split(tmp, c, HR_CSV_COLS_MAX);

    cols->ncols = n;
    cols->tstamp = cols->mtorr = cols->htrreq = cols->htron = -1;
    cols->top = cols->mid = cols->bot = cols->room = cols->process = -1;
    for (int i = 0; i < n; i++) {
        const char *h = c[i];
        if (cols->tstamp < 0 && strcmp(h, "TStamp") == 0) {
            cols->tstamp = i;
        } else if (cols->mtorr < 0 && strcmp(h, "mTorr") == 0) {
            cols->mtorr = i;
        } else if (cols->htrreq < 0 && strcmp(h, "HtrReq") == 0) {
            cols->htrreq = i;
        } else if (cols->htron < 0 && strcmp(h, "HtrOn") == 0) {
            cols->htron = i;
        } else if (cols->room < 0 && has(h, "Room")) {
            cols->room = i;
        } else if (cols->process < 0 && strcmp(h, "Process") == 0) {
            cols->process = i;
        } else if (cols->top < 0 && (has(h, "Top") || strcmp(h, "J20") == 0)) {
            cols->top = i;
        } else if (cols->mid < 0 &&
                   (has(h, "Mid") || has(h, "Trays") || has(h, "Heat-M"))) {
            cols->mid = i;
        } else if (cols->bot < 0 && (has(h, "Bot") || strcmp(h, "J19") == 0)) {
            cols->bot = i;
        } else if (cols->mid < 0 && strcmp(h, "J17") == 0) {
            cols->mid = i;
        }
    }
    return cols->tstamp >= 0;
}

static bool col_long(char **c, int n, int idx, long *out)
{
    if (idx < 0 || idx >= n || c[idx][0] == '\0') {
        return false;
    }
    char *end = NULL;
    long v = strtol(c[idx], &end, 10);
    if (end == c[idx]) {
        return false;
    }
    *out = v;
    return true;
}

static int col_temp(char **c, int n, int idx)
{
    long v;
    if (!col_long(c, n, idx, &v)) {
        return HR_CSV_NO_TEMP; /* absent, or the dryer's "--" */
    }
    if (v < -200 || v > 500) {
        return HR_CSV_NO_TEMP;
    }
    return (int)v;
}

bool hr_csv_row_parse(char *line, const hr_csv_cols_t *cols, hr_csv_row_t *row)
{
    if (line == NULL || cols == NULL || row == NULL) {
        return false;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                       line[len - 1] == '\a')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return false;
    }
    char *c[HR_CSV_COLS_MAX];
    int n = split(line, c, HR_CSV_COLS_MAX);
    /* the header's fixed part must be there */
    if (n <= cols->tstamp) {
        return false;
    }
    memset(row, 0, sizeof(*row));
    row->mtorr = -1;
    row->htrreq = row->htron = -1;
    row->top_f = row->mid_f = row->bot_f = row->room_f = HR_CSV_NO_TEMP;
    row->process = "";

    long v;
    if (col_long(c, n, 0, &v)) {
        row->counter = v;
    }
    /* "M/D/YYYY H:MM" - also accepts "-" between date parts */
    {
        const char *t = c[cols->tstamp];
        int mo = 0, d = 0, y = 0, h = 0, mi = 0;
        if (sscanf(t, "%d/%d/%d %d:%d", &mo, &d, &y, &h, &mi) == 5 ||
            sscanf(t, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
            if (y < 100) {
                y += 2000;
            }
            if (mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 && h < 24 &&
                mi >= 0 && mi < 60) {
                row->month = mo;
                row->day = d;
                row->year = y;
                row->hour = h;
                row->minute = mi;
                row->have_time = true;
            }
        }
    }
    if (col_long(c, n, cols->mtorr, &v)) {
        row->mtorr = v;
    }
    if (col_long(c, n, cols->htrreq, &v)) {
        row->htrreq = (int)v;
    }
    if (col_long(c, n, cols->htron, &v)) {
        row->htron = (int)v;
    }
    row->top_f = col_temp(c, n, cols->top);
    row->mid_f = col_temp(c, n, cols->mid);
    row->bot_f = col_temp(c, n, cols->bot);
    row->room_f = col_temp(c, n, cols->room);
    if (cols->process >= 0 && cols->process < n) {
        row->process = c[cols->process];
    }
    /* a row with a time stamp but nothing measured is still a row */
    return row->have_time || row->mtorr >= 0 || row->mid_f != HR_CSV_NO_TEMP;
}

long hr_csv_row_minutes(const hr_csv_row_t *row)
{
    if (row == NULL || !row->have_time) {
        return -1;
    }
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273,
                                304, 334};
    long y = row->year - 2000;
    if (y < 0) {
        return -1;
    }
    /* leap days before this year, plus this year's if past February */
    long days = y * 365 + (y + 3) / 4;
    days += cum[row->month - 1] + (row->day - 1);
    if (row->month > 2 && (y % 4) == 0) {
        days += 1;
    }
    return (days * 24 + row->hour) * 60 + row->minute;
}
