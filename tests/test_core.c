#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include "hxm.h"

void counters_tick_record(uint64_t dt_ns);

static void test_counters_init_and_empty_dump(void) {
    counters_init();
    assert(counters.tick_count == 0);
    assert(counters.tick_duration_min == UINT64_MAX);

    counters_dump();
    printf("test_counters_init_and_empty_dump passed\n");
}

static void test_counters_tick_and_events(void) {
    counters_init();
    counters_tick_record(10);
    counters_tick_record(5);

    assert(counters.tick_count == 2);
    assert(counters.tick_duration_sum == 15);
    assert(counters.tick_duration_min == 5);
    assert(counters.tick_duration_max == 10);

    counters.x_flush_count = 3;
    counters.config_requests_applied = 4;
    counters.restacks_applied = 5;
    counters.events_seen[10] = 1;
    counters.coalesced_drops[11] = 2;

    counters_dump();
    printf("test_counters_tick_and_events passed\n");
}

static void test_counters_edge_cases(void) {
    counters_init();
    // Simulate a case where tick_count > 0 but min is still sentinel (e.g. manual manipulation or bug)
    // This exercises the 'if (min == UINT64_MAX) min = 0' branch in print_tick_stats
    counters.tick_count = 1;
    counters.tick_duration_sum = 100;
    counters.tick_duration_min = UINT64_MAX;
    counters.tick_duration_max = 100;

    printf("--- Edge case output start ---\n");
    counters_dump();
    printf("--- Edge case output end ---\n");

    printf("test_counters_edge_cases passed\n");
}

static void test_monotonic_time(void) {
    uint64_t t1 = monotonic_time_ns();
    assert(t1 > 0);

    // Simple monotonicity check
    uint64_t t2 = monotonic_time_ns();
    assert(t2 >= t1);

    printf("test_monotonic_time passed: %" PRIu64 " -> %" PRIu64 "\n", t1, t2);
}

int main(void) {
    test_counters_init_and_empty_dump();
    test_counters_tick_and_events();
    test_counters_edge_cases();
    test_monotonic_time();
    return 0;
}
