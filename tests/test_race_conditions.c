#include <assert.h>
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
extern int (*stub_poll_for_reply_hook)(xcb_connection_t* c, unsigned int request, void** reply,
                                       xcb_generic_error_t** error);
extern bool xcb_stubs_attr_request_window(uint32_t seq, xcb_window_t* out_window);

static void setup_server(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->is_test = true;
    s->conn = xcb_connect(NULL, NULL);
    atoms_init(s->conn);
    s->root = 1;
    s->root_visual = 1;
    s->root_depth = 24;
    s->root_visual_type = xcb_get_visualtype(s->conn, 0);
    list_init(&s->focus_history);
    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s->layers[i]);
    small_vec_init(&s->active_clients);
    cookie_jar_init(&s->cookie_jar);
    slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
}

static void cleanup_server(server_t* s) {
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (s->clients.hdr[i].live) {
            handle_t h = handle_make(i, s->clients.hdr[i].gen);
            client_hot_t* hot = server_chot(s, h);
            client_cold_t* cold = server_ccold(s, h);
            if (cold) arena_destroy(&cold->string_arena);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    small_vec_destroy(&s->active_clients);
    cookie_jar_destroy(&s->cookie_jar);
    slotmap_destroy(&s->clients);
    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);
    xcb_disconnect(s->conn);
}

typedef struct attr_reply_entry {
    xcb_window_t window;
    bool override_redirect;
    uint8_t map_state;
} attr_reply_entry_t;

static attr_reply_entry_t g_attr_entries[4];
static int g_attr_entries_len = 0;

static int poll_attrs_then_die(xcb_connection_t* c, unsigned int request, void** reply, xcb_generic_error_t** error) {
    (void)c;
    if (error) *error = NULL;

    xcb_window_t win = XCB_NONE;
    if (!xcb_stubs_attr_request_window(request, &win)) return 0;

    for (int i = 0; i < g_attr_entries_len; i++) {
        if (g_attr_entries[i].window == win) {
            xcb_get_window_attributes_reply_t* r = calloc(1, sizeof(*r));
            r->override_redirect = g_attr_entries[i].override_redirect ? 1 : 0;
            r->map_state = g_attr_entries[i].map_state;
            if (reply) *reply = r;
            return 1;
        }
    }

    return 0;
}

static void test_destroy_during_manage_no_crash(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    stub_poll_for_reply_hook = poll_attrs_then_die;

    xcb_window_t win = 9001;
    client_manage_start(&s, win);

    handle_t h = server_get_client_by_window(&s, win);
    assert(h != HANDLE_INVALID);

    g_attr_entries_len = 0;
    g_attr_entries[g_attr_entries_len++] = (attr_reply_entry_t){win, false, XCB_MAP_STATE_VIEWABLE};

    cookie_jar_drain(&s.cookie_jar, s.conn, &s, 8);

    client_hot_t* hot = server_chot(&s, h);
    assert(hot != NULL);

    hot->state = STATE_DESTROYED;
    client_unmanage(&s, h);

    cookie_jar_drain(&s.cookie_jar, s.conn, &s, 8);

    assert(server_get_client_by_window(&s, win) == HANDLE_INVALID);

    printf("test_destroy_during_manage_no_crash passed\n");
    cleanup_server(&s);
}

static void test_state_toggle_stability(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms._NET_WM_STATE_ABOVE = 10;
    atoms._NET_WM_STATE_BELOW = 11;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 128);
    hot->self = h;
    hot->xid = 9101;
    hot->state = STATE_MAPPED;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    for (int i = 0; i < 100; i++) {
        wm_client_update_state(&s, h, 2, atoms._NET_WM_STATE_ABOVE);
        wm_client_update_state(&s, h, 2, atoms._NET_WM_STATE_BELOW);
    }

    assert(hot->state_above == false);
    assert(hot->state_below == true);
    assert(hot->layer == LAYER_BELOW);

    printf("test_state_toggle_stability passed\n");
    cleanup_server(&s);
}

static void test_out_of_order_reply_ignored(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 128);
    hot->self = h;
    hot->xid = 9201;
    hot->state = STATE_NEW;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    // Mark destroyed then unmanage to simulate stale replies arriving later.
    hot->state = STATE_DESTROYED;
    client_unmanage(&s, h);

    cookie_slot_t slot = {0};
    slot.client = h;
    slot.type = COOKIE_GET_GEOMETRY;
    slot.data = hot->xid;

    xcb_get_geometry_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.width = 100;
    reply.height = 100;

    wm_handle_reply(&s, &slot, &reply, NULL);

    printf("test_out_of_order_reply_ignored passed\n");
    cleanup_server(&s);
}

int main(void) {
    test_destroy_during_manage_no_crash();
    test_state_toggle_stability();
    test_out_of_order_reply_ignored();
    return 0;
}
