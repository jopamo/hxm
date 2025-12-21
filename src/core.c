#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bbox.h"

struct counters counters;

void counters_init(void) {
    memset(&counters, 0, sizeof(counters));
    counters.tick_duration_min = UINT64_MAX;
}

void counters_dump(void) {
    printf("=== BBOX counters ===\n");
    printf("Tick count: %" PRIu64 "\n", counters.tick_count);

    if (counters.tick_count > 0) {
        uint64_t avg = counters.tick_duration_sum / counters.tick_count;
        printf("Tick duration: min=%" PRIu64 " avg=%" PRIu64 " max=%" PRIu64 " ns\n", counters.tick_duration_min, avg,
               counters.tick_duration_max);
    }

    printf("X flushes: %" PRIu64 "\n", counters.x_flush_count);
    printf("Config requests applied: %" PRIu64 "\n", counters.config_requests_applied);
    printf("Restacks applied: %" PRIu64 "\n", counters.restacks_applied);

    // Event type dump (only non-zero)
    for (int i = 0; i < 256; i++) {
        if (counters.events_seen[i] > 0) {
            printf("Event %3d: seen=%" PRIu64 " coalesced=%" PRIu64 "\n", i, counters.events_seen[i],
                   counters.coalesced_drops[i]);
        }
    }
}

uint64_t monotonic_time_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
