#include "hr_log.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Ring sizing.
 *
 * Was 80 lines, which held about 45 seconds once TX logging roughly doubled
 * the line rate - not enough to capture a boot plus the handshake that
 * follows it, which is exactly the window that matters when diagnosing a
 * dryer that will not start.
 *
 * 384 x 144 = 55KB of static DRAM. At the observed busy rate (~1.8 lines/s
 * while a panel sits on the WiFi screen) that is around 3.5 minutes, and
 * roughly 10 minutes on an idle machine.
 */
#define LOG_LINES 384
#define LOG_LINE_LEN HR_LOG_LINE_MAX

static char s_lines[LOG_LINES][LOG_LINE_LEN];
static int s_head;      /* next slot to write */
static int s_count;     /* how many valid lines */
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev_vprintf;

static void push_line(const char *text)
{
    if (s_lock == NULL) {
        return;
    }
    /* Never block the logging path for long. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    snprintf(s_lines[s_head], LOG_LINE_LEN, "%s", text);
    /* strip trailing newline/CR for clean JSON */
    size_t n = strlen(s_lines[s_head]);
    while (n > 0 && (s_lines[s_head][n - 1] == '\n' ||
                     s_lines[s_head][n - 1] == '\r')) {
        s_lines[s_head][--n] = '\0';
    }
    s_head = (s_head + 1) % LOG_LINES;
    if (s_count < LOG_LINES) {
        s_count++;
    }
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
    vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    /* Skip lines that are only whitespace/escape noise. */
    if (buf[0] != '\0') {
        push_line(buf);
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
    s_lock = xSemaphoreCreateMutex();
    s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
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
        n = (size_t)s_count;
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
        if (i < (size_t)s_count) {
            int start = (s_count == LOG_LINES) ? s_head : 0;
            int idx = (start + (int)i) % LOG_LINES;
            snprintf(out, cap, "%s", s_lines[idx]);
            ok = true;
        }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

size_t hr_log_json(char *out, size_t cap)
{
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        int start = (s_count == LOG_LINES) ? s_head : 0;
        for (int i = 0; i < s_count && o + 200 < cap; i++) {
            int idx = (start + i) % LOG_LINES;
            char e[LOG_LINE_LEN * 2];
            hr_log_escape(s_lines[idx], e, sizeof(e));
            o += (size_t)snprintf(out + o, cap - o, "%s\"%s\"", i ? "," : "", e);
        }
        xSemaphoreGive(s_lock);
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    return o;
}
