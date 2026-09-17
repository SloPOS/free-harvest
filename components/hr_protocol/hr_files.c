/*
 * hr_files - see hr_files.h. Reads only; nothing here can change the dryer.
 */
#include "hr_files.h"

#include <stdio.h>
#include <string.h>

#define BLOCK_VERB     "FDFILEBLOCK,"
#define BLOCK_VERB_LEN 12
#define LIST_VERB      "FDFILELIST"

/* ------------------------------------------------------------------ */
/* What we are willing to say to the dryer                             */
/* ------------------------------------------------------------------ */
/*
 * Every verb the dryer looks for BEFORE the two we use.
 *
 * Its dispatcher searches the whole line for each verb in its own order and
 * takes the first hit, so a verb sitting inside an argument wins over the one
 * we meant: asking to read a file whose name contains DEL is asking the
 * machine to consider deleting something. The order is the command-ID order
 * recorded in decoded/PROTOCOL_NOTES.md; these are the ids below FDFILES.
 *
 * The comparison there is case-sensitive, so only capitals collide - which is
 * why "recipe.csv" is fine and "RECIPE.CSV" would not be. hr_recipe.c guards
 * recipe names the same way, for the same reason.
 */
static const char *const k_verbs_before_us[] = {
    "MEMTEST", "PRINT", "BEEP", "DIR", "MEMSIZE", "RMOLD", "XWIFI", "XW",
    "DUTY", "SERIAL", "FUZZY", "DUMP", "COPY", "DEL", "DIRC", "GETR", "ADV",
    "GETP", "ADD", "UNIQUE", "FDNAME", "STATUS", "CLICK", "STATE", "FDRENAME",
    "SENDBATCH", "SENDCANDY", "SENDCUSTOM", "ECHO", "GOTIT", "SETBNAME",
    "HCS", "SPC", "WIFIINFO", "REQCFG", "REQTSUM", "REQTHST", "SETSN",
    "REQSTAT", "REBOOT", "REQSYSINF", "REQBATSUM",
};

const char *hr_files_arg_verb(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_verbs_before_us) / sizeof(*k_verbs_before_us);
         i++) {
        if (strstr(s, k_verbs_before_us[i]) != NULL) {
            return k_verbs_before_us[i];
        }
    }
    return NULL;
}

bool hr_files_arg_ok(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    size_t n = strlen(s);
    if (n >= HR_FILE_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        /*
         * Printable only. A control character is the dangerous one: our own
         * terminator inside an argument would end the frame and leave the
         * rest of it to be read as a command of somebody else's choosing.
         * The rest cannot travel: a space splits the argument in two, a
         * quote has no escape on this wire, a comma would corrupt the reply
         * header, and slashes and wildcards are not ours to send.
         */
        if (c <= 0x20 || c >= 0x7f) {
            return false;
        }
        if (c == '"' || c == '\'' || c == ',' || c == '/' || c == '\\' ||
            c == '*' || c == '?' || c == '|' || c == ';') {
            return false;
        }
    }
    return hr_files_arg_verb(s) == NULL;
}

/* ------------------------------------------------------------------ */
/* Small parsing helpers                                               */
/* ------------------------------------------------------------------ */
/*
 * A decimal field running from `from` to the next comma. Returns the index
 * just past that comma, or -1 if the field is absent, empty, over-long or
 * not a plain non-negative number. Ten digits is more than any of these
 * fields can carry and keeps the running total inside a long.
 */
static int uint_field(const char *buf, size_t len, size_t from, long *out)
{
    size_t i = from;
    long v = 0;
    size_t digits = 0;
    for (; i < len && buf[i] != ','; i++) {
        if (buf[i] < '0' || buf[i] > '9' || digits >= 9) {
            return -1;
        }
        v = v * 10 + (buf[i] - '0');
        digits++;
    }
    if (i >= len || digits == 0) {
        return -1;
    }
    *out = v;
    return (int)(i + 1);
}

static int hex_digit(char c)
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

/* ------------------------------------------------------------------ */
/* FDFILELIST                                                          */
/* ------------------------------------------------------------------ */
bool hr_file_entry_parse(const hr_frame_t *f, hr_file_entry_t *e)
{
    if (f == NULL || e == NULL || strcmp(f->verb, LIST_VERB) != 0) {
        return false;
    }
    const char *name = hr_frame_field(f, 0);
    const char *idx = hr_frame_field(f, 1);
    const char *size = hr_frame_field(f, 2);
    if (name == NULL || idx == NULL || size == NULL) {
        return false;
    }
    if (strlen(name) >= sizeof(e->name)) {
        return false;
    }
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->index = hr_frame_field_int(f, 1, -1);
    e->size = hr_frame_field_int(f, 2, -1);
    if (e->index < 0 || e->size < 0) {
        return false;
    }
    /* Past the last match the dryer names the entry NULL and sizes it 0. */
    if (strcmp(e->name, "NULL") == 0 && e->size == 0) {
        e->last = true;
        e->name[0] = '\0';
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* FDFILEBLOCK                                                         */
/* ------------------------------------------------------------------ */
int hr_file_block_measure(const char *buf, size_t len, size_t *total)
{
    if (buf == NULL || total == NULL) {
        return -1;
    }
    /* Compare only as far as we have, so a line that is not a block frame is
     * rejected on its first differing byte rather than at the end. */
    const size_t cmp = len < BLOCK_VERB_LEN ? len : BLOCK_VERB_LEN;
    if (memcmp(buf, BLOCK_VERB, cmp) != 0) {
        return -1;
    }
    if (len < BLOCK_VERB_LEN) {
        return 0;
    }
    /* The name, which is empty when the dryer has no such file. */
    size_t i = BLOCK_VERB_LEN;
    while (i < len && buf[i] != ',') {
        i++;
    }
    if (i - BLOCK_VERB_LEN >= HR_FILE_NAME_MAX) {
        return -1;
    }
    if (i >= len) {
        return 0;
    }

    long nbytes = 0, block = 0, size = 0;
    int at = uint_field(buf, len, i + 1, &nbytes);
    if (at < 0) {
        return (memchr(buf + i + 1, ',', len - i - 1) == NULL) ? 0 : -1;
    }
    int at2 = uint_field(buf, len, (size_t)at, &block);
    if (at2 < 0) {
        return (memchr(buf + at, ',', len - (size_t)at) == NULL) ? 0 : -1;
    }
    int at3 = uint_field(buf, len, (size_t)at2, &size);
    if (at3 < 0) {
        return (memchr(buf + at2, ',', len - (size_t)at2) == NULL) ? 0 : -1;
    }
    if (nbytes > HR_FILE_BLOCK) {
        return -1;
    }
    /* Header, the data the header declares, and the two checksum digits. */
    *total = (size_t)at3 + (size_t)nbytes + 2;
    return 1;
}

bool hr_file_block_parse(const char *frame, size_t len, hr_file_block_t *out)
{
    size_t total = 0;
    if (frame == NULL || out == NULL) {
        return false;
    }
    if (hr_file_block_measure(frame, len, &total) != 1 || total != len) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *name = frame + BLOCK_VERB_LEN;
    const char *comma = memchr(name, ',', len - BLOCK_VERB_LEN);
    if (comma == NULL) {
        return false;
    }
    const size_t nl = (size_t)(comma - name);
    memcpy(out->name, name, nl);
    out->name[nl] = '\0';

    long nbytes = 0, block = 0, size = 0;
    int at = uint_field(frame, len, (size_t)(comma - frame) + 1, &nbytes);
    at = uint_field(frame, len, (size_t)at, &block);
    at = uint_field(frame, len, (size_t)at, &size);
    if (at < 0) {
        return false;    /* measure() already agreed, so this cannot happen */
    }
    out->nbytes = nbytes;
    out->block = block;
    out->size = size;
    out->data = frame + at;

    const int hi = hex_digit(frame[len - 2]);
    const int lo = hex_digit(frame[len - 1]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    out->sum = (unsigned)(hi * 16 + lo);

    unsigned acc = 0;
    for (long i = 0; i < nbytes; i++) {
        acc += (unsigned char)out->data[i];
    }
    out->sum_ok = ((acc & 0xffu) == out->sum);
    /* No name, no bytes and no size: the dryer is saying it has no such file. */
    out->missing = (out->name[0] == '\0' && nbytes == 0 && size == 0);
    return true;
}

size_t hr_file_restore_cr(char *data, size_t n)
{
    if (data == NULL) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (data[i] == 0x07) {
            data[i] = '\r';
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* The transfer                                                        */
/* ------------------------------------------------------------------ */
void hr_files_init(hr_files_t *fs, hr_files_send_fn send, void *user)
{
    if (fs == NULL) {
        return;
    }
    memset(fs, 0, sizeof(*fs));
    fs->send = send;
    fs->send_user = user;
    fs->size = -1;
}

void hr_files_set_entry_cb(hr_files_t *fs, hr_files_entry_fn fn, void *user)
{
    if (fs != NULL) {
        fs->on_entry = fn;
        fs->entry_user = user;
    }
}

void hr_files_set_data_cb(hr_files_t *fs, hr_files_data_fn fn, void *user)
{
    if (fs != NULL) {
        fs->on_data = fn;
        fs->data_user = user;
    }
}

void hr_files_set_done_cb(hr_files_t *fs, hr_files_done_fn fn, void *user)
{
    if (fs != NULL) {
        fs->on_done = fn;
        fs->done_user = user;
    }
}

bool hr_files_busy(const hr_files_t *fs)
{
    return fs != NULL &&
           (fs->state == HR_FILES_LISTING || fs->state == HR_FILES_READING);
}

static void finish(hr_files_t *fs, hr_files_state_t st, hr_files_err_t err)
{
    fs->state = st;
    fs->err = err;
    fs->waiting = false;
    fs->pending = false;
    fs->paused = false;
    if (fs->on_done != NULL) {
        fs->on_done(st, err, fs->done_user);
    }
}

static bool begin(hr_files_t *fs, const char *arg, unsigned long now_ms)
{
    if (fs == NULL || hr_files_busy(fs) || !hr_files_arg_ok(arg)) {
        return false;
    }
    snprintf(fs->arg, sizeof(fs->arg), "%s", arg);
    fs->index = 0;
    fs->block = 0;
    fs->size = -1;
    fs->received = 0;
    fs->entries = 0;
    fs->retries = 0;
    fs->waiting = false;
    fs->paused = false;
    fs->pending = true;
    fs->err = HR_FILES_OK;
    fs->started_ms = now_ms;
    fs->sent_ms = now_ms;
    fs->transfers++;
    return true;
}

bool hr_files_list(hr_files_t *fs, const char *pattern, unsigned long now_ms)
{
    if (!begin(fs, pattern, now_ms)) {
        return false;
    }
    fs->state = HR_FILES_LISTING;
    return true;
}

bool hr_files_read(hr_files_t *fs, const char *name, long size,
                   unsigned long now_ms)
{
    if (fs == NULL || name == NULL) {
        return false;
    }
    /*
     * A name of the dryer's own choosing may start with a dot; one of ours
     * may not. The machine hides dot-names from its listings, so anything
     * starting with one came from somewhere other than a listing.
     */
    if (name[0] == '.') {
        return false;
    }
    if (size > HR_FILE_SIZE_MAX) {
        fs->state = HR_FILES_FAILED;
        fs->err = HR_FILES_ERR_TOO_BIG;
        return false;
    }
    if (!begin(fs, name, now_ms)) {
        return false;
    }
    fs->state = HR_FILES_READING;
    fs->size = size;
    return true;
}

void hr_files_cancel(hr_files_t *fs, unsigned long now_ms)
{
    (void)now_ms;
    if (hr_files_busy(fs)) {
        finish(fs, HR_FILES_FAILED, HR_FILES_ERR_CANCELLED);
    }
}

void hr_files_resume(hr_files_t *fs, unsigned long now_ms)
{
    if (fs == NULL || !fs->paused) {
        return;
    }
    fs->paused = false;
    fs->pending = true;
    fs->sent_ms = now_ms;
}

/* Put one request on the wire. */
static void send_request(hr_files_t *fs, unsigned long now_ms)
{
    char idx[16];
    const bool listing = (fs->state == HR_FILES_LISTING);
    snprintf(idx, sizeof(idx), "%ld", listing ? fs->index : fs->block);
    fs->pending = false;
    if (fs->send == NULL ||
        !fs->send(listing ? "FDFILES" : "FILEREAD", fs->arg, idx,
                  fs->send_user)) {
        finish(fs, HR_FILES_FAILED, HR_FILES_ERR_SEND);
        return;
    }
    fs->requests++;
    fs->waiting = true;
    fs->sent_ms = now_ms;
}

void hr_files_on_frame(hr_files_t *fs, const hr_frame_t *f,
                       unsigned long now_ms)
{
    hr_file_entry_t e;
    if (fs == NULL || f == NULL || fs->state != HR_FILES_LISTING) {
        return;
    }
    if (!fs->waiting || !hr_file_entry_parse(f, &e)) {
        return;    /* not an answer, or not one we are waiting for */
    }
    /*
     * An entry for an index we did not ask for is a late answer to a request
     * we already gave up on. Treating it as the end would cut the listing
     * short, so it is simply dropped and the outstanding request stands.
     */
    if (e.index != fs->index) {
        return;
    }
    fs->waiting = false;
    fs->retries = 0;
    if (fs->on_entry != NULL) {
        fs->on_entry(&e, fs->entry_user);
    }
    if (e.last) {
        finish(fs, HR_FILES_DONE, HR_FILES_OK);
        return;
    }
    fs->entries++;
    fs->index++;
    fs->pending = true;
    (void)now_ms;
}

void hr_files_on_block(hr_files_t *fs, char *frame, size_t len,
                       unsigned long now_ms)
{
    hr_file_block_t b;
    if (fs == NULL || frame == NULL || fs->state != HR_FILES_READING) {
        return;
    }
    if (!fs->waiting || !hr_file_block_parse(frame, len, &b)) {
        return;
    }
    if (b.missing) {
        finish(fs, HR_FILES_FAILED, HR_FILES_ERR_MISSING);
        return;
    }
    /* Somebody else's block, or a late one: leave the request outstanding. */
    if (b.block != fs->block || strcmp(b.name, fs->arg) != 0) {
        return;
    }
    if (!b.sum_ok) {
        /*
         * The checksum is the only thing standing between a corrupted block
         * and a file that looks fine, so a bad one is re-asked rather than
         * passed on. The dryer re-reads it happily.
         */
        fs->blocks_bad++;
        fs->waiting = false;
        if (++fs->retries > HR_FILES_RETRIES) {
            finish(fs, HR_FILES_FAILED, HR_FILES_ERR_CHECKSUM);
            return;
        }
        fs->pending = true;
        return;
    }
    fs->blocks_ok++;
    fs->waiting = false;
    fs->retries = 0;
    if (fs->size < 0) {
        fs->size = b.size;
    }

    /*
     * Hand the data over, its own carriage returns restored. A consumer with
     * no room says so, and we stop asking: the block is NOT counted, and
     * hr_files_resume() re-requests this same block rather than keeping a
     * copy of it here. One repeated kilobyte is cheaper than a second buffer
     * on a chip with 4 KB to spare.
     */
    char *data = frame + (b.data - frame);
    hr_file_restore_cr(data, (size_t)b.nbytes);
    if (b.nbytes > 0 && fs->on_data != NULL &&
        !fs->on_data(data, (size_t)b.nbytes, fs->data_user)) {
        fs->paused = true;
        fs->pending = false;
        return;
    }
    fs->received += b.nbytes;

    /* A short block is the end of the file; so is reaching the size. */
    if (b.nbytes < HR_FILE_BLOCK ||
        (fs->size >= 0 && fs->received >= fs->size)) {
        finish(fs, HR_FILES_DONE, HR_FILES_OK);
        return;
    }
    fs->block++;
    fs->pending = true;
    (void)now_ms;
}

void hr_files_tick(hr_files_t *fs, unsigned long now_ms, bool link_up)
{
    if (!hr_files_busy(fs)) {
        return;
    }
    if (!link_up) {
        finish(fs, HR_FILES_FAILED, HR_FILES_ERR_LINK);
        return;
    }
    if (fs->paused) {
        return;    /* the consumer's clock, not the dryer's */
    }
    if (fs->waiting && now_ms - fs->sent_ms >= HR_FILES_REPLY_MS) {
        fs->timeouts++;
        fs->waiting = false;
        if (++fs->retries > HR_FILES_RETRIES) {
            finish(fs, HR_FILES_FAILED, HR_FILES_ERR_TIMEOUT);
            return;
        }
        fs->pending = true;
    }
    if (fs->pending) {
        send_request(fs, now_ms);
    }
}

const char *hr_files_state_str(hr_files_state_t st)
{
    switch (st) {
    case HR_FILES_IDLE:    return "idle";
    case HR_FILES_LISTING: return "listing";
    case HR_FILES_READING: return "reading";
    case HR_FILES_DONE:    return "done";
    case HR_FILES_FAILED:  return "failed";
    default:               return "?";
    }
}

const char *hr_files_err_str(hr_files_err_t e)
{
    switch (e) {
    case HR_FILES_OK:            return "";
    case HR_FILES_ERR_TIMEOUT:   return "no answer";
    case HR_FILES_ERR_LINK:      return "link down";
    case HR_FILES_ERR_CHECKSUM:  return "checksum";
    case HR_FILES_ERR_MISSING:   return "no such file";
    case HR_FILES_ERR_TOO_BIG:   return "too big";
    case HR_FILES_ERR_SINK:      return "nobody taking it";
    case HR_FILES_ERR_SEND:      return "could not send";
    case HR_FILES_ERR_CANCELLED: return "cancelled";
    default:                     return "?";
    }
}
