#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "event.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern bool xcb_stubs_enqueue_queued_event(xcb_generic_event_t* ev);
extern bool xcb_stubs_enqueue_event(xcb_generic_event_t* ev);
extern size_t xcb_stubs_queued_event_len(void);
extern size_t xcb_stubs_event_len(void);

static xcb_generic_event_t* make_event(uint8_t type) {
    xcb_generic_event_t* ev = calloc(1, sizeof(*ev));
    ev->response_type = type;
    return ev;
}

static void setup_server(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->conn = xcb_connect(NULL, NULL);
    atoms_init(s->conn);
    arena_init(&s->tick_arena, 1024);

    small_vec_init(&s->buckets.map_requests);
    small_vec_init(&s->buckets.unmap_notifies);
    small_vec_init(&s->buckets.destroy_notifies);
    small_vec_init(&s->buckets.key_presses);
    small_vec_init(&s->buckets.button_events);
    small_vec_init(&s->buckets.client_messages);

    hash_map_init(&s->buckets.expose_regions);
    hash_map_init(&s->buckets.configure_requests);
    hash_map_init(&s->buckets.configure_notifies);
    hash_map_init(&s->buckets.destroyed_windows);
    hash_map_init(&s->buckets.property_notifies);
    hash_map_init(&s->buckets.motion_notifies);
    hash_map_init(&s->buckets.damage_regions);
}

static void cleanup_server(server_t* s) {
    small_vec_destroy(&s->buckets.map_requests);
    small_vec_destroy(&s->buckets.unmap_notifies);
    small_vec_destroy(&s->buckets.destroy_notifies);
    small_vec_destroy(&s->buckets.key_presses);
    small_vec_destroy(&s->buckets.button_events);
    small_vec_destroy(&s->buckets.client_messages);

    hash_map_destroy(&s->buckets.expose_regions);
    hash_map_destroy(&s->buckets.configure_requests);
    hash_map_destroy(&s->buckets.configure_notifies);
    hash_map_destroy(&s->buckets.destroyed_windows);
    hash_map_destroy(&s->buckets.property_notifies);
    hash_map_destroy(&s->buckets.motion_notifies);
    hash_map_destroy(&s->buckets.damage_regions);

    arena_destroy(&s->tick_arena);
    xcb_disconnect(s->conn);
}

static void test_event_ingest_bounded(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    const int extra = 4;
    for (int i = 0; i < MAX_EVENTS_PER_TICK + extra; i++) {
        assert(xcb_stubs_enqueue_queued_event(make_event(XCB_KEY_PRESS)));
    }

    event_ingest(&s, false);

    assert(s.buckets.ingested == MAX_EVENTS_PER_TICK);
    assert(s.x_poll_immediate == true);
    assert(xcb_stubs_queued_event_len() == (size_t)extra);

    printf("test_event_ingest_bounded passed\n");
    xcb_stubs_reset();
    cleanup_server(&s);
}

static void test_event_ingest_drains_all_when_ready(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    assert(xcb_stubs_enqueue_queued_event(make_event(XCB_KEY_PRESS)));
    assert(xcb_stubs_enqueue_event(make_event(XCB_BUTTON_PRESS)));

    event_ingest(&s, true);

    assert(s.buckets.ingested == 2);
    assert(s.x_poll_immediate == false);
    assert(xcb_stubs_queued_event_len() == 0);
    assert(xcb_stubs_event_len() == 0);

    printf("test_event_ingest_drains_all_when_ready passed\n");
    xcb_stubs_reset();
    cleanup_server(&s);
}

int main(void) {
    test_event_ingest_bounded();
    test_event_ingest_drains_all_when_ready();
    return 0;
}
