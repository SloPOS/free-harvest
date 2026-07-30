/* Minimal dependency-free test harness. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_fails = 0;
static const char *g_case = "";

#define TEST_CASE(name) do { g_case = (name); } while (0)

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fails++;                                                         \
            printf("  FAIL [%s] %s:%d: %s\n", g_case, __FILE__, __LINE__,      \
                   #cond);                                                     \
        }                                                                      \
    } while (0)

#define CHECK_STR(actual, expected)                                            \
    do {                                                                       \
        g_checks++;                                                            \
        const char *a_ = (actual);                                             \
        const char *e_ = (expected);                                           \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                               \
            g_fails++;                                                         \
            printf("  FAIL [%s] %s:%d: got \"%s\", want \"%s\"\n", g_case,     \
                   __FILE__, __LINE__, a_ ? a_ : "(null)", e_);                \
        }                                                                      \
    } while (0)

#define CHECK_INT(actual, expected)                                            \
    do {                                                                       \
        g_checks++;                                                            \
        long a_ = (long)(actual);                                              \
        long e_ = (long)(expected);                                            \
        if (a_ != e_) {                                                        \
            g_fails++;                                                         \
            printf("  FAIL [%s] %s:%d: got %ld, want %ld\n", g_case, __FILE__, \
                   __LINE__, a_, e_);                                          \
        }                                                                      \
    } while (0)

#define TEST_REPORT()                                                          \
    (printf("\n%s: %d checks, %d failures\n", g_fails ? "FAILED" : "PASSED",   \
            g_checks, g_fails),                                                \
     g_fails ? 1 : 0)

#endif /* TEST_UTIL_H */
