/* src/log.c
 * Logging subsystem.
 */

#include <stdarg.h>
#include <stdio.h>

#include "hxm.h"

#if HXM_DIAG

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static bool env_truthy(const char* s) {
    if (!s || !*s) return false;
    if (strcmp(s, "1") == 0) return true;
    if (strcasecmp(s, "true") == 0) return true;
    if (strcasecmp(s, "yes") == 0) return true;
    if (strcasecmp(s, "on") == 0) return true;
    return false;
}

static void log_init_once(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    const char* utc = getenv("HXM_LOG_UTC");
    if (env_truthy(utc)) use_utc = true;

    const char* mono = getenv("HXM_LOG_MONO");
    if (env_truthy(mono)) use_monotonic = true;

    // Make stdout line-buffered so logs show up promptly when piped
    setvbuf(stdout, NULL, _IOLBF, 0);
}

static void format_timestamp(char* out, size_t out_sz, long* ms_out) {
    struct timespec ts;
    clockid_t clk = use_monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    if (clock_gettime(clk, &ts) != 0) {
        snprintf(out, out_sz, "??:??:??");
        *ms_out = 0;
        return;
    }

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
    if (!HXM_LOG_ENABLED(level)) return;
    log_init_once();

    if (!fmt) fmt = "(null fmt)";

    char tsbuf[32];
    long ms = 0;
    format_timestamp(tsbuf, sizeof(tsbuf), &ms);

    FILE* out = (level >= LOG_WARN) ? stderr : stdout;
    fprintf(out, "[%s.%03ld %s] ", tsbuf, ms, safe_level_str(level));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);

    if (level >= LOG_WARN) fflush(out);
}

#else

void hxm_err(const char* fmt, ...) {
    if (!fmt) fmt = "(null fmt)";

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#endif
