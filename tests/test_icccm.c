#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
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

volatile sig_atomic_t g_reload_pending = 0;

void test_icccm_protocols() {
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

    wm_handle_reply(&s, &slot, &mock_r);

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

void test_client_close() {
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

void test_name_fallback() {
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

    wm_handle_reply(&s, &slot, &mock_wm_name);
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
    wm_handle_reply(&s, &slot, &mock_net_name);
    assert(strcmp(cold->title, "modern") == 0);

    // 3. Another WM_NAME reply (should NOT overwrite modern)
    slot.data = ((uint64_t)123 << 32) | atoms.WM_NAME;
    wm_handle_reply(&s, &slot, &mock_wm_name);
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

int main() {
    test_icccm_protocols();
    test_client_close();
    test_name_fallback();
    return 0;
}
