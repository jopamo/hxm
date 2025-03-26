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

int main(void) {
    test_counters_init_and_empty_dump();
    test_counters_tick_and_events();
    return 0;
}
