/* src/core.c
 * Core system counters and initialization
 *
 * Notes:
 *  - counters is a global singleton, zeroed on init
 *  - monotonic_time_ns is weak so tests can override it
 *  - dump prints human-friendly + machine-scrapable-ish output
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hxm.h"

struct counters counters;

static inline uint64_t u64_min(uint64_t a, uint64_t b) { return (a < b) ? a : b; }

static inline uint64_t u64_max(uint64_t a, uint64_t b) { return (a > b) ? a : b; }

void counters_init(void) {
    memset(&counters, 0, sizeof(counters));

    /* If no ticks recorded, min stays at UINT64_MAX (sentinel) */
    counters.tick_duration_min = UINT64_MAX;
}

void counters_tick_record(uint64_t dt_ns) {
    counters.tick_count++;

    counters.tick_duration_sum += dt_ns;
    counters.tick_duration_min = u64_min(counters.tick_duration_min, dt_ns);
    counters.tick_duration_max = u64_max(counters.tick_duration_max, dt_ns);
}

#ifdef HXM_ENABLE_DEBUG_LOGGING
static void print_tick_stats(void) {
    printf("Tick count: %" PRIu64 "\n", counters.tick_count);

    if (counters.tick_count == 0) return;

    uint64_t min = counters.tick_duration_min;
    uint64_t max = counters.tick_duration_max;
    uint64_t avg = counters.tick_duration_sum / counters.tick_count;

    if (min == UINT64_MAX) min = 0;

    printf("Tick duration: min=%" PRIu64 " avg=%" PRIu64 " max=%" PRIu64 " ns\n", min, avg, max);
}

static void print_event_stats(void) {
    bool any = false;
    for (int i = 0; i < 256; i++) {
        if (counters.events_seen[i] || counters.coalesced_drops[i]) {
            any = true;
            break;
        }
    }

    if (!any) return;

    printf("=== X events ===\n");
    for (int i = 0; i < 256; i++) {
        uint64_t seen = counters.events_seen[i];
        uint64_t drop = counters.coalesced_drops[i];
        if (!seen && !drop) continue;

        printf("Event %3d: seen=%" PRIu64 " coalesced=%" PRIu64 "\n", i, seen, drop);
    }
}

void counters_dump(void) {
    printf("=== HXM counters ===\n");

    print_tick_stats();

    printf("X flushes: %" PRIu64 "\n", counters.x_flush_count);
    printf("Config requests applied: %" PRIu64 "\n", counters.config_requests_applied);
    printf("Restacks applied: %" PRIu64 "\n", counters.restacks_applied);

    print_event_stats();
}
#endif

__attribute__((weak)) uint64_t monotonic_time_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;

    /* Avoid UB on weird platforms; cast after multiply */
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}
