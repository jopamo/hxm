#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/damage.h>
#include <xcb/randr.h>

#include "event.h"

extern void xcb_stubs_reset(void);
extern void atoms_init(xcb_connection_t* conn);
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

static void test_event_ingest_coalesces_configure_request(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    xcb_window_t win = 0x12345;

    // Event 1: X, Y, WIDTH
    xcb_configure_request_event_t* ev1 = calloc(1, sizeof(*ev1));
    ev1->response_type = XCB_CONFIGURE_REQUEST;
    ev1->window = win;
    ev1->value_mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH;
    ev1->x = 100;
    ev1->y = 200;
    ev1->width = 300;

    // Event 2: HEIGHT (same window)
    xcb_configure_request_event_t* ev2 = calloc(1, sizeof(*ev2));
    ev2->response_type = XCB_CONFIGURE_REQUEST;
    ev2->window = win;
    ev2->value_mask = XCB_CONFIG_WINDOW_HEIGHT;
    ev2->height = 400;

    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)ev1));
    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)ev2));

    event_ingest(&s, false);

    assert(hash_map_size(&s.buckets.configure_requests) == 1);

    pending_config_t* pc = hash_map_get(&s.buckets.configure_requests, win);
    assert(pc != NULL);
    assert(pc->mask ==
           (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT));
    assert(pc->x == 100);
    assert(pc->y == 200);
    assert(pc->width == 300);
    assert(pc->height == 400);
    assert(s.buckets.coalesced == 1);

    printf("test_event_ingest_coalesces_configure_request passed\n");
    xcb_stubs_reset();
    cleanup_server(&s);
}

static void test_event_ingest_coalesces_randr(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    s.randr_supported = true;
    s.randr_event_base = 100;

    // Event 1: Width=800, Height=600
    xcb_randr_screen_change_notify_event_t* ev1 = calloc(1, sizeof(*ev1));
    ev1->response_type = (uint8_t)(s.randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY);
    ev1->width = 800;
    ev1->height = 600;

    // Event 2: Width=1024, Height=768
    xcb_randr_screen_change_notify_event_t* ev2 = calloc(1, sizeof(*ev2));
    ev2->response_type = (uint8_t)(s.randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY);
    ev2->width = 1024;
    ev2->height = 768;

    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)ev1));
    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)ev2));

    event_ingest(&s, false);

    assert(s.buckets.randr_dirty == true);
    assert(s.buckets.randr_width == 1024);
    assert(s.buckets.randr_height == 768);
    assert(s.buckets.coalesced == 1);

    printf("test_event_ingest_coalesces_randr passed\n");
    xcb_stubs_reset();
    cleanup_server(&s);
}

static void test_event_ingest_coalesces_pointer_notify(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    // Two EnterNotify
    xcb_enter_notify_event_t* e1 = calloc(1, sizeof(*e1));
    e1->response_type = XCB_ENTER_NOTIFY;
    e1->event = 0x111;

    xcb_enter_notify_event_t* e2 = calloc(1, sizeof(*e2));
    e2->response_type = XCB_ENTER_NOTIFY;
    e2->event = 0x222;

    // Two LeaveNotify
    xcb_leave_notify_event_t* l1 = calloc(1, sizeof(*l1));
    l1->response_type = XCB_LEAVE_NOTIFY;
    l1->event = 0x333;

    xcb_leave_notify_event_t* l2 = calloc(1, sizeof(*l2));
    l2->response_type = XCB_LEAVE_NOTIFY;
    l2->event = 0x444;

    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)e1));
    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)e2));
    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)l1));
    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)l2));

    event_ingest(&s, false);

    assert(s.buckets.pointer_notify.enter_valid == true);
    assert(s.buckets.pointer_notify.enter.event == 0x222);
    assert(s.buckets.pointer_notify.leave_valid == true);
    assert(s.buckets.pointer_notify.leave.event == 0x444);
    assert(s.buckets.coalesced == 2);

    printf("test_event_ingest_coalesces_pointer_notify passed\n");
    xcb_stubs_reset();
    cleanup_server(&s);
}

static void test_event_ingest_dispatches_colormap_notify(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    // We need a focused client for wm_handle_colormap_notify to do anything,
    // but here we just want to see it doesn't crash and maybe hits the branch.
    // Since we don't have a mock for wm_handle_colormap_notify, we just call it.
    xcb_colormap_notify_event_t* ev = calloc(1, sizeof(*ev));
    ev->response_type = XCB_COLORMAP_NOTIFY;
    ev->window = 0x123;
    ev->colormap = 0x456;

    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)ev));

    event_ingest(&s, false);

    // XCB_COLORMAP_NOTIFY is dispatched immediately in event_ingest_one,
    // it doesn't go to a bucket.
    assert(s.buckets.ingested == 1);

    printf("test_event_ingest_dispatches_colormap_notify passed\n");
    xcb_stubs_reset();
    cleanup_server(&s);
}

static void test_event_ingest_coalesces_damage(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    s.damage_supported = true;
    s.damage_event_base = 110;
    xcb_window_t win = 0x789;

    // Event 1: (0,0, 10x10)
    xcb_damage_notify_event_t* ev1 = calloc(1, sizeof(*ev1));
    ev1->response_type = (uint8_t)(s.damage_event_base + XCB_DAMAGE_NOTIFY);
    ev1->drawable = win;
    ev1->area.x = 0;
    ev1->area.y = 0;
    ev1->area.width = 10;
    ev1->area.height = 10;

    // Event 2: (5,5, 10x10) -> Union should be (0,0, 15x15)
    xcb_damage_notify_event_t* ev2 = calloc(1, sizeof(*ev2));
    ev2->response_type = (uint8_t)(s.damage_event_base + XCB_DAMAGE_NOTIFY);
    ev2->drawable = win;
    ev2->area.x = 5;
    ev2->area.y = 5;
    ev2->area.width = 10;
    ev2->area.height = 10;

    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)ev1));
    assert(xcb_stubs_enqueue_queued_event((xcb_generic_event_t*)ev2));

    event_ingest(&s, false);

    assert(hash_map_size(&s.buckets.damage_regions) == 1);
    dirty_region_t* region = hash_map_get(&s.buckets.damage_regions, win);
    assert(region != NULL);
    assert(region->x == 0);
    assert(region->y == 0);
    assert(region->w == 15);
    assert(region->h == 15);
    assert(s.buckets.coalesced == 1);

    printf("test_event_ingest_coalesces_damage passed\n");
    xcb_stubs_reset();
    cleanup_server(&s);
}

int main(void) {
    test_event_ingest_bounded();
    test_event_ingest_drains_all_when_ready();
    test_event_ingest_coalesces_configure_request();
    test_event_ingest_coalesces_randr();
    test_event_ingest_coalesces_pointer_notify();
    test_event_ingest_dispatches_colormap_notify();
    test_event_ingest_coalesces_damage();
    return 0;
}
