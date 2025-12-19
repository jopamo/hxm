#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bbox.h"

struct counters counters;

void counters_init(void) {
    memset(&counters, 0, sizeof(counters));
    counters.tick_duration_min = (uint64_t)-1;
}

void counters_dump(void) {
    printf("=== BBOX counters ===\n");
    printf("Tick count: %lu\n", counters.tick_count);
    if (counters.tick_count > 0) {
        uint64_t avg = counters.tick_duration_sum / counters.tick_count;
        printf("Tick duration: min=%lu avg=%lu max=%lu ns\n", counters.tick_duration_min, avg,
               counters.tick_duration_max);
    }
    printf("X flushes: %lu\n", counters.x_flush_count);
    printf("Config requests applied: %lu\n", counters.config_requests_applied);
    printf("Restacks applied: %lu\n", counters.restacks_applied);

    // Event type dump (only non-zero)
    for (int i = 0; i < 256; i++) {
        if (counters.events_seen[i] > 0) {
            printf("Event %3d: seen=%lu coalesced=%lu\n", i, counters.events_seen[i], counters.coalesced_drops[i]);
        }
    }
}

uint64_t monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}