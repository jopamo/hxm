#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/damage.h>
#include <xcb/xproto.h>

#include "event.h"
#include "hxm.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern bool xcb_stubs_enqueue_event(xcb_generic_event_t* ev);

static void setup_server(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->is_test = true;
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
    arena_destroy(&s->tick_arena);
    hash_map_destroy(&s->buckets.expose_regions);
    hash_map_destroy(&s->buckets.configure_requests);
    hash_map_destroy(&s->buckets.configure_notifies);
    hash_map_destroy(&s->buckets.destroyed_windows);
    hash_map_destroy(&s->buckets.property_notifies);
    hash_map_destroy(&s->buckets.motion_notifies);
    hash_map_destroy(&s->buckets.damage_regions);
    xcb_disconnect(s->conn);
}

static void test_expose_coalesces_regions(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    xcb_expose_event_t* e1 = calloc(1, sizeof(*e1));
    e1->response_type = XCB_EXPOSE;
    e1->window = 10;
    e1->x = 10;
    e1->y = 10;
    e1->width = 20;
    e1->height = 20;

    xcb_expose_event_t* e2 = calloc(1, sizeof(*e2));
    e2->response_type = XCB_EXPOSE;
    e2->window = 10;
    e2->x = 25;
    e2->y = 5;
    e2->width = 10;
    e2->height = 10;

    assert(xcb_stubs_enqueue_event((xcb_generic_event_t*)e1));
    assert(xcb_stubs_enqueue_event((xcb_generic_event_t*)e2));

    event_ingest(&s, true);

    dirty_region_t* region = hash_map_get(&s.buckets.expose_regions, 10);
    assert(region != NULL);
    assert(region->valid);
    assert(region->x == 10);
    assert(region->y == 5);
    assert(region->w == 25);
    assert(region->h == 25);

    printf("test_expose_coalesces_regions passed\n");
    cleanup_server(&s);
}

static void test_damage_coalesces_regions(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    s.damage_supported = true;
    s.damage_event_base = 0;

    xcb_damage_notify_event_t* d1 = calloc(1, sizeof(*d1));
    d1->response_type = XCB_DAMAGE_NOTIFY;
    d1->drawable = 99;
    d1->area.x = 0;
    d1->area.y = 0;
    d1->area.width = 50;
    d1->area.height = 20;

    xcb_damage_notify_event_t* d2 = calloc(1, sizeof(*d2));
    d2->response_type = XCB_DAMAGE_NOTIFY;
    d2->drawable = 99;
    d2->area.x = 40;
    d2->area.y = 10;
    d2->area.width = 20;
    d2->area.height = 30;

    assert(xcb_stubs_enqueue_event((xcb_generic_event_t*)d1));
    assert(xcb_stubs_enqueue_event((xcb_generic_event_t*)d2));

    event_ingest(&s, true);

    dirty_region_t* region = hash_map_get(&s.buckets.damage_regions, 99);
    assert(region != NULL);
    assert(region->valid);
    assert(region->x == 0);
    assert(region->y == 0);
    assert(region->w == 60);
    assert(region->h == 40);

    printf("test_damage_coalesces_regions passed\n");
    cleanup_server(&s);
}

static void test_motion_coalesces_last_event(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    xcb_motion_notify_event_t* m1 = calloc(1, sizeof(*m1));
    m1->response_type = XCB_MOTION_NOTIFY;
    m1->event = 42;
    m1->root_x = 10;
    m1->root_y = 10;

    xcb_motion_notify_event_t* m2 = calloc(1, sizeof(*m2));
    m2->response_type = XCB_MOTION_NOTIFY;
    m2->event = 42;
    m2->root_x = 50;
    m2->root_y = 60;

    assert(xcb_stubs_enqueue_event((xcb_generic_event_t*)m1));
    assert(xcb_stubs_enqueue_event((xcb_generic_event_t*)m2));

    event_ingest(&s, true);

    xcb_motion_notify_event_t* last = hash_map_get(&s.buckets.motion_notifies, 42);
    assert(last != NULL);
    assert(last->root_x == 50);
    assert(last->root_y == 60);

    printf("test_motion_coalesces_last_event passed\n");
    cleanup_server(&s);
}

int main(void) {
    test_expose_coalesces_regions();
    test_damage_coalesces_regions();
    test_motion_coalesces_last_event();
    return 0;
}
