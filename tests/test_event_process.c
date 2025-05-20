#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "../src/wm_internal.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

// External stubs from xcb_stubs.c
extern void xcb_stubs_reset(void);
extern void atoms_init(xcb_connection_t* conn);
extern int stub_configure_window_count;
extern xcb_window_t stub_last_config_window;
extern uint16_t stub_last_config_mask;
extern int32_t stub_last_config_x;
extern int32_t stub_last_config_y;
extern uint32_t stub_last_config_w;
extern uint32_t stub_last_config_h;
extern int stub_prop_calls_len;

// Counters for wrapped functions
static int call_wm_handle_key_press = 0;
static int call_wm_handle_button_press = 0;
static int call_wm_handle_button_release = 0;
static int call_menu_handle_expose_region = 0;
static int call_frame_redraw_region = 0;
static int call_wm_handle_motion_notify = 0;
static int call_wm_update_monitors = 0;
static int call_wm_compute_workarea = 0;
static int call_wm_publish_workarea = 0;

// Wrappers
void __wrap_wm_handle_key_press(server_t* s, xcb_key_press_event_t* ev) {
    (void)s;
    (void)ev;
    call_wm_handle_key_press++;
}

void __wrap_wm_handle_button_press(server_t* s, xcb_button_press_event_t* ev) {
    (void)s;
    (void)ev;
    call_wm_handle_button_press++;
}

void __wrap_wm_handle_button_release(server_t* s, xcb_button_release_event_t* ev) {
    (void)s;
    (void)ev;
    call_wm_handle_button_release++;
}

void __wrap_menu_handle_expose_region(server_t* s, dirty_region_t* region) {
    (void)s;
    (void)region;
    call_menu_handle_expose_region++;
}

void __wrap_frame_redraw_region(server_t* s, handle_t h, dirty_region_t* region) {
    (void)s;
    (void)h;
    (void)region;
    call_frame_redraw_region++;
}

void __wrap_wm_handle_motion_notify(server_t* s, xcb_motion_notify_event_t* ev) {
    (void)s;
    (void)ev;
    call_wm_handle_motion_notify++;
}

void __wrap_wm_update_monitors(server_t* s) {
    (void)s;
    call_wm_update_monitors++;
}

void __wrap_wm_compute_workarea(server_t* s, rect_t* wa) {
    (void)s;
    (void)wa;
    call_wm_compute_workarea++;
}

void __wrap_wm_publish_workarea(server_t* s, const rect_t* wa) {
    (void)s;
    (void)wa;
    call_wm_publish_workarea++;
}

static void reset_counters(void) {
    call_wm_handle_key_press = 0;
    call_wm_handle_button_press = 0;
    call_wm_handle_button_release = 0;
    call_menu_handle_expose_region = 0;
    call_frame_redraw_region = 0;
    call_wm_handle_motion_notify = 0;
    call_wm_update_monitors = 0;
    call_wm_compute_workarea = 0;
    call_wm_publish_workarea = 0;
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

    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
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

    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);

    arena_destroy(&s->tick_arena);
    xcb_disconnect(s->conn);
}

static void test_6_1_key_press_dispatch(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();
    reset_counters();

    xcb_key_press_event_t* ev = arena_alloc(&s.tick_arena, sizeof(*ev));
    ev->response_type = XCB_KEY_PRESS;
    ev->detail = 10;
    small_vec_push(&s.buckets.key_presses, ev);

    event_process(&s);

    assert(call_wm_handle_key_press == 1);
    printf("test_6_1_key_press_dispatch passed\n");
    cleanup_server(&s);
}

static void test_6_2_button_events_dispatch(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();
    reset_counters();

    xcb_button_press_event_t* bp = arena_alloc(&s.tick_arena, sizeof(*bp));
    bp->response_type = XCB_BUTTON_PRESS;
    small_vec_push(&s.buckets.button_events, bp);

    xcb_button_release_event_t* br = arena_alloc(&s.tick_arena, sizeof(*br));
    br->response_type = XCB_BUTTON_RELEASE;
    small_vec_push(&s.buckets.button_events, br);

    event_process(&s);

    assert(call_wm_handle_button_press == 1);
    assert(call_wm_handle_button_release == 1);
    printf("test_6_2_button_events_dispatch passed\n");
    cleanup_server(&s);
}

static void test_6_3_menu_expose_dispatch(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();
    reset_counters();

    s.menu.window = 0xabc;

    dirty_region_t* region = arena_alloc(&s.tick_arena, sizeof(*region));
    *region = dirty_region_make(0, 0, 100, 100);
    hash_map_insert(&s.buckets.expose_regions, s.menu.window, region);

    event_process(&s);

    assert(call_menu_handle_expose_region == 1);
    assert(call_frame_redraw_region == 0);

    printf("test_6_3_menu_expose_dispatch passed\n");
    cleanup_server(&s);
}

static void test_6_4_motion_notify_dispatch(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();
    reset_counters();

    xcb_motion_notify_event_t* mn = arena_alloc(&s.tick_arena, sizeof(*mn));
    mn->event = 0x123;
    hash_map_insert(&s.buckets.motion_notifies, mn->event, mn);

    event_process(&s);

    assert(call_wm_handle_motion_notify == 1);

    printf("test_6_4_motion_notify_dispatch passed\n");
    cleanup_server(&s);
}

static void test_6_5_configure_request_unknown_window(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();
    reset_counters();

    xcb_window_t win = 0x999;
    pending_config_t* pc = arena_alloc(&s.tick_arena, sizeof(*pc));
    pc->window = win;
    pc->x = 50;
    pc->y = 60;
    pc->width = 200;
    pc->height = 150;
    pc->mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

    hash_map_insert(&s.buckets.configure_requests, win, pc);

    // No client registered for 0x999, so it is unknown.

    event_process(&s);

    assert(stub_configure_window_count == 1);
    assert(stub_last_config_window == win);
    assert(stub_last_config_x == 50);
    assert(stub_last_config_y == 60);
    assert(stub_last_config_w == 200);
    assert(stub_last_config_h == 150);

    printf("test_6_5_configure_request_unknown_window passed\n");
    cleanup_server(&s);
}

static void test_6_6_randr_dirty_processing(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();
    reset_counters();

    s.buckets.randr_dirty = true;
    s.buckets.randr_width = 1920;
    s.buckets.randr_height = 1080;

    event_process(&s);

    assert(call_wm_update_monitors == 1);
    assert(call_wm_compute_workarea == 1);
    assert(call_wm_publish_workarea == 1);

    // Check property change call (implied by wm_publish_workarea or explicit logic in event_process)
    // event_process explicitly calls xcb_change_property for _NET_DESKTOP_GEOMETRY
    // Let's check xcb stubs for that.
    assert(stub_prop_calls_len > 0);
    // We can iterate stub_prop_calls to find _NET_DESKTOP_GEOMETRY if we want, but method call counts are good proxies.

    printf("test_6_6_randr_dirty_processing passed\n");
    cleanup_server(&s);
}

int main(void) {
    test_6_1_key_press_dispatch();
    test_6_2_button_events_dispatch();
    test_6_3_menu_expose_dispatch();
    test_6_4_motion_notify_dispatch();
    test_6_5_configure_request_unknown_window();
    test_6_6_randr_dirty_processing();
    return 0;
}
