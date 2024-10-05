#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bbox.h"
#include "cookie_jar.h"
#include "wm.h"

// Externs from xcb_stubs.c
extern int (*stub_poll_for_reply_hook)(xcb_connection_t* c, unsigned int request, void** reply,
                                       xcb_generic_error_t** error);

// Mock data
static bool g_handler_called = false;
static uint32_t g_handler_seq = 0;
static void* g_handler_reply = NULL;
static xcb_generic_error_t* g_handler_error = NULL;

static void reset_handler_state(void) {
    g_handler_called = false;
    g_handler_seq = 0;
    g_handler_reply = NULL;
    g_handler_error = NULL;
}

static void mock_handler(struct server* s, const struct cookie_slot* slot, void* reply, xcb_generic_error_t* err) {
    (void)s;
    g_handler_called = true;
    g_handler_seq = slot->sequence;
    g_handler_reply = reply;
    g_handler_error = err;
}

// Mock poll hook controls
static uint32_t g_ready_seq = 0;
static void* g_ready_reply = NULL;
static xcb_generic_error_t* g_ready_error = NULL;

// Optional: simulate "reply exists but error also exists"
static bool g_ready_both_reply_and_error = false;

static int mock_poll(xcb_connection_t* c, unsigned int request, void** reply, xcb_generic_error_t** error) {
    (void)c;

    if (g_ready_seq != request) return 0;

    if (g_ready_both_reply_and_error) {
        // Some servers can give you a reply with an error ptr
        *reply = malloc(1);
        *error = malloc(sizeof(xcb_generic_error_t));
        return 1;
    }

    if (g_ready_reply) {
        // Allocate a dummy reply because cookie_jar_drain will free it
        *reply = malloc(1);
    } else {
        *reply = NULL;
    }

    if (g_ready_error) {
        *error = malloc(sizeof(xcb_generic_error_t));
        // make it look like an error so handlers can branch if they want
        (*error)->error_code = 1;
    } else {
        *error = NULL;
    }

    return 1;
}

static uint64_t g_mock_time = 0;
static bool g_use_mock_time = false;

uint64_t monotonic_time_ns(void) {
    if (g_use_mock_time) return g_mock_time;

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Helpers
static void set_ready_none(void) {
    g_ready_seq = 0;
    g_ready_reply = NULL;
    g_ready_error = NULL;
    g_ready_both_reply_and_error = false;
}

static void set_ready_reply(uint32_t seq) {
    g_ready_seq = seq;
    g_ready_reply = (void*)1;
    g_ready_error = NULL;
    g_ready_both_reply_and_error = false;
}

static void set_ready_error(uint32_t seq) {
    g_ready_seq = seq;
    g_ready_reply = NULL;
    g_ready_error = (xcb_generic_error_t*)1;
    g_ready_both_reply_and_error = false;
}

static void set_ready_both(uint32_t seq) {
    g_ready_seq = seq;
    g_ready_reply = (void*)1;
    g_ready_error = (xcb_generic_error_t*)1;
    g_ready_both_reply_and_error = true;
}

static void fail(const char* msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static void require_drain_until_called(cookie_jar_t* cj, int max_iters, int max_replies_per_iter) {
    int it = 0;
    while (!g_handler_called && it < max_iters) {
        cookie_jar_drain(cj, NULL, NULL, max_replies_per_iter);
        it++;
    }
    if (!g_handler_called) fail("handler not called within drain loop");
}

static void test_init_destroy(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);
    assert(cj.cap >= 16);
    assert(cj.live_count == 0);
    assert(cj.slots != NULL);

    cookie_jar_destroy(&cj);
    assert(cj.slots == NULL);
    printf("test_init_destroy passed\n");
}

static void test_push_and_drain(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    reset_handler_state();
    stub_poll_for_reply_hook = mock_poll;

    uint32_t seq = 123;
    bool pushed = cookie_jar_push(&cj, seq, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(pushed);
    assert(cj.live_count == 1);

    // not ready
    set_ready_none();
    cookie_jar_drain(&cj, NULL, NULL, 10);
    assert(g_handler_called == false);
    assert(cj.live_count == 1);

    // ready reply
    reset_handler_state();
    set_ready_reply(seq);
    cookie_jar_drain(&cj, NULL, NULL, 10);
    assert(g_handler_called == true);
    assert(g_handler_seq == seq);
    assert(g_handler_reply != NULL);
    assert(g_handler_error == NULL);
    assert(cj.live_count == 0);

    cookie_jar_destroy(&cj);
    printf("test_push_and_drain passed\n");
}

static void test_duplicate_push_rejected_or_replaced(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    stub_poll_for_reply_hook = mock_poll;

    uint32_t seq = 42;
    bool p1 = cookie_jar_push(&cj, seq, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(p1);
    assert(cj.live_count == 1);

    // Depending on implementation, pushing same seq may be rejected or replace
    bool p2 = cookie_jar_push(&cj, seq, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);

    // Allowed outcomes:
    // - rejected -> live_count unchanged
    // - replaced -> live_count unchanged
    // Never allowed: count increases
    assert(cj.live_count == 1);

    // Drain should call handler at most once
    reset_handler_state();
    set_ready_reply(seq);
    cookie_jar_drain(&cj, NULL, NULL, 10);
    assert(g_handler_called);
    assert(g_handler_seq == seq);
    assert(cj.live_count == 0);

    (void)p2;
    cookie_jar_destroy(&cj);
    printf("test_duplicate_push_rejected_or_replaced passed\n");
}

static void test_drain_budget_respected(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    stub_poll_for_reply_hook = mock_poll;

    // push many
    const uint32_t N = 200;
    for (uint32_t i = 1; i <= N; i++) {
        bool ok = cookie_jar_push(&cj, i, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
        assert(ok);
    }
    assert(cj.live_count == N);

    // Only one seq is ever ready, but we cap max_replies=1
    // We should handle at most one per drain call.
    uint32_t handled = 0;
    for (uint32_t i = 1; i <= N; i++) {
        reset_handler_state();
        set_ready_reply(i);
        cookie_jar_drain(&cj, NULL, NULL, 1);
        assert(g_handler_called);
        assert(g_handler_seq == i);
        handled++;
        assert(cj.live_count == (size_t)(N - handled));
    }

    assert(cj.live_count == 0);
    cookie_jar_destroy(&cj);
    printf("test_drain_budget_respected passed\n");
}

static void test_growth_and_reachability(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);
    size_t initial_cap = cj.cap;

    stub_poll_for_reply_hook = mock_poll;

    // push enough to grow
    const uint32_t N = 3000;
    for (uint32_t i = 1; i <= N; i++) {
        bool pushed = cookie_jar_push(&cj, i, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
        assert(pushed);
    }

    assert(cj.live_count == (size_t)N);
    assert(cj.cap >= initial_cap);

    // drain all
    for (uint32_t i = 1; i <= N; i++) {
        reset_handler_state();
        set_ready_reply(i);
        require_drain_until_called(&cj, 64, 64);
        assert(g_handler_seq == i);
    }

    assert(cj.live_count == 0);

    cookie_jar_destroy(&cj);
    printf("test_growth_and_reachability passed\n");
}

static void test_collisions_linear_probe(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    // Ensure we are at minimum cap so (seq & mask) trick works
    // If init starts larger, this still collides for many masks, but this is the simplest.
    // We only rely on the fact your index uses low bits with a power-of-2 mask.
    // If you change hashing, this test will still pass because it doesn't require a collision,
    // it just requires both entries to be drainable.
    stub_poll_for_reply_hook = mock_poll;

    bool p1 = cookie_jar_push(&cj, 1, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    bool p2 = cookie_jar_push(&cj, 17, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(p1 && p2);
    assert(cj.live_count == 2);

    // Drain 17 first
    reset_handler_state();
    set_ready_reply(17);
    require_drain_until_called(&cj, 16, 16);
    assert(g_handler_seq == 17);

    // Drain 1
    reset_handler_state();
    set_ready_reply(1);
    require_drain_until_called(&cj, 16, 16);
    assert(g_handler_seq == 1);

    cookie_jar_destroy(&cj);
    printf("test_collisions_linear_probe passed\n");
}

static void test_remove_breaks_chain_regression(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    // This test tries to catch the classic bug:
    // deleting an element in a probe chain without backshift/rehash breaks lookup for later elements.
    // We force a cluster by using many sequential seq values.
    stub_poll_for_reply_hook = mock_poll;

    const uint32_t base = 1;
    const uint32_t count = 64;

    for (uint32_t i = 0; i < count; i++) {
        bool ok = cookie_jar_push(&cj, base + i, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
        assert(ok);
    }
    assert(cj.live_count == count);

    // Drain one in the middle
    reset_handler_state();
    set_ready_reply(base + 10);
    require_drain_until_called(&cj, 64, 64);
    assert(g_handler_seq == base + 10);
    assert(cj.live_count == (size_t)(count - 1));

    // Now ensure later ones still drain
    for (uint32_t i = 11; i < count; i++) {
        reset_handler_state();
        set_ready_reply(base + i);
        require_drain_until_called(&cj, 64, 64);
        assert(g_handler_seq == base + i);
    }

    // Drain earlier ones too
    for (uint32_t i = 0; i < 10; i++) {
        reset_handler_state();
        set_ready_reply(base + i);
        require_drain_until_called(&cj, 64, 64);
        assert(g_handler_seq == base + i);
    }

    assert(cj.live_count == 0);

    cookie_jar_destroy(&cj);
    printf("test_remove_breaks_chain_regression passed\n");
}

static void test_error_path(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    stub_poll_for_reply_hook = mock_poll;

    reset_handler_state();
    bool pushed = cookie_jar_push(&cj, 500, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(pushed);

    // ready error, no reply
    set_ready_error(500);
    cookie_jar_drain(&cj, NULL, NULL, 10);

    assert(g_handler_called);
    assert(g_handler_seq == 500);
    assert(g_handler_reply == NULL);
    assert(g_handler_error != NULL);

    // cookie_jar_drain should free error after handler? depends on your contract
    // Your current test assumes reply is freed by jar; error too usually.
    // We only assert live_count dropped.
    assert(cj.live_count == 0);

    cookie_jar_destroy(&cj);
    printf("test_error_path passed\n");
}

static void test_reply_and_error_both(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    stub_poll_for_reply_hook = mock_poll;

    reset_handler_state();
    bool pushed = cookie_jar_push(&cj, 501, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(pushed);

    // ready both
    set_ready_both(501);
    cookie_jar_drain(&cj, NULL, NULL, 10);

    assert(g_handler_called);
    assert(g_handler_seq == 501);
    // policy choice: either you pass reply, or null it when error present
    // at minimum, it must not crash and must remove slot
    assert(cj.live_count == 0);

    cookie_jar_destroy(&cj);
    printf("test_reply_and_error_both passed\n");
}

static void test_timeout(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    stub_poll_for_reply_hook = mock_poll;
    reset_handler_state();

    g_use_mock_time = true;
    g_mock_time = 1000000000ULL;

    bool pushed = cookie_jar_push(&cj, 999, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(pushed);

    // not ready, +1s
    g_mock_time += 1000000000ULL;
    set_ready_none();
    cookie_jar_drain(&cj, NULL, NULL, 10);
    assert(!g_handler_called);
    assert(cj.live_count == 1);

    // +6s total elapsed
    g_mock_time += 5000000000ULL;
    cookie_jar_drain(&cj, NULL, NULL, 10);

    assert(g_handler_called);
    assert(g_handler_seq == 999);
    assert(g_handler_reply == NULL);
    assert(g_handler_error == NULL);
    assert(cj.live_count == 0);

    g_use_mock_time = false;
    cookie_jar_destroy(&cj);
    printf("test_timeout passed\n");
}

static void test_timeout_then_late_reply_ignored(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    stub_poll_for_reply_hook = mock_poll;

    g_use_mock_time = true;
    g_mock_time = 1000000000ULL;

    reset_handler_state();
    bool pushed = cookie_jar_push(&cj, 1001, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(pushed);

    // force timeout
    g_mock_time += 7000000000ULL;
    set_ready_none();
    cookie_jar_drain(&cj, NULL, NULL, 10);
    assert(g_handler_called);
    assert(g_handler_reply == NULL);
    assert(cj.live_count == 0);

    // now pretend reply arrives late; jar must not resurrect or call handler again
    reset_handler_state();
    set_ready_reply(1001);
    cookie_jar_drain(&cj, NULL, NULL, 10);
    assert(!g_handler_called);
    assert(cj.live_count == 0);

    g_use_mock_time = false;
    cookie_jar_destroy(&cj);
    printf("test_timeout_then_late_reply_ignored passed\n");
}

static void test_cursor_fairness_progress(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    stub_poll_for_reply_hook = mock_poll;

    // push sparse seq values so scan order is not trivially contiguous
    const uint32_t keys[] = {7, 100, 3, 9999, 42, 888, 5, 1234};
    const size_t n = sizeof(keys) / sizeof(keys[0]);

    for (size_t i = 0; i < n; i++) {
        bool ok = cookie_jar_push(&cj, keys[i], COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
        assert(ok);
    }
    assert(cj.live_count == n);

    // Mark only one key as ready; drain with small max_replies so we force cursor to move
    // If cursor never advances on non-ready slots, this can starve items depending on implementation.
    reset_handler_state();
    set_ready_reply(9999);

    require_drain_until_called(&cj, 128, 1);
    assert(g_handler_seq == 9999);

    cookie_jar_destroy(&cj);
    printf("test_cursor_fairness_progress passed\n");
}

static void test_performance_smoke(void) {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    struct timespec ts_start, ts_mid, ts_end;

    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    const int N = 20000;

    for (int i = 0; i < N; i++) {
        bool ok = cookie_jar_push(&cj, (uint32_t)(i + 1), COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
        assert(ok);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_mid);

    stub_poll_for_reply_hook = mock_poll;
    set_ready_none();

    cookie_jar_drain(&cj, NULL, NULL, N);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    uint64_t start = (uint64_t)ts_start.tv_sec * 1000000000ULL + (uint64_t)ts_start.tv_nsec;
    uint64_t mid = (uint64_t)ts_mid.tv_sec * 1000000000ULL + (uint64_t)ts_mid.tv_nsec;
    uint64_t end = (uint64_t)ts_end.tv_sec * 1000000000ULL + (uint64_t)ts_end.tv_nsec;

    printf("Performance: inserted %d in %" PRIu64 " ns (%.2f ns/item)\n", N, (mid - start),
           (double)(mid - start) / (double)N);
    printf("Performance: drained scan (none ready) in %" PRIu64 " ns\n", (end - mid));

    cookie_jar_destroy(&cj);
    printf("test_performance_smoke passed\n");
}

int main(void) {
    test_init_destroy();
    test_push_and_drain();
    test_duplicate_push_rejected_or_replaced();
    test_drain_budget_respected();
    test_growth_and_reachability();
    test_collisions_linear_probe();
    test_remove_breaks_chain_regression();
    test_error_path();
    test_reply_and_error_both();
    test_timeout();
    test_timeout_then_late_reply_ignored();
    test_cursor_fairness_progress();
    test_performance_smoke();

    printf("All cookie_jar tests passed\n");
    return 0;
}
