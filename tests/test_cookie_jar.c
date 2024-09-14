#include <assert.h>
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

static void reset_handler_state() {
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

// Mock poll hook
static uint32_t g_ready_seq = 0;
static void* g_ready_reply = NULL;
static xcb_generic_error_t* g_ready_error = NULL;

static int mock_poll(xcb_connection_t* c, unsigned int request, void** reply, xcb_generic_error_t** error) {
    (void)c;
    if (g_ready_seq == request) {
        if (g_ready_reply) {
            // Allocate a copy because cookie_jar_drain will free it
            *reply = malloc(1);  // minimal dummy
        } else {
            *reply = NULL;
        }

        if (g_ready_error) {
            *error = malloc(sizeof(xcb_generic_error_t));
        } else {
            *error = NULL;
        }
        return 1;
    }
    return 0;
}

void test_init_destroy() {
    cookie_jar_t cj;
    cookie_jar_init(&cj);
    assert(cj.cap >= 16);
    assert(cj.live_count == 0);
    assert(cj.slots != NULL);
    cookie_jar_destroy(&cj);
    assert(cj.slots == NULL);
    printf("test_init_destroy passed.\n");
}

void test_push_and_drain() {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    reset_handler_state();
    stub_poll_for_reply_hook = mock_poll;

    // Push a cookie
    uint32_t seq = 123;
    bool pushed = cookie_jar_push(&cj, seq, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(pushed);
    assert(cj.live_count == 1);

    // Drain - not ready
    g_ready_seq = 0;
    cookie_jar_drain(&cj, NULL, NULL, 10);
    assert(g_handler_called == false);
    assert(cj.live_count == 1);

    // Drain - ready
    g_ready_seq = seq;
    g_ready_reply = (void*)1;  // Just non-null
    cookie_jar_drain(&cj, NULL, NULL, 10);

    assert(g_handler_called == true);
    assert(g_handler_seq == seq);
    assert(g_handler_reply != NULL);
    assert(cj.live_count == 0);

    cookie_jar_destroy(&cj);
    printf("test_push_and_drain passed.\n");
}

void test_growth() {
    cookie_jar_t cj;
    cookie_jar_init(&cj);
    size_t initial_cap = cj.cap;

    // Push many items to force growth
    // Default cap is 1024. Load factor 0.7. Threshold ~716.
    // Push 2000 items.
    int N = 2000;
    for (uint32_t i = 1; i <= (uint32_t)N; i++) {
        bool pushed = cookie_jar_push(&cj, i, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
        assert(pushed);
    }

    assert(cj.cap > initial_cap);
    assert(cj.live_count == (size_t)N);

    // Verify all are present
    stub_poll_for_reply_hook = mock_poll;

    // Drain one by one
    for (uint32_t i = 1; i <= (uint32_t)N; i++) {
        reset_handler_state();
        g_ready_seq = i;
        g_ready_reply = (void*)1;

        // We might need to call drain multiple times if it scans strictly
        // cookie_jar_drain scans up to cap or max_replies.
        // It resumes from cursor.

        // Let's loop until handled
        int attempts = 0;
        while (!g_handler_called && attempts < 10) {
            cookie_jar_drain(&cj, NULL, NULL, 10);
            attempts++;
        }

        if (!g_handler_called) {
            printf("Failed to drain seq %u\n", i);
        }
        assert(g_handler_called);
        assert(g_handler_seq == i);
    }

    assert(cj.live_count == 0);

    cookie_jar_destroy(&cj);
    printf("test_growth passed.\n");
}

void test_collisions() {
    cookie_jar_t cj;
    cookie_jar_init(&cj);
    // Force small cap for collision testing
    // But init enforces min 16.
    // Mask is 15 (0xF).
    // Seq 1 -> index 1
    // Seq 17 -> index 1 (17 & 15 = 1) -> Collision!

    bool p1 = cookie_jar_push(&cj, 1, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(p1);

    bool p2 = cookie_jar_push(&cj, 17, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(p2);

    assert(cj.live_count == 2);

    // Drain 17 first
    g_ready_seq = 17;
    g_ready_reply = (void*)1;
    reset_handler_state();

    int attempts = 0;
    while (!g_handler_called && attempts < 5) {
        cookie_jar_drain(&cj, NULL, NULL, 10);
        attempts++;
    }
    assert(g_handler_called);
    assert(g_handler_seq == 17);

    // Drain 1
    g_ready_seq = 1;
    g_ready_reply = (void*)1;
    reset_handler_state();

    attempts = 0;
    while (!g_handler_called && attempts < 5) {
        cookie_jar_drain(&cj, NULL, NULL, 10);
        attempts++;
    }
    assert(g_handler_called);
    assert(g_handler_seq == 1);

    cookie_jar_destroy(&cj);
    printf("test_collisions passed.\n");
}

static uint64_t g_mock_time = 0;
static bool g_use_mock_time = false;

uint64_t monotonic_time_ns(void) {
    if (g_use_mock_time) {
        return g_mock_time;
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void test_timeout() {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    stub_poll_for_reply_hook = mock_poll;
    reset_handler_state();

    g_use_mock_time = true;
    g_mock_time = 1000000000ULL;  // Start at 1s

    // Push a cookie
    bool pushed = cookie_jar_push(&cj, 999, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    assert(pushed);

    // Drain - time hasn't passed enough (timeout is 5s)
    g_mock_time += 1000000000ULL;  // +1s = 2s. Elapsed 1s.
    g_ready_seq = 0;
    cookie_jar_drain(&cj, NULL, NULL, 10);
    assert(!g_handler_called);
    assert(cj.live_count == 1);

    // Drain - time passed (timeout 5s)
    g_mock_time += 5000000000ULL;  // +5s = 7s. Elapsed 6s total from push.
    cookie_jar_drain(&cj, NULL, NULL, 10);

    assert(g_handler_called);
    assert(g_handler_reply == NULL);  // Timeout returns NULL reply
    assert(g_handler_error == NULL);
    assert(cj.live_count == 0);

    g_use_mock_time = false;
    cookie_jar_destroy(&cj);
    printf("test_timeout passed.\n");
}

void test_performance() {
    cookie_jar_t cj;
    cookie_jar_init(&cj);

    // Use direct clock_gettime for measurement to avoid our mock overhead/logic
    struct timespec ts_start, ts_mid, ts_end;

    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    int N = 10000;

    for (int i = 0; i < N; i++) {
        cookie_jar_push(&cj, i + 1, COOKIE_GET_GEOMETRY, HANDLE_INVALID, 0, mock_handler);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_mid);

    stub_poll_for_reply_hook = mock_poll;
    g_ready_seq = 0;  // None ready initially
    g_ready_reply = (void*)1;

    // This is just measuring scan overhead if nothing is ready
    cookie_jar_drain(&cj, NULL, NULL, N);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    uint64_t start = (uint64_t)ts_start.tv_sec * 1000000000ULL + ts_start.tv_nsec;
    uint64_t mid = (uint64_t)ts_mid.tv_sec * 1000000000ULL + ts_mid.tv_nsec;
    uint64_t end = (uint64_t)ts_end.tv_sec * 1000000000ULL + ts_end.tv_nsec;

    printf("Performance: Inserted %d items in %lu ns (%.2f ns/item)\n", N, (mid - start), (double)(mid - start) / N);
    printf("Performance: Scanned %d items (none ready) in %lu ns\n", N, (end - mid));

    cookie_jar_destroy(&cj);
    printf("test_performance passed.\n");
}

int main() {
    test_init_destroy();
    test_push_and_drain();
    test_growth();
    test_collisions();
    test_timeout();
    test_performance();

    printf("All cookie_jar tests passed!\n");
    return 0;
}
