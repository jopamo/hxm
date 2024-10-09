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

#include "bbox.h"

static enum log_level min_level = LOG_INFO;
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

static enum log_level parse_level(const char* env) {
    if (!env || !env[0]) return LOG_INFO;

    if (strcmp(env, "debug") == 0) return LOG_DEBUG;
    if (strcmp(env, "info") == 0) return LOG_INFO;
    if (strcmp(env, "warn") == 0) return LOG_WARN;
    if (strcmp(env, "error") == 0) return LOG_ERROR;

    /* Accept numeric too */
    if (strcmp(env, "0") == 0) return LOG_DEBUG;
    if (strcmp(env, "1") == 0) return LOG_INFO;
    if (strcmp(env, "2") == 0) return LOG_WARN;
    if (strcmp(env, "3") == 0) return LOG_ERROR;

    return LOG_INFO;
}

static void log_init_once(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    min_level = parse_level(getenv("BBOX_LOG"));

    const char* utc = getenv("BBOX_LOG_UTC");
    if (utc && (strcmp(utc, "1") == 0 || strcmp(utc, "true") == 0)) use_utc = true;

    const char* mono = getenv("BBOX_LOG_MONO");
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

void bbox_log(enum log_level level, const char* fmt, ...) {
    log_init_once();

    if (level < min_level) return;

    if (!fmt) fmt = "(null fmt)";

    char tsbuf[32];
    long ms = 0;
    format_timestamp(tsbuf, sizeof(tsbuf), &ms);

    /* One fprintf for prefix + one vfprintf for body
     * stdout is not used; keep logs together and unbuffered-ish
     */
    fprintf(stderr, "[%s.%03ld %s] ", tsbuf, ms, safe_level_str(level));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
