#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xproto.h>

#include "client.h"
#include "config.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

// External stubs counters
extern int stub_send_event_count;
extern xcb_window_t stub_last_send_event_destination;
extern char stub_last_event[32];
extern int stub_kill_client_count;
extern uint32_t stub_last_kill_client_resource;
extern xcb_atom_t stub_last_prop_atom;
extern uint32_t stub_last_prop_len;
extern uint8_t stub_last_prop_data[4096];
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

void test_icccm_protocols(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);

    atoms.WM_PROTOCOLS = 10;
    atoms.WM_DELETE_WINDOW = 11;
    atoms.WM_TAKE_FOCUS = 12;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 123;
    hot->state = STATE_NEW;
    hot->pending_replies = 10;
    arena_init(&cold->string_arena, 512);

    // Mock reply for WM_PROTOCOLS with both DELETE and TAKE_FOCUS
    struct {
        xcb_get_property_reply_t reply;
        xcb_atom_t atoms[2];
    } mock_r;
    memset(&mock_r, 0, sizeof(mock_r));
    mock_r.reply.format = 32;
    mock_r.reply.type = XCB_ATOM_ATOM;
    mock_r.reply.value_len = 2;
    mock_r.atoms[0] = atoms.WM_DELETE_WINDOW;
    mock_r.atoms[1] = atoms.WM_TAKE_FOCUS;

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)123 << 32) | atoms.WM_PROTOCOLS;

    wm_handle_reply(&s, &slot, &mock_r, NULL);

    assert(cold->protocols & PROTOCOL_DELETE_WINDOW);
    assert(cold->protocols & PROTOCOL_TAKE_FOCUS);
    printf("test_icccm_protocols passed\n");

    arena_destroy(&cold->string_arena);
    for (uint32_t i = 1; i < s.clients.cap; i++) {
        if (s.clients.hdr[i].live) {
            handle_t h = handle_make(i, s.clients.hdr[i].gen);
            client_hot_t* hot = server_chot(&s, h);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    slotmap_destroy(&s.clients);
    free(s.conn);
}

void test_client_close(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    atoms.WM_PROTOCOLS = 10;
    atoms.WM_DELETE_WINDOW = 11;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 123;
    hot->state = STATE_MAPPED;
    arena_init(&cold->string_arena, 512);

    // 1. Test close with WM_DELETE_WINDOW
    cold->protocols |= PROTOCOL_DELETE_WINDOW;
    stub_send_event_count = 0;
    client_close(&s, h);
    assert(stub_send_event_count == 1);
    assert(stub_last_send_event_destination == 123);
    xcb_client_message_event_t* ev = (xcb_client_message_event_t*)stub_last_event;
    assert(ev->type == atoms.WM_PROTOCOLS);
    assert(ev->data.data32[0] == atoms.WM_DELETE_WINDOW);

    // 2. Test close without WM_DELETE_WINDOW (kill)
    cold->protocols &= ~PROTOCOL_DELETE_WINDOW;
    stub_kill_client_count = 0;
    client_close(&s, h);
    assert(stub_kill_client_count == 1);
    assert(stub_last_kill_client_resource == 123);

    // 3. Test close with race (DESTROYED state)
    hot->state = STATE_DESTROYED;
    stub_send_event_count = 0;
    stub_kill_client_count = 0;
    client_close(&s, h);
    assert(stub_send_event_count == 0);
    assert(stub_kill_client_count == 0);

    printf("test_client_close passed\n");
    arena_destroy(&cold->string_arena);
    for (uint32_t i = 1; i < s.clients.cap; i++) {
        if (s.clients.hdr[i].live) {
            handle_t h = handle_make(i, s.clients.hdr[i].gen);
            client_hot_t* hot = server_chot(&s, h);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    slotmap_destroy(&s.clients);
    free(s.conn);
}

void test_wm_take_focus_on_focus(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root = 1;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);

    atoms.WM_PROTOCOLS = 20;
    atoms.WM_TAKE_FOCUS = 21;

    list_init(&s.focus_history);
    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 128);
    hot->self = h;
    hot->xid = 123;
    hot->state = STATE_MAPPED;
    list_init(&hot->focus_node);
    cold->protocols |= PROTOCOL_TAKE_FOCUS;
    cold->can_focus = true;

    stub_send_event_count = 0;
    wm_set_focus(&s, h);
    wm_flush_dirty(&s);
    assert(stub_send_event_count == 1);
    assert(stub_last_send_event_destination == hot->xid);
    xcb_client_message_event_t* ev = (xcb_client_message_event_t*)stub_last_event;
    assert(ev->type == atoms.WM_PROTOCOLS);
    assert(ev->data.data32[0] == atoms.WM_TAKE_FOCUS);
    assert(ev->data.data32[1] == XCB_CURRENT_TIME);

    hot->user_time = 424242;
    wm_set_focus(&s, HANDLE_INVALID);
    wm_flush_dirty(&s);
    stub_send_event_count = 0;
    wm_set_focus(&s, h);
    wm_flush_dirty(&s);
    assert(stub_send_event_count == 1);
    ev = (xcb_client_message_event_t*)stub_last_event;
    assert(ev->data.data32[0] == atoms.WM_TAKE_FOCUS);
    assert(ev->data.data32[1] == hot->user_time);

    printf("test_wm_take_focus_on_focus passed\n");
    arena_destroy(&cold->string_arena);
    for (uint32_t i = 1; i < s.clients.cap; i++) {
        if (s.clients.hdr[i].live) {
            handle_t h = handle_make(i, s.clients.hdr[i].gen);
            client_hot_t* hot = server_chot(&s, h);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    arena_destroy(&s.tick_arena);
    slotmap_destroy(&s.clients);
    free(s.conn);
}

void test_wm_state_manage_unmanage(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root = 1;
    s.root_visual = 1;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    config_init_defaults(&s.config);
    list_init(&s.focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s.layers[i]);
    hash_map_init(&s.window_to_client);
    hash_map_init(&s.frame_to_client);

    atoms.WM_STATE = 30;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 128);

    hot->self = h;
    hot->xid = 555;
    hot->state = STATE_NEW;
    hot->type = WINDOW_TYPE_NORMAL;
    hot->focus_override = -1;
    hot->transient_for = HANDLE_INVALID;
    hot->desktop = 0;
    hot->initial_state = XCB_ICCCM_WM_STATE_NORMAL;
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

    const struct stub_prop_call* set = find_prop_call(hot->xid, atoms.WM_STATE, false);
    assert(set != NULL);
    const uint32_t* vals = (const uint32_t*)set->data;
    assert(vals[0] == XCB_ICCCM_WM_STATE_NORMAL);

    client_unmanage(&s, h);

    const struct stub_prop_call* del = find_prop_call(hot->xid, atoms.WM_STATE, true);
    assert(del != NULL);

    printf("test_wm_state_manage_unmanage passed\n");
    config_destroy(&s.config);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_destroy(&s.layers[i]);
    hash_map_destroy(&s.window_to_client);
    hash_map_destroy(&s.frame_to_client);
    slotmap_destroy(&s.clients);
    free(s.conn);
}

void test_name_fallback(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    atoms.WM_NAME = 1;
    atoms._NET_WM_NAME = 2;
    atoms.UTF8_STRING = 3;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 123;
    hot->state = STATE_NEW;
    hot->pending_replies = 10;
    arena_init(&cold->string_arena, 512);

    // 1. WM_NAME reply
    struct {
        xcb_get_property_reply_t reply;
        char name[8];
    } mock_wm_name;
    memset(&mock_wm_name, 0, sizeof(mock_wm_name));
    mock_wm_name.reply.format = 8;
    mock_wm_name.reply.value_len = 6;
    memcpy(mock_wm_name.name, "legacy", 6);

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)123 << 32) | atoms.WM_NAME;

    wm_handle_reply(&s, &slot, &mock_wm_name, NULL);
    assert(cold->title != NULL);
    assert(strcmp(cold->title, "legacy") == 0);

    // 2. _NET_WM_NAME reply (should overwrite legacy)
    struct {
        xcb_get_property_reply_t reply;
        char name[8];
    } mock_net_name;
    memset(&mock_net_name, 0, sizeof(mock_net_name));
    mock_net_name.reply.format = 8;
    mock_net_name.reply.value_len = 6;
    memcpy(mock_net_name.name, "modern", 6);

    slot.data = ((uint64_t)123 << 32) | atoms._NET_WM_NAME;
    wm_handle_reply(&s, &slot, &mock_net_name, NULL);
    assert(strcmp(cold->title, "modern") == 0);

    // 3. Another WM_NAME reply (should NOT overwrite modern)
    slot.data = ((uint64_t)123 << 32) | atoms.WM_NAME;
    wm_handle_reply(&s, &slot, &mock_wm_name, NULL);
    assert(strcmp(cold->title, "modern") == 0);

    printf("test_name_fallback passed\n");
    arena_destroy(&cold->string_arena);
    for (uint32_t i = 1; i < s.clients.cap; i++) {
        if (s.clients.hdr[i].live) {
            handle_t h = handle_make(i, s.clients.hdr[i].gen);
            client_hot_t* hot = server_chot(&s, h);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    slotmap_destroy(&s.clients);
    free(s.conn);
}

int main(void) {
    test_icccm_protocols();
    test_client_close();
    test_wm_take_focus_on_focus();
    test_wm_state_manage_unmanage();
    test_name_fallback();
    return 0;
}
