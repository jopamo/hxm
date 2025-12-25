#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xproto.h>

#include "client.h"
#include "config.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern int stub_send_event_count;
extern char stub_last_event[32];
extern xcb_atom_t stub_last_prop_atom;
extern uint32_t stub_last_prop_len;
extern uint8_t stub_last_prop_data[4096];
extern int stub_config_calls_len;

typedef struct stub_config_call {
    xcb_window_t win;
    uint16_t mask;

    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;

    uint32_t border_width;
    xcb_window_t sibling;
    uint32_t stack_mode;
} stub_config_call_t;

extern const stub_config_call_t* stub_config_call_at(int idx);

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
    s->config.theme.border_width = 2;
    s->config.theme.title_height = 10;

    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    list_init(&s->focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s->layers[i]);

    slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
    small_vec_init(&s->active_clients);
    arena_init(&s->tick_arena, 4096);
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
    slotmap_destroy(&s->clients);
    small_vec_destroy(&s->active_clients);
    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_destroy(&s->layers[i]);
    arena_destroy(&s->tick_arena);
    config_destroy(&s->config);
    xcb_disconnect(s->conn);
}

static handle_t add_client(server_t* s, xcb_window_t win, xcb_window_t frame) {
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
    hot->desired = (rect_t){10, 20, 100, 80};
    hot->server = hot->desired;
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s->window_to_client, win, handle_to_ptr(h));
    hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));
    small_vec_push(&s->active_clients, handle_to_ptr(h));

    return h;
}

static void test_configure_request_applies_and_extents(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    atoms._NET_FRAME_EXTENTS = 700;

    handle_t h = add_client(&s, 1001, 1101);
    client_hot_t* hot = server_chot(&s, h);

    pending_config_t pc = {0};
    pc.window = hot->xid;
    pc.mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    pc.x = 30;
    pc.y = 40;
    pc.width = 140;
    pc.height = 100;

    wm_handle_configure_request(&s, h, &pc);
    assert(hot->desired.x == 30);
    assert(hot->desired.y == 40);
    assert(hot->desired.w == 140);
    assert(hot->desired.h == 100);
    assert(hot->dirty & DIRTY_GEOM);

    stub_config_calls_len = 0;
    stub_last_prop_atom = 0;
    wm_flush_dirty(&s, monotonic_time_ns());

    assert(stub_config_calls_len >= 2);
    const stub_config_call_t* frame_call = stub_config_call_at(0);
    const stub_config_call_t* client_call = stub_config_call_at(1);

    assert(frame_call->win == hot->frame);
    assert(frame_call->x == 30);
    assert(frame_call->y == 40);
    assert(frame_call->w == 140 + 2 * s.config.theme.border_width);
    assert(frame_call->h == 100 + s.config.theme.title_height + s.config.theme.border_width);

    assert(client_call->win == hot->xid);
    assert(client_call->x == (int32_t)s.config.theme.border_width);
    assert(client_call->y == (int32_t)s.config.theme.title_height);
    assert(client_call->w == 140);
    assert(client_call->h == 100);

    assert(stub_last_prop_atom == atoms._NET_FRAME_EXTENTS);
    assert(stub_last_prop_len == 4);
    uint32_t* extents = (uint32_t*)stub_last_prop_data;
    assert(extents[0] == s.config.theme.border_width);
    assert(extents[1] == s.config.theme.border_width);
    assert(extents[2] == s.config.theme.title_height + s.config.theme.border_width);
    assert(extents[3] == s.config.theme.border_width);

    printf("test_configure_request_applies_and_extents passed\n");
    cleanup_server(&s);
}

static void test_configure_request_mask_respects_existing(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    handle_t h = add_client(&s, 2001, 2101);
    client_hot_t* hot = server_chot(&s, h);
    hot->desired = (rect_t){5, 6, 80, 70};

    pending_config_t pc = {0};
    pc.window = hot->xid;
    pc.mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    pc.width = 120;
    pc.height = 90;

    wm_handle_configure_request(&s, h, &pc);
    assert(hot->desired.x == 5);
    assert(hot->desired.y == 6);
    assert(hot->desired.w == 120);
    assert(hot->desired.h == 90);

    printf("test_configure_request_mask_respects_existing passed\n");
    cleanup_server(&s);
}

static void test_synthetic_configure_notify_sent(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    handle_t h = add_client(&s, 3001, 3101);
    client_hot_t* hot = server_chot(&s, h);

    stub_send_event_count = 0;
    hot->dirty |= DIRTY_GEOM;
    wm_flush_dirty(&s, monotonic_time_ns());

    assert(stub_send_event_count == 1);
    xcb_configure_notify_event_t* ev = (xcb_configure_notify_event_t*)stub_last_event;
    assert((ev->response_type & ~0x80) == XCB_CONFIGURE_NOTIFY);
    assert(ev->window == hot->xid);

    printf("test_synthetic_configure_notify_sent passed\n");
    cleanup_server(&s);
}

static void test_configure_request_ignores_border_and_stack_fields(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    handle_t h = add_client(&s, 4001, 4101);
    client_hot_t* hot = server_chot(&s, h);

    pending_config_t pc = {0};
    pc.window = hot->xid;
    pc.mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
              XCB_CONFIG_WINDOW_BORDER_WIDTH | XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
    pc.x = 12;
    pc.y = 24;
    pc.width = 180;
    pc.height = 90;
    pc.border_width = 7;
    pc.sibling = 9999;
    pc.stack_mode = 3;

    wm_handle_configure_request(&s, h, &pc);
    assert(hot->desired.x == 12);
    assert(hot->desired.y == 24);
    assert(hot->desired.w == 180);
    assert(hot->desired.h == 90);
    assert(hot->dirty & DIRTY_GEOM);

    stub_send_event_count = 0;
    wm_flush_dirty(&s, monotonic_time_ns());
    assert(stub_send_event_count == 1);

    printf("test_configure_request_ignores_border_and_stack_fields passed\n");
    cleanup_server(&s);
}

int main(void) {
    test_configure_request_applies_and_extents();
    test_configure_request_mask_respects_existing();
    test_synthetic_configure_notify_sent();
    test_configure_request_ignores_border_and_stack_fields();
    return 0;
}
