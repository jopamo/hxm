#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xproto.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern void xcb_stubs_set_query_tree_children(const xcb_window_t* children, int len);
extern bool xcb_stubs_attr_request_window(uint32_t seq, xcb_window_t* out_window);
extern int (*stub_poll_for_reply_hook)(xcb_connection_t* c, unsigned int request, void** reply,
                                       xcb_generic_error_t** error);
extern int stub_map_window_count;
extern int stub_unmap_window_count;
extern int stub_mapped_windows_len;
extern xcb_window_t stub_mapped_windows[256];
extern bool xcb_stubs_enqueue_event(xcb_generic_event_t* ev);
extern int stub_destroy_window_count;
extern xcb_window_t stub_last_destroyed_window;

static void setup_server(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->is_test = true;

    xcb_stubs_reset();
    s->conn = xcb_connect(NULL, NULL);
    atoms_init(s->conn);

    s->root = 1;
    s->root_visual = 1;
    s->root_depth = 24;
    s->root_visual_type = xcb_get_visualtype(s->conn, 0);

    s->config.theme.border_width = 1;
    s->config.theme.title_height = 10;

    cookie_jar_init(&s->cookie_jar);
    slotmap_init(&s->clients, 64, sizeof(client_hot_t), sizeof(client_cold_t));
    small_vec_init(&s->active_clients);
    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    list_init(&s->focus_history);

    for (int i = 0; i < LAYER_COUNT; i++) {
        small_vec_init(&s->layers[i]);
    }
}

static void cleanup_server(server_t* s) {
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (s->clients.hdr[i].live) {
            handle_t h = handle_make(i, s->clients.hdr[i].gen);
            client_hot_t* hot = server_chot(s, h);
            client_cold_t* cold = server_ccold(s, h);
            if (cold) {
                arena_destroy(&cold->string_arena);
            }
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    cookie_jar_destroy(&s->cookie_jar);
    slotmap_destroy(&s->clients);
    small_vec_destroy(&s->active_clients);
    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);
    xcb_disconnect(s->conn);
}

typedef struct adopt_attr {
    xcb_window_t window;
    bool override_redirect;
    uint8_t map_state;
} adopt_attr_t;

static adopt_attr_t g_adopt_attrs[8];
static int g_adopt_attrs_len = 0;
static xcb_window_t g_map_request_window = XCB_NONE;

static int adopt_poll_for_reply(xcb_connection_t* c, unsigned int request, void** reply, xcb_generic_error_t** error) {
    (void)c;
    if (error) *error = NULL;

    xcb_window_t win = XCB_NONE;
    if (!xcb_stubs_attr_request_window(request, &win)) return 0;

    for (int i = 0; i < g_adopt_attrs_len; i++) {
        if (g_adopt_attrs[i].window == win) {
            xcb_get_window_attributes_reply_t* r = calloc(1, sizeof(*r));
            r->override_redirect = g_adopt_attrs[i].override_redirect ? 1 : 0;
            r->map_state = g_adopt_attrs[i].map_state;
            if (reply) *reply = r;
            return 1;
        }
    }

    return 0;
}

static int map_request_poll_for_reply(xcb_connection_t* c, unsigned int request, void** reply,
                                      xcb_generic_error_t** error) {
    (void)c;
    if (error) *error = NULL;

    xcb_window_t win = XCB_NONE;
    if (!xcb_stubs_attr_request_window(request, &win)) return 0;
    if (win != g_map_request_window) return 0;

    xcb_get_window_attributes_reply_t* r = calloc(1, sizeof(*r));
    r->override_redirect = 0;
    r->map_state = XCB_MAP_STATE_UNMAPPED;
    r->_class = XCB_WINDOW_CLASS_INPUT_OUTPUT;
    if (reply) *reply = r;
    return 1;
}

static void test_adopt_children_skips_override_and_unmapped(void) {
    server_t s;
    setup_server(&s);

    xcb_window_t supporting = 9000;
    s.supporting_wm_check = supporting;

    xcb_window_t w1 = 1001;
    xcb_window_t w2 = 1002;
    xcb_window_t w3 = 1003;
    xcb_window_t children[] = {supporting, w1, w2, w3};
    xcb_stubs_set_query_tree_children(children, 4);

    g_adopt_attrs_len = 0;
    g_adopt_attrs[g_adopt_attrs_len++] = (adopt_attr_t){w1, false, XCB_MAP_STATE_VIEWABLE};
    g_adopt_attrs[g_adopt_attrs_len++] = (adopt_attr_t){w2, true, XCB_MAP_STATE_VIEWABLE};
    g_adopt_attrs[g_adopt_attrs_len++] = (adopt_attr_t){w3, false, XCB_MAP_STATE_UNMAPPED};

    stub_poll_for_reply_hook = adopt_poll_for_reply;

    wm_adopt_children(&s);
    cookie_jar_drain(&s.cookie_jar, s.conn, &s, 32);

    assert(server_get_client_by_window(&s, w1) != HANDLE_INVALID);
    assert(server_get_client_by_window(&s, w2) == HANDLE_INVALID);
    assert(server_get_client_by_window(&s, w3) == HANDLE_INVALID);

    printf("test_adopt_children_skips_override_and_unmapped passed\n");
    cleanup_server(&s);
}

static size_t count_live_clients(server_t* s) {
    size_t count = 0;
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (s->clients.hdr[i].live) count++;
    }
    return count;
}

static bool cookie_jar_has_atom(cookie_jar_t* cj, xcb_atom_t atom) {
    if (!cj || !cj->slots) return false;
    for (size_t i = 0; i < cj->cap; i++) {
        const cookie_slot_t* slot = &cj->slots[i];
        if (!slot->live) continue;
        if (slot->type != COOKIE_GET_PROPERTY) continue;
        xcb_atom_t slot_atom = (xcb_atom_t)(slot->data & 0xFFFFFFFFu);
        if (slot_atom == atom) return true;
    }
    return false;
}

static void test_map_request_starts_manage_once(void) {
    server_t s;
    setup_server(&s);

    xcb_map_request_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window = 1234;
    ev.parent = s.root;

    wm_handle_map_request(&s, &ev);
    g_map_request_window = ev.window;
    stub_poll_for_reply_hook = map_request_poll_for_reply;
    cookie_jar_drain(&s.cookie_jar, s.conn, &s, 8);
    handle_t h = server_get_client_by_window(&s, ev.window);
    assert(h != HANDLE_INVALID);

    size_t live_before = count_live_clients(&s);
    wm_handle_map_request(&s, &ev);
    handle_t h2 = server_get_client_by_window(&s, ev.window);
    assert(h2 == h);
    assert(count_live_clients(&s) == live_before);

    printf("test_map_request_starts_manage_once passed\n");
    stub_poll_for_reply_hook = NULL;
    cleanup_server(&s);
}

static void test_finish_manage_maps_client_then_frame(void) {
    server_t s;
    setup_server(&s);

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));

    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 512);

    hot->self = h;
    hot->xid = 2001;
    hot->state = STATE_NEW;
    hot->type = WINDOW_TYPE_NORMAL;
    hot->focus_override = -1;
    hot->transient_for = HANDLE_INVALID;
    hot->desktop = 0;
    hot->desired = (rect_t){0, 0, 100, 80};
    hot->visual_id = s.root_visual;
    hot->depth = s.root_depth;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s.window_to_client, hot->xid, handle_to_ptr(h));

    stub_mapped_windows_len = 0;
    client_finish_manage(&s, h);

    assert(stub_mapped_windows_len == 2);
    assert(stub_mapped_windows[0] == hot->xid);
    assert(stub_mapped_windows[1] == hot->frame);

    printf("test_finish_manage_maps_client_then_frame passed\n");
    cleanup_server(&s);
}

static void test_finish_manage_ignores_reparent_unmap(void) {
    server_t s;
    setup_server(&s);

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));

    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 512);

    hot->self = h;
    hot->xid = 2101;
    hot->state = STATE_NEW;
    hot->type = WINDOW_TYPE_NORMAL;
    hot->focus_override = -1;
    hot->transient_for = HANDLE_INVALID;
    hot->desktop = 0;
    hot->desired = (rect_t){0, 0, 100, 80};
    hot->visual_id = s.root_visual;
    hot->depth = s.root_depth;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s.window_to_client, hot->xid, handle_to_ptr(h));

    client_finish_manage(&s, h);
    assert(hot->ignore_unmap > 0);

    xcb_unmap_notify_event_t unmap;
    memset(&unmap, 0, sizeof(unmap));
    unmap.window = hot->xid;
    unmap.event = s.root;
    wm_handle_unmap_notify(&s, &unmap);

    assert(server_get_client_by_window(&s, hot->xid) == h);

    printf("test_finish_manage_ignores_reparent_unmap passed\n");
    cleanup_server(&s);
}

static void test_map_request_maps_and_stays_mapped(void) {
    server_t s;
    setup_server(&s);
    arena_init(&s.tick_arena, 4096);

    s.desktop_count = 1;
    s.current_desktop = 0;

    xcb_map_request_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window = 5678;
    ev.parent = s.root;

    g_map_request_window = ev.window;
    stub_poll_for_reply_hook = map_request_poll_for_reply;

    wm_handle_map_request(&s, &ev);
    cookie_jar_drain(&s.cookie_jar, s.conn, &s, 8);

    handle_t h = server_get_client_by_window(&s, ev.window);
    assert(h != HANDLE_INVALID);

    client_hot_t* hot = server_chot(&s, h);
    assert(hot != NULL);

    hot->desired = (rect_t){0, 0, 120, 90};
    hot->visual_id = s.root_visual;
    hot->depth = s.root_depth;
    hot->pending_replies = 0;
    hot->state = STATE_READY;

    stub_map_window_count = 0;
    stub_unmap_window_count = 0;
    stub_destroy_window_count = 0;
    stub_mapped_windows_len = 0;

    wm_flush_dirty(&s, monotonic_time_ns());

    assert(hot->state == STATE_MAPPED);
    assert(stub_map_window_count == 2);
    assert(stub_mapped_windows_len == 2);
    assert(stub_mapped_windows[0] == hot->xid);
    assert(stub_mapped_windows[1] == hot->frame);
    assert(stub_unmap_window_count == 0);
    assert(stub_destroy_window_count == 0);
    assert(server_get_client_by_window(&s, hot->xid) == h);

    wm_flush_dirty(&s, monotonic_time_ns());
    assert(stub_unmap_window_count == 0);

    printf("test_map_request_maps_and_stays_mapped passed\n");
    stub_poll_for_reply_hook = NULL;
    arena_destroy(&s.tick_arena);
    cleanup_server(&s);
}

static void test_unmap_destroy_unmanages(void) {
    server_t s;
    setup_server(&s);

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));

    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 512);

    hot->self = h;
    hot->xid = 3001;
    hot->frame = 3002;
    hot->state = STATE_MAPPED;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    hot->ignore_unmap = 0;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s.window_to_client, hot->xid, handle_to_ptr(h));
    hash_map_insert(&s.frame_to_client, hot->frame, handle_to_ptr(h));

    wm_set_focus(&s, h);
    assert(s.focused_client == h);

    xcb_unmap_notify_event_t unmap;
    memset(&unmap, 0, sizeof(unmap));
    unmap.window = hot->xid;
    unmap.event = s.root;
    wm_handle_unmap_notify(&s, &unmap);

    assert(server_get_client_by_window(&s, hot->xid) == HANDLE_INVALID);
    assert((s.root_dirty & ROOT_DIRTY_CLIENT_LIST) != 0);

    void *hot_ptr2 = NULL, *cold_ptr2 = NULL;
    handle_t h2 = slotmap_alloc(&s.clients, &hot_ptr2, &cold_ptr2);
    client_hot_t* hot2 = (client_hot_t*)hot_ptr2;
    client_cold_t* cold2 = (client_cold_t*)cold_ptr2;
    memset(hot2, 0, sizeof(*hot2));
    memset(cold2, 0, sizeof(*cold2));
    render_init(&hot2->render_ctx);
    arena_init(&cold2->string_arena, 512);
    hot2->self = h2;
    hot2->xid = 3003;
    hot2->frame = 3004;
    hot2->state = STATE_MAPPED;
    hot2->layer = LAYER_NORMAL;
    hot2->base_layer = LAYER_NORMAL;
    list_init(&hot2->focus_node);
    list_init(&hot2->transients_head);
    list_init(&hot2->transient_sibling);
    hash_map_insert(&s.window_to_client, hot2->xid, handle_to_ptr(h2));
    hash_map_insert(&s.frame_to_client, hot2->frame, handle_to_ptr(h2));

    xcb_destroy_notify_event_t destroy;
    memset(&destroy, 0, sizeof(destroy));
    destroy.window = hot2->xid;
    destroy.event = s.root;
    wm_handle_destroy_notify(&s, &destroy);

    assert(server_get_client_by_window(&s, hot2->xid) == HANDLE_INVALID);
    assert((s.root_dirty & ROOT_DIRTY_CLIENT_LIST) != 0);

    printf("test_unmap_destroy_unmanages passed\n");
    cleanup_server(&s);
}

static void test_destroy_notify_unmanages_and_destroys_frame(void) {
    server_t s;
    setup_server(&s);

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));

    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 512);

    hot->self = h;
    hot->xid = 5001;
    hot->frame = 5002;
    hot->state = STATE_MAPPED;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s.window_to_client, hot->xid, handle_to_ptr(h));
    hash_map_insert(&s.frame_to_client, hot->frame, handle_to_ptr(h));

    wm_set_focus(&s, h);
    assert(s.focused_client == h);

    stub_destroy_window_count = 0;

    xcb_destroy_notify_event_t destroy;
    memset(&destroy, 0, sizeof(destroy));
    destroy.window = hot->xid;
    destroy.event = s.root;
    wm_handle_destroy_notify(&s, &destroy);

    assert(server_get_client_by_window(&s, hot->xid) == HANDLE_INVALID);
    assert(stub_destroy_window_count == 1);
    assert(stub_last_destroyed_window == hot->frame);
    assert(s.focused_client == HANDLE_INVALID);

    printf("test_destroy_notify_unmanages_and_destroys_frame passed\n");
    cleanup_server(&s);
}

static void test_iconify_ignores_unmap_notify_send_event(void) {
    server_t s;
    setup_server(&s);

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));

    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 512);

    hot->self = h;
    hot->xid = 4001;
    hot->frame = 4002;
    hot->state = STATE_MAPPED;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    hot->stacking_layer = -1;
    hot->stacking_index = -1;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s.window_to_client, hot->xid, handle_to_ptr(h));
    hash_map_insert(&s.frame_to_client, hot->frame, handle_to_ptr(h));

    wm_client_iconify(&s, h);
    assert(hot->state == STATE_UNMAPPED);
    assert(hot->ignore_unmap > 0);

    xcb_unmap_notify_event_t unmap;
    memset(&unmap, 0, sizeof(unmap));
    unmap.response_type = XCB_UNMAP_NOTIFY | 0x80;
    unmap.window = hot->xid;
    unmap.event = s.root;
    wm_handle_unmap_notify(&s, &unmap);

    assert(server_get_client_by_window(&s, hot->xid) == h);
    assert(hot->ignore_unmap == 0);

    printf("test_iconify_ignores_unmap_notify_send_event passed\n");
    cleanup_server(&s);
}

static void test_reparent_notify_ignored(void) {
    server_t s;
    setup_server(&s);

    arena_init(&s.tick_arena, 1024);

    xcb_reparent_notify_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_REPARENT_NOTIFY;
    ev.window = 4001;
    ev.parent = s.root;

    xcb_stubs_reset();
    xcb_stubs_enqueue_event((xcb_generic_event_t*)memcpy(calloc(1, sizeof(ev)), &ev, sizeof(ev)));
    event_ingest(&s, true);

    assert(s.buckets.map_requests.length == 0);
    assert(s.buckets.unmap_notifies.length == 0);
    assert(s.buckets.destroy_notifies.length == 0);

    printf("test_reparent_notify_ignored passed\n");
    arena_destroy(&s.tick_arena);
    cleanup_server(&s);
}

static void test_manage_start_already_managed(void) {
    server_t s;
    setup_server(&s);

    xcb_window_t win = 12345;
    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    hot->xid = win;
    hot->state = STATE_MAPPED;  // Assuming managed
    hash_map_insert(&s.window_to_client, win, handle_to_ptr(h));

    // Call manage_start again
    // Should return early and not allocate new slot
    // We check this by ensuring slotmap count is still 1
    client_manage_start(&s, win);

    assert(count_live_clients(&s) == 1);

    printf("test_manage_start_already_managed passed\n");
    cleanup_server(&s);
}

static void test_manage_start_requests_window_type(void) {
    server_t s;
    setup_server(&s);

    xcb_window_t win = 2222;
    client_manage_start(&s, win);

    assert(cookie_jar_has_atom(&s.cookie_jar, atoms._NET_WM_WINDOW_TYPE));

    printf("test_manage_start_requests_window_type passed\n");
    cleanup_server(&s);
}

static void test_manage_start_slot_full(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    xcb_stubs_reset();
    s.conn = xcb_connect(NULL, NULL);
    atoms_init(s.conn);

    // cap=2 => index 0 invalid, index 1 valid. capacity for 1 client.
    slotmap_init(&s.clients, 2, sizeof(client_hot_t), sizeof(client_cold_t));
    hash_map_init(&s.window_to_client);
    hash_map_init(&s.frame_to_client);
    list_init(&s.focus_history);
    cookie_jar_init(&s.cookie_jar);

    // Fill the slot
    void *hot, *cold;
    handle_t h = slotmap_alloc(&s.clients, &hot, &cold);
    assert(h != HANDLE_INVALID);

    // Now try to manage another window
    xcb_window_t win = 999;
    client_manage_start(&s, win);

    // Should have failed to allocate and returned early
    // Verify window not in map
    assert(server_get_client_by_window(&s, win) == HANDLE_INVALID);

    printf("test_manage_start_slot_full passed\n");

    cookie_jar_destroy(&s.cookie_jar);
    slotmap_destroy(&s.clients);
    hash_map_destroy(&s.window_to_client);
    hash_map_destroy(&s.frame_to_client);
    xcb_disconnect(s.conn);
}

static void test_manage_start_defaults_desktop_current(void) {
    server_t s;
    setup_server(&s);

    s.desktop_count = 4;
    s.current_desktop = 2;

    xcb_window_t win = 4242;
    client_manage_start(&s, win);

    handle_t h = server_get_client_by_window(&s, win);
    assert(h != HANDLE_INVALID);

    client_hot_t* hot = server_chot(&s, h);
    assert(hot != NULL);
    assert(hot->desktop == (int32_t)s.current_desktop);
    assert(hot->sticky == false);
    assert(hot->net_wm_desktop_seen == false);

    printf("test_manage_start_defaults_desktop_current passed\n");
    cleanup_server(&s);
}

static void test_should_focus_on_map_override(void) {
    client_hot_t hot = {0};

    hot.focus_override = -1;
    hot.type = WINDOW_TYPE_NORMAL;

    // Default depends on type/transient, for NORMAL it's false
    assert(should_focus_on_map(&hot) == false);

    hot.focus_override = 1;
    assert(should_focus_on_map(&hot) == true);

    hot.focus_override = 0;
    // Even if it's a dialog (which normally returns true)
    hot.type = WINDOW_TYPE_DIALOG;
    assert(should_focus_on_map(&hot) == false);

    printf("test_should_focus_on_map_override passed\n");
}

int main(void) {
    test_adopt_children_skips_override_and_unmapped();
    test_map_request_starts_manage_once();
    test_finish_manage_maps_client_then_frame();
    test_finish_manage_ignores_reparent_unmap();
    test_map_request_maps_and_stays_mapped();
    test_unmap_destroy_unmanages();
    test_destroy_notify_unmanages_and_destroys_frame();
    test_iconify_ignores_unmap_notify_send_event();
    test_reparent_notify_ignored();

    test_manage_start_already_managed();
    test_manage_start_requests_window_type();
    test_manage_start_slot_full();
    test_manage_start_defaults_desktop_current();
    test_should_focus_on_map_override();

    return 0;
}
