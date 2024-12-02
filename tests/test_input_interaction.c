#include <X11/keysym.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xproto.h>

#include "bbox.h"
#include "client.h"
#include "config.h"
#include "event.h"
#include "wm.h"
#include "wm_internal.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern int stub_grab_pointer_count;
extern int stub_ungrab_pointer_count;
extern int stub_grab_key_count;
extern int stub_ungrab_key_count;
extern uint16_t stub_last_grab_key_mods;
extern xcb_keycode_t stub_last_grab_keycode;

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

    slotmap_init(&s->clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    list_init(&s->focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s->layers[i]);

    s->desktop_count = 2;
    s->current_desktop = 0;
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
    config_destroy(&s->config);
    slotmap_destroy(&s->clients);
    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);
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
    hot->focus_override = -1;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    hot->server = (rect_t){10, 10, 200, 150};
    hot->desired = hot->server;

    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s->window_to_client, win, handle_to_ptr(h));
    hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));

    return h;
}

static void test_click_to_focus(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    handle_t h1 = add_mapped_client(&s, 1001, 1101);
    handle_t h2 = add_mapped_client(&s, 1002, 1102);

    wm_set_focus(&s, h1);
    assert(s.focused_client == h1);

    xcb_button_press_event_t ev = {0};
    ev.event = 1002;
    ev.detail = 1;
    ev.state = 0;

    wm_handle_button_press(&s, &ev);

    assert(s.focused_client == h2);
    assert(s.interaction_mode == INTERACTION_NONE);

    printf("test_click_to_focus passed\n");
    cleanup_server(&s);
}

static void test_move_interaction(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    handle_t h = add_mapped_client(&s, 2001, 2101);
    client_hot_t* hot = server_chot(&s, h);

    xcb_button_press_event_t press = {0};
    press.event = hot->xid;
    press.detail = 1;
    press.state = XCB_MOD_MASK_1;
    press.root_x = 50;
    press.root_y = 60;

    wm_handle_button_press(&s, &press);
    assert(s.interaction_mode == INTERACTION_MOVE);
    assert(stub_grab_pointer_count == 1);

    xcb_motion_notify_event_t motion = {0};
    motion.root_x = 70;
    motion.root_y = 90;
    motion.event = hot->frame;

    wm_handle_motion_notify(&s, &motion);
    assert(hot->desired.x == hot->server.x + 20);
    assert(hot->desired.y == hot->server.y + 30);
    assert(hot->dirty & DIRTY_GEOM);

    xcb_button_release_event_t release = {0};
    wm_handle_button_release(&s, &release);
    assert(s.interaction_mode == INTERACTION_NONE);
    assert(stub_ungrab_pointer_count == 1);

    printf("test_move_interaction passed\n");
    cleanup_server(&s);
}

static void test_resize_interaction(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    handle_t h = add_mapped_client(&s, 3001, 3101);
    client_hot_t* hot = server_chot(&s, h);

    xcb_button_press_event_t press = {0};
    press.event = hot->xid;
    press.detail = 3;
    press.state = XCB_MOD_MASK_1;
    press.root_x = 100;
    press.root_y = 100;

    wm_handle_button_press(&s, &press);
    assert(s.interaction_mode == INTERACTION_RESIZE);
    assert(stub_grab_pointer_count == 1);

    xcb_motion_notify_event_t motion = {0};
    motion.root_x = 140;
    motion.root_y = 120;
    motion.event = hot->frame;

    wm_handle_motion_notify(&s, &motion);
    assert(hot->desired.w == (uint16_t)(hot->server.w + 40));
    assert(hot->desired.h == (uint16_t)(hot->server.h + 20));
    assert(hot->desired.x == hot->server.x);
    assert(hot->desired.y == hot->server.y);

    xcb_button_release_event_t release = {0};
    wm_handle_button_release(&s, &release);
    assert(stub_ungrab_pointer_count == 1);

    printf("test_resize_interaction passed\n");
    cleanup_server(&s);
}

static void test_resize_corner_top_left(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    handle_t h = add_mapped_client(&s, 4001, 4101);
    client_hot_t* hot = server_chot(&s, h);

    wm_start_interaction(&s, h, hot, false, RESIZE_TOP | RESIZE_LEFT, 100, 100);
    assert(stub_grab_pointer_count == 1);

    xcb_motion_notify_event_t motion = {0};
    motion.root_x = 110;
    motion.root_y = 105;
    motion.event = hot->frame;

    wm_handle_motion_notify(&s, &motion);

    assert(hot->desired.w == (uint16_t)(hot->server.w - 10));
    assert(hot->desired.h == (uint16_t)(hot->server.h - 5));
    assert(hot->desired.x == hot->server.x + 10);
    assert(hot->desired.y == hot->server.y + 5);

    printf("test_resize_corner_top_left passed\n");
    cleanup_server(&s);
}

static void test_keybinding_clean_mods(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    key_binding_t* b = calloc(1, sizeof(*b));
    b->keysym = XK_Escape;
    b->modifiers = XCB_MOD_MASK_1;
    b->action = ACTION_RESTART;
    small_vec_init(&s.config.key_bindings);
    small_vec_push(&s.config.key_bindings, b);

    g_restart_pending = 0;

    xcb_key_press_event_t ev = {0};
    ev.detail = 9;
    ev.state = XCB_MOD_MASK_1 | XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2;

    s.keysyms = xcb_key_symbols_alloc(s.conn);
    wm_handle_key_press(&s, &ev);

    assert(g_restart_pending == 1);

    printf("test_keybinding_clean_mods passed\n");
    xcb_key_symbols_free(s.keysyms);
    cleanup_server(&s);
}

static void test_keybinding_conflict_deterministic(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    key_binding_t* first = calloc(1, sizeof(*first));
    first->keysym = XK_Escape;
    first->modifiers = 0;
    first->action = ACTION_RESTART;

    key_binding_t* second = calloc(1, sizeof(*second));
    second->keysym = XK_Escape;
    second->modifiers = 0;
    second->action = ACTION_WORKSPACE_NEXT;

    small_vec_init(&s.config.key_bindings);
    small_vec_push(&s.config.key_bindings, first);
    small_vec_push(&s.config.key_bindings, second);

    g_restart_pending = 0;
    s.current_desktop = 0;

    xcb_key_press_event_t ev = {0};
    ev.detail = 9;
    ev.state = 0;

    s.keysyms = xcb_key_symbols_alloc(s.conn);
    wm_handle_key_press(&s, &ev);

    assert(g_restart_pending == 1);
    assert(s.current_desktop == 0);

    printf("test_keybinding_conflict_deterministic passed\n");
    xcb_key_symbols_free(s.keysyms);
    cleanup_server(&s);
}

static void test_key_grabs_from_config(void) {
    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    key_binding_t* b = calloc(1, sizeof(*b));
    b->keysym = XK_Escape;
    b->modifiers = XCB_MOD_MASK_1;
    b->action = ACTION_RESTART;
    small_vec_init(&s.config.key_bindings);
    small_vec_push(&s.config.key_bindings, b);

    wm_setup_keys(&s);

    assert(stub_ungrab_key_count >= 1);
    assert(stub_grab_key_count >= 1);
    assert(stub_last_grab_key_mods == XCB_MOD_MASK_1);
    assert(stub_last_grab_keycode != 0);

    printf("test_key_grabs_from_config passed\n");
    cleanup_server(&s);
}

int main(void) {
    test_click_to_focus();
    test_move_interaction();
    test_resize_interaction();
    test_resize_corner_top_left();
    test_keybinding_clean_mods();
    test_keybinding_conflict_deterministic();
    test_key_grabs_from_config();
    return 0;
}
