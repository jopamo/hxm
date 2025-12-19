#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bbox.h"

static enum log_level min_level = LOG_INFO;

static const char* level_str[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO] = "INFO",
    [LOG_WARN] = "WARN",
    [LOG_ERROR] = "ERROR",
};

static void log_init(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    const char* env = getenv("BBOX_LOG");
    if (env) {
        if (strcmp(env, "debug") == 0)
            min_level = LOG_DEBUG;
        else if (strcmp(env, "info") == 0)
            min_level = LOG_INFO;
        else if (strcmp(env, "warn") == 0)
            min_level = LOG_WARN;
        else if (strcmp(env, "error") == 0)
            min_level = LOG_ERROR;
    }
}

void bbox_log(enum log_level level, const char* fmt, ...) {
    log_init();
    if (level < min_level) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tm);

    fprintf(stderr, "[%s.%03ld %s] ", timestamp, ts.tv_nsec / 1000000, level_str[level]);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}