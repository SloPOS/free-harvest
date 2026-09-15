#include "hr_log.h"
#include "hr_logring.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Ring sizing.
 *
 * This was 384 fixed slots of 144 bytes - 55 KB of static DRAM, about a sixth
 * of everything the chip has, for a log whose lines average ~65 characters.
 * Packed, the same DRAM holds more than twice the lines, and the default of
 * CONFIG_HR_LOG_RING_KB (32 KB) still keeps around 500 lines: a boot, the
 * handshake and roughly five minutes of a busy link, or a quarter of an hour
 * of an idle machine. Boards without PSRAM can turn it down.
 */
#define LOG_RING_BYTES ((size_t)CONFIG_HR_LOG_RING_KB * 1024u)
#define LOG_LINE_LEN HR_LOG_LINE_MAX

static char s_store[LOG_RING_BYTES];
static hr_logring_t s_ring;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev_vprintf;
static hr_log_sink_t s_sink;

static void push_line(const char *text)
{
    if (s_lock == NULL) {
        return;
    }
    size_t n = strlen(text);
    /* strip trailing newline/CR for clean JSON */
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r')) {
        n--;
    }
    if (n == 0) {
        return;
    }
    /* Never block the logging path for long. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    hr_logring_push(&s_ring, text, n);
    xSemaphoreGive(s_lock);
}

/*
 * ESP-IDF log hook. We format into a local buffer, keep a copy, and also
 * forward to the original vprintf so the serial console still works.
 */
static int log_vprintf(const char *fmt, va_list args)
{
    char buf[LOG_LINE_LEN];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    /* Skip lines that are only whitespace/escape noise. */
    if (buf[0] != '\0') {
        push_line(buf);
        if (s_sink != NULL && n > 0) {
            s_sink(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
        }
    }
    if (s_prev_vprintf) {
        return s_prev_vprintf(fmt, args);
    }
    return 0;
}

void hr_log_init(void)
{
    if (s_lock != NULL) {
        return;
    }
    hr_logring_init(&s_ring, s_store, sizeof(s_store));
    s_lock = xSemaphoreCreateMutex();
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
}

void hr_log_set_sink(hr_log_sink_t sink)
{
    s_sink = sink;
}

void hr_log_addf(const char *fmt, ...)
{
    char buf[LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    push_line(buf);
}

/* Escape a log line into JSON-string body. */
size_t hr_log_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20 || c == 0x7f) {
            continue; /* drop control chars incl. ANSI colour escapes */
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

size_t hr_log_count(void)
{
    size_t n = 0;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        n = hr_logring_count(&s_ring);
        xSemaphoreGive(s_lock);
    }
    return n;
}

bool hr_log_line(size_t i, char *out, size_t cap)
{
    bool ok = false;
    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        ok = hr_logring_get(&s_ring, i, out, cap);
        xSemaphoreGive(s_lock);
    }
    return ok;
}

size_t hr_log_json(char *out, size_t cap)
{
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t pos = s_ring.tail, left = s_ring.count;
        char line[LOG_LINE_LEN];
        char e[LOG_LINE_LEN * 2];
        bool first = true;
        while (o + 200 < cap &&
               hr_logring_next(&s_ring, &pos, &left, line, sizeof(line))) {
            hr_log_escape(line, e, sizeof(e));
            o += (size_t)snprintf(out + o, cap - o, "%s\"%s\"",
                                  first ? "" : ",", e);
            first = false;
        }
        xSemaphoreGive(s_lock);
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}
