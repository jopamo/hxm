/* src/log.c
 * Logging utilities
 *
 * Goals:
 *  - async-signal-safe enough for "best effort" diagnostics (avoid malloc)
 *  - cheap fast-path when filtered
 *  - tolerate bad inputs (NULL fmt, invalid level)
 *  - optional monotonic timestamps for ordering
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hxm.h"

static bool use_utc = false;
static bool use_monotonic = false;

static const char* level_str[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO] = "INFO",
    [LOG_WARN] = "WARN",
    [LOG_ERROR] = "ERROR",
};

static inline const char* safe_level_str(enum log_level level) {
    if ((unsigned)level < (unsigned)(sizeof(level_str) / sizeof(level_str[0])) && level_str[level])
        return level_str[level];
    return "UNK";
}

static void log_init_once(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    const char* utc = getenv("HXM_LOG_UTC");
    if (utc && (strcmp(utc, "1") == 0 || strcmp(utc, "true") == 0)) use_utc = true;

    const char* mono = getenv("HXM_LOG_MONO");
    if (mono && (strcmp(mono, "1") == 0 || strcmp(mono, "true") == 0)) use_monotonic = true;
}

static void format_timestamp(char* out, size_t out_sz, long* ms_out) {
    struct timespec ts;
    clockid_t clk = use_monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    clock_gettime(clk, &ts);

    *ms_out = ts.tv_nsec / 1000000;

    struct tm tm;
    time_t sec = ts.tv_sec;
    if (use_monotonic) {
        /* For monotonic, print seconds since boot-like epoch */
        snprintf(out, out_sz, "%ld", (long)sec);
        return;
    }

    if (use_utc)
        gmtime_r(&sec, &tm);
    else
        localtime_r(&sec, &tm);

    strftime(out, out_sz, "%H:%M:%S", &tm);
}

void hxm_log(enum log_level level, const char* fmt, ...) {
    log_init_once();

    if (!fmt) fmt = "(null fmt)";

    char tsbuf[32];
    long ms = 0;
    format_timestamp(tsbuf, sizeof(tsbuf), &ms);

#ifdef HXM_ENABLE_DEBUG_LOGGING
    FILE* out = (level == LOG_ERROR) ? stderr : stdout;
    fprintf(out, "[%s.%03ld %s] ", tsbuf, ms, safe_level_str(level));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
#else
    // In production, only log ERRORs to stderr.
    if (level == LOG_ERROR) {
        fprintf(stderr, "[%s.%03ld %s] ", tsbuf, ms, safe_level_str(level));
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
#endif
}
