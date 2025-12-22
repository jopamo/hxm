#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "config.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern bool xcb_stubs_enqueue_queued_event(xcb_generic_event_t* ev);

#define HINT_WORDS ((sizeof(xcb_size_hints_t) + 3) / 4)
#define HINT_EXTRA_WORDS 4

static const size_t MAX_TITLE_BYTES = 4096;

static xcb_generic_event_t* make_reparent_event(xcb_window_t win, xcb_window_t parent, uint8_t override_redirect) {
    xcb_reparent_notify_event_t* ev = calloc(1, sizeof(*ev));
    ev->response_type = XCB_REPARENT_NOTIFY;
    ev->window = win;
    ev->parent = parent;
    ev->override_redirect = override_redirect;
    return (xcb_generic_event_t*)ev;
}

static void setup_server(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->is_test = true;
    s->root = 1;
    s->root_depth = 24;
    s->root_visual = 1;
    s->root_visual_type = xcb_get_visualtype(NULL, 0);
    s->conn = (xcb_connection_t*)malloc(1);
    config_init_defaults(&s->config);

    list_init(&s->focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s->layers[i]);

    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    cookie_jar_init(&s->cookie_jar);
    slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));

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
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (s->clients.hdr[i].live) {
            handle_t h = handle_make(i, s->clients.hdr[i].gen);
            client_hot_t* hot = server_chot(s, h);
            client_cold_t* cold = server_ccold(s, h);
            if (cold) {
                arena_destroy(&cold->string_arena);
                free(cold->colormap_windows);
                cold->colormap_windows = NULL;
                cold->colormap_windows_len = 0;
            }
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }

    cookie_jar_destroy(&s->cookie_jar);
    slotmap_destroy(&s->clients);
    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);

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
    free(s->conn);
}

static void test_wm_normal_hints_malformed_short(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms.WM_NORMAL_HINTS = 10;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    hot->xid = 4001;
    hot->state = STATE_NEW;
    hot->pending_replies = 1;
    arena_init(&cold->string_arena, 128);

    hot->hints_flags = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
    hot->hints.min_w = 123;
    hot->hints.min_h = 456;

    struct {
        xcb_get_property_reply_t r;
        uint32_t data[1];
    } reply;
    memset(&reply, 0, sizeof(reply));
    reply.r.format = 32;
    reply.r.type = XCB_ATOM_WM_SIZE_HINTS;
    reply.r.value_len = 1;

    cookie_slot_t slot = {0};
    slot.client = h;
    slot.type = COOKIE_GET_PROPERTY;
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_NORMAL_HINTS;

    wm_handle_reply(&s, &slot, &reply.r, NULL);

    assert(hot->hints_flags == XCB_ICCCM_SIZE_HINT_P_MIN_SIZE);
    assert(hot->hints.min_w == 123);
    assert(hot->hints.min_h == 456);

    printf("test_wm_normal_hints_malformed_short passed\n");
    cleanup_server(&s);
}

static void test_wm_normal_hints_malformed_long(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms.WM_NORMAL_HINTS = 11;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    hot->xid = 4002;
    hot->state = STATE_MAPPED;
    hot->pending_replies = 1;
    arena_init(&cold->string_arena, 128);

    xcb_size_hints_t hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
    hints.min_width = 200;
    hints.min_height = 150;

    struct {
        xcb_get_property_reply_t r;
        uint32_t data[HINT_WORDS + HINT_EXTRA_WORDS];
    } reply;
    memset(&reply, 0, sizeof(reply));
    reply.r.format = 32;
    reply.r.type = XCB_ATOM_WM_SIZE_HINTS;
    reply.r.value_len = (uint32_t)(HINT_WORDS + HINT_EXTRA_WORDS);
    memcpy(reply.data, &hints, sizeof(hints));

    cookie_slot_t slot = {0};
    slot.client = h;
    slot.type = COOKIE_GET_PROPERTY;
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_NORMAL_HINTS;

    wm_handle_reply(&s, &slot, &reply.r, NULL);

    assert((hot->hints_flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) != 0);
    assert(hot->hints.min_w == 200);
    assert(hot->hints.min_h == 150);

    printf("test_wm_normal_hints_malformed_long passed\n");
    cleanup_server(&s);
}

static void test_wm_name_huge_bounded(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms.WM_NAME = 12;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    hot->xid = 4003;
    hot->state = STATE_MAPPED;
    hot->pending_replies = 1;
    arena_init(&cold->string_arena, 512);

    size_t huge_len = 1024 * 1024;
    size_t total = sizeof(xcb_get_property_reply_t) + huge_len;
    xcb_get_property_reply_t* rep = calloc(1, total);
    assert(rep != NULL);
    rep->format = 8;
    rep->type = XCB_ATOM_STRING;
    rep->value_len = (uint32_t)huge_len;
    memset(xcb_get_property_value(rep), 'A', huge_len);

    cookie_slot_t slot = {0};
    slot.client = h;
    slot.type = COOKIE_GET_PROPERTY;
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_NAME;

    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);

    assert(cold->base_title != NULL);
    assert(strlen(cold->base_title) == MAX_TITLE_BYTES);

    printf("test_wm_name_huge_bounded passed\n");
    cleanup_server(&s);
}

static void test_override_redirect_midstream_aborts_manage(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    hot->xid = 4004;
    hot->state = STATE_NEW;
    hot->manage_phase = MANAGE_PHASE1;
    hot->pending_replies = 1;
    arena_init(&cold->string_arena, 128);

    xcb_get_window_attributes_reply_t r;
    memset(&r, 0, sizeof(r));
    r.override_redirect = 1;
    r.visual = s.root_visual;

    cookie_slot_t slot = {0};
    slot.client = h;
    slot.type = COOKIE_GET_WINDOW_ATTRIBUTES;
    slot.data = hot->xid;

    wm_handle_reply(&s, &slot, &r, NULL);
    assert(hot->manage_aborted);

    printf("test_override_redirect_midstream_aborts_manage passed\n");
    cleanup_server(&s);
}

static void test_map_unmap_property_race(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms.WM_NAME = 20;
    atoms.WM_STATE = 21;
    atoms._NET_WM_STATE = 22;
    atoms._NET_WM_DESKTOP = 23;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    hot->self = h;
    hot->xid = 4005;
    hot->state = STATE_MAPPED;
    hot->server.x = 10;
    hot->server.y = 10;
    hot->server.w = 100;
    hot->server.h = 100;
    list_init(&hot->focus_node);
    list_init(&hot->transient_sibling);
    list_init(&hot->transients_head);
    arena_init(&cold->string_arena, 128);

    hash_map_insert(&s.window_to_client, hot->xid, handle_to_ptr(h));

    xcb_unmap_notify_event_t* unmap = calloc(1, sizeof(*unmap));
    unmap->response_type = XCB_UNMAP_NOTIFY;
    unmap->event = s.root;
    unmap->window = hot->xid;
    small_vec_push(&s.buckets.unmap_notifies, unmap);

    xcb_property_notify_event_t* prop = calloc(1, sizeof(*prop));
    prop->response_type = XCB_PROPERTY_NOTIFY;
    prop->window = hot->xid;
    prop->atom = atoms.WM_NAME;
    uint64_t key = ((uint64_t)prop->window << 32) | prop->atom;
    hash_map_insert(&s.buckets.property_notifies, key, prop);

    event_process(&s);

    assert(server_get_client_by_window(&s, hot->xid) == HANDLE_INVALID);

    printf("test_map_unmap_property_race passed\n");
    cleanup_server(&s);
}

static void test_reparent_notify_self_ignored(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    hot->self = h;
    hot->xid = 4006;
    hot->state = STATE_MAPPED;
    list_init(&hot->focus_node);
    list_init(&hot->transient_sibling);
    list_init(&hot->transients_head);
    arena_init(&cold->string_arena, 128);

    hash_map_insert(&s.window_to_client, hot->xid, handle_to_ptr(h));

    assert(xcb_stubs_enqueue_queued_event(make_reparent_event(hot->xid, hot->xid, 1)));
    event_ingest(&s, false);
    event_process(&s);

    assert(server_get_client_by_window(&s, hot->xid) == h);

    printf("test_reparent_notify_self_ignored passed\n");
    cleanup_server(&s);
}

int main(void) {
    test_wm_normal_hints_malformed_short();
    test_wm_normal_hints_malformed_long();
    test_wm_name_huge_bounded();
    test_override_redirect_midstream_aborts_manage();
    test_map_unmap_property_race();
    test_reparent_notify_self_ignored();
    return 0;
}
