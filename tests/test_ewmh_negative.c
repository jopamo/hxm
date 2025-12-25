#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xproto.h>

#include "client.h"
#include "config.h"
#include "cookie_jar.h"
#include "event.h"
#include "hxm.h"
#include "src/wm_internal.h"
#include "wm.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern int stub_prop_calls_len;
extern struct stub_prop_call {
    xcb_window_t window;
    xcb_atom_t atom;
    xcb_atom_t type;
    uint8_t format;
    uint32_t len;
    uint8_t data[4096];
    bool deleted;
} stub_prop_calls[128];

static const struct stub_prop_call* find_prop_call(xcb_window_t win, xcb_atom_t atom, bool deleted) {
    for (int i = stub_prop_calls_len - 1; i >= 0; i--) {
        if (stub_prop_calls[i].window == win && stub_prop_calls[i].atom == atom &&
            stub_prop_calls[i].deleted == deleted) {
            return &stub_prop_calls[i];
        }
    }
    return NULL;
}

static xcb_get_property_reply_t* make_string_reply(xcb_atom_t type, const char* value, int len) {
    size_t total = sizeof(xcb_get_property_reply_t) + (size_t)len;
    xcb_get_property_reply_t* rep = calloc(1, total);
    if (!rep) return NULL;
    rep->format = 8;
    rep->type = type;
    rep->value_len = (uint32_t)len;
    rep->length = (uint32_t)((len + 3) / 4);
    if (len > 0 && value) {
        memcpy(xcb_get_property_value(rep), value, (size_t)len);
    }
    return rep;
}

static void setup_server(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->is_test = true;
    s->conn = xcb_connect(NULL, NULL);
    atoms_init(s->conn);

    s->root = 1;
    s->root_visual = 1;
    s->root_depth = 24;
    s->root_visual_type = xcb_get_visualtype(s->conn, 0);

    config_init_defaults(&s->config);
    s->desktop_count = 1;
    s->current_desktop = 0;
    s->workarea = (rect_t){0, 0, 0, 0};

    arena_init(&s->tick_arena, 4096);
    cookie_jar_init(&s->cookie_jar);
    slotmap_init(&s->clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    list_init(&s->focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s->layers[i]);
}

static void cleanup_server(server_t* s) {
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (!s->clients.hdr[i].live) continue;
        handle_t h = handle_make(i, s->clients.hdr[i].gen);
        client_hot_t* hot = server_chot(s, h);
        client_cold_t* cold = server_ccold(s, h);
        if (cold) arena_destroy(&cold->string_arena);
        if (hot) {
            render_free(&hot->render_ctx);
            if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
        }
    }
    cookie_jar_destroy(&s->cookie_jar);
    slotmap_destroy(&s->clients);
    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_destroy(&s->layers[i]);
    arena_destroy(&s->tick_arena);
    config_destroy(&s->config);
    xcb_disconnect(s->conn);
}

static handle_t add_mapped_client(server_t* s, xcb_window_t win, xcb_window_t frame) {
    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));

    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 128);

    hot->self = h;
    hot->xid = win;
    hot->frame = frame;
    hot->state = STATE_MAPPED;
    hot->type = WINDOW_TYPE_NORMAL;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    hot->manage_phase = MANAGE_DONE;
    hot->server = (rect_t){10, 10, 200, 150};
    hot->desired = hot->server;

    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s->window_to_client, win, handle_to_ptr(h));
    hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));
    return h;
}

static void test_malformed_wm_state_format_ignored(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms._NET_WM_STATE = 100;
    atoms._NET_WM_STATE_FULLSCREEN = 101;

    handle_t h = add_mapped_client(&s, 1001, 1101);
    client_hot_t* hot = server_chot(&s, h);

    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.type = atoms._NET_WM_STATE;
    ev.window = hot->xid;
    ev.format = 8;
    ev.data.data32[0] = 1;
    ev.data.data32[1] = atoms._NET_WM_STATE_FULLSCREEN;

    wm_handle_client_message(&s, &ev);

    assert(hot->layer == LAYER_NORMAL);
    assert(!(hot->flags & CLIENT_FLAG_UNDECORATED));

    printf("test_malformed_wm_state_format_ignored passed\n");
    cleanup_server(&s);
}

static void test_unknown_window_type_ignored(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms._NET_WM_WINDOW_TYPE = 200;

    handle_t h = add_mapped_client(&s, 2001, 2101);
    client_hot_t* hot = server_chot(&s, h);

    cookie_slot_t slot = {0};
    slot.client = h;
    slot.type = COOKIE_GET_PROPERTY;
    slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_WINDOW_TYPE;

    struct {
        xcb_get_property_reply_t r;
        xcb_atom_t types[1];
    } reply;
    memset(&reply, 0, sizeof(reply));
    reply.r.format = 32;
    reply.r.value_len = 1;
    reply.r.type = XCB_ATOM_ATOM;
    reply.types[0] = 9999;

    wm_handle_reply(&s, &slot, &reply.r, NULL);

    assert(hot->type == WINDOW_TYPE_NORMAL);
    assert(!hot->type_from_net);

    printf("test_unknown_window_type_ignored passed\n");
    cleanup_server(&s);
}

static void test_strut_partial_invalid_ranges_ignored(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms._NET_WORKAREA = 300;
    atoms._NET_WM_STRUT_PARTIAL = 301;
    s.desktop_count = 1;

    handle_t h = add_mapped_client(&s, 3001, 3101);

    cookie_slot_t slot = {0};
    slot.client = h;
    slot.type = COOKIE_GET_PROPERTY;
    slot.data = ((uint64_t)3001 << 32) | atoms._NET_WM_STRUT_PARTIAL;

    struct {
        xcb_get_property_reply_t r;
        uint32_t data[12];
    } reply;
    memset(&reply, 0, sizeof(reply));
    reply.r.format = 32;
    reply.r.value_len = 12;
    reply.r.type = XCB_ATOM_CARDINAL;
    reply.data[0] = 100;
    reply.data[4] = 50;
    reply.data[5] = 40;

    wm_handle_reply(&s, &slot, &reply.r, NULL);
    wm_flush_dirty(&s, monotonic_time_ns());

    const struct stub_prop_call* wa = find_prop_call(s.root, atoms._NET_WORKAREA, false);
    assert(wa != NULL);
    const uint32_t* vals = (const uint32_t*)wa->data;
    assert(vals[0] == 0);
    assert(vals[1] == 0);
    assert(vals[2] == 1920);
    assert(vals[3] == 1080);

    printf("test_strut_partial_invalid_ranges_ignored passed\n");
    cleanup_server(&s);
}

static void test_property_spam_no_crash(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms._NET_WM_NAME = 400;
    atoms.UTF8_STRING = 401;

    handle_t h = add_mapped_client(&s, 4001, 4101);
    client_hot_t* hot = server_chot(&s, h);

    cookie_slot_t slot = {0};
    slot.client = h;
    slot.type = COOKIE_GET_PROPERTY;
    slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;

    for (int i = 0; i < 256; i++) {
        xcb_get_property_reply_t* rep = make_string_reply(atoms.UTF8_STRING, "spam", 4);
        wm_handle_reply(&s, &slot, rep, NULL);
        free(rep);
    }

    client_cold_t* cold = server_ccold(&s, h);
    assert(cold->has_net_wm_name);
    assert(cold->base_title != NULL);

    printf("test_property_spam_no_crash passed\n");
    cleanup_server(&s);
}

int main(void) {
    test_malformed_wm_state_format_ignored();
    test_unknown_window_type_ignored();
    test_strut_partial_invalid_ranges_ignored();
    test_property_spam_no_crash();
    return 0;
}
