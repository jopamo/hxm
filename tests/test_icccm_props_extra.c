#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

extern int stub_set_input_focus_count;
extern xcb_window_t stub_last_input_focus_window;

static xcb_get_property_reply_t* make_string_reply(xcb_atom_t type, const char* value, int len) {
    size_t total = sizeof(xcb_get_property_reply_t) + (size_t)len;
    xcb_get_property_reply_t* rep = calloc(1, total);
    if (!rep) return NULL;
    rep->format = 8;
    rep->type = type;
    rep->value_len = (uint32_t)len;
    if (len > 0 && value) {
        memcpy(xcb_get_property_value(rep), value, (size_t)len);
    }
    return rep;
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
    slotmap_destroy(&s->clients);
    free(s->conn);
}

static void test_wm_icon_name_fallback(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);

    atoms._NET_WM_ICON_NAME = 10;
    atoms.WM_ICON_NAME = 11;
    atoms.UTF8_STRING = 12;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 123;
    hot->state = STATE_MAPPED;
    hot->pending_replies = 1;
    arena_init(&cold->string_arena, 512);

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;

    xcb_get_property_reply_t* rep = make_string_reply(atoms.UTF8_STRING, "icon-net", 8);
    slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_ICON_NAME;
    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);

    assert(cold->has_net_wm_icon_name);
    assert(cold->base_icon_name != NULL);
    assert(strcmp(cold->base_icon_name, "icon-net") == 0);

    rep = make_string_reply(XCB_ATOM_STRING, "legacy", 6);
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_ICON_NAME;
    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);
    assert(strcmp(cold->base_icon_name, "icon-net") == 0);

    rep = make_string_reply(atoms.UTF8_STRING, "", 0);
    slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_ICON_NAME;
    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);
    assert(!cold->has_net_wm_icon_name);

    rep = make_string_reply(XCB_ATOM_STRING, "legacy", 6);
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_ICON_NAME;
    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);
    assert(strcmp(cold->base_icon_name, "legacy") == 0);

    printf("test_wm_icon_name_fallback passed\n");
    cleanup_server(&s);
}

static void test_wm_class_invalid_no_nul(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    atoms.WM_CLASS = 3;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 456;
    hot->state = STATE_NEW;
    hot->pending_replies = 1;
    arena_init(&cold->string_arena, 512);

    struct {
        xcb_get_property_reply_t reply;
        char data[8];
    } mock_r;
    memset(&mock_r, 0, sizeof(mock_r));
    mock_r.reply.format = 8;
    mock_r.reply.type = XCB_ATOM_STRING;
    mock_r.reply.value_len = 7;
    memcpy(mock_r.data, "badclas", 7);

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_CLASS;
    wm_handle_reply(&s, &slot, &mock_r, NULL);

    assert(cold->wm_instance == NULL);
    assert(cold->wm_class == NULL);

    printf("test_wm_class_invalid_no_nul passed\n");
    cleanup_server(&s);
}

static void test_wm_client_machine_large(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    atoms.WM_CLIENT_MACHINE = 4;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 789;
    hot->state = STATE_NEW;
    hot->pending_replies = 1;
    arena_init(&cold->string_arena, 512);

    const int len = 2048;
    char* buf = malloc((size_t)len);
    memset(buf, 'a', (size_t)len);

    xcb_get_property_reply_t* rep = make_string_reply(XCB_ATOM_STRING, buf, len);
    free(buf);

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_CLIENT_MACHINE;
    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);

    assert(cold->wm_client_machine != NULL);
    assert(strlen(cold->wm_client_machine) == (size_t)len);
    assert(cold->wm_client_machine[0] == 'a');
    assert(cold->wm_client_machine[len - 1] == 'a');

    printf("test_wm_client_machine_large passed\n");
    cleanup_server(&s);
}

static void test_wm_command_first_token(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    atoms.WM_COMMAND = 5;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 321;
    hot->state = STATE_NEW;
    hot->pending_replies = 1;
    arena_init(&cold->string_arena, 512);

    const char payload[] = "cmd\0--flag\0";
    xcb_get_property_reply_t* rep = make_string_reply(XCB_ATOM_STRING, payload, (int)sizeof(payload));

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_COMMAND;
    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);

    assert(cold->wm_command != NULL);
    assert(strcmp(cold->wm_command, "cmd") == 0);

    printf("test_wm_command_first_token passed\n");
    cleanup_server(&s);
}

static void test_wm_hints_input_affects_focus(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    atoms.WM_HINTS = 6;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;
    list_init(&s.focus_history);

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 512);
    hot->self = h;
    hot->xid = 654;
    hot->state = STATE_MAPPED;
    list_init(&hot->focus_node);

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_HINTS;

    struct {
        xcb_get_property_reply_t r;
        uint32_t data[9];
    } reply;
    memset(&reply, 0, sizeof(reply));
    reply.r.format = 32;
    reply.r.value_len = 9;
    reply.r.type = XCB_ATOM_WM_HINTS;

    // input = false
    reply.data[0] = XCB_ICCCM_WM_HINT_INPUT;
    reply.data[1] = 0;
    wm_handle_reply(&s, &slot, &reply.r, NULL);
    assert(cold->can_focus == false);

    stub_set_input_focus_count = 0;
    stub_last_input_focus_window = 0;
    wm_set_focus(&s, h);
    assert(stub_set_input_focus_count == 0);

    // input = true
    reply.data[0] = XCB_ICCCM_WM_HINT_INPUT;
    reply.data[1] = 1;
    wm_handle_reply(&s, &slot, &reply.r, NULL);
    assert(cold->can_focus == true);

    stub_set_input_focus_count = 0;
    stub_last_input_focus_window = 0;
    wm_set_focus(&s, h);
    assert(stub_set_input_focus_count == 1);
    assert(stub_last_input_focus_window == hot->xid);

    printf("test_wm_hints_input_affects_focus passed\n");
    cleanup_server(&s);
}

static void test_wm_hints_icon_safe(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    atoms.WM_HINTS = 7;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));
    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 512);
    hot->xid = 777;
    hot->state = STATE_NEW;

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_HINTS;

    struct {
        xcb_get_property_reply_t r;
        uint32_t data[9];
    } reply;
    memset(&reply, 0, sizeof(reply));
    reply.r.format = 32;
    reply.r.value_len = 9;
    reply.r.type = XCB_ATOM_WM_HINTS;
    reply.data[0] = XCB_ICCCM_WM_HINT_ICON_PIXMAP | XCB_ICCCM_WM_HINT_ICON_MASK;
    reply.data[3] = 0xdeadbeef;
    reply.data[7] = 0xbaadf00d;

    wm_handle_reply(&s, &slot, &reply.r, NULL);
    assert((hot->flags & CLIENT_FLAG_URGENT) == 0);
    assert(cold->can_focus == true);

    printf("test_wm_hints_icon_safe passed\n");
    cleanup_server(&s);
}

int main(void) {
    test_wm_icon_name_fallback();
    test_wm_class_invalid_no_nul();
    test_wm_client_machine_large();
    test_wm_command_first_token();
    test_wm_hints_input_affects_focus();
    test_wm_hints_icon_safe();
    return 0;
}
