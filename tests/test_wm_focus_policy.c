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

volatile sig_atomic_t g_reload_pending = 0;

extern int stub_grab_button_count;

void test_focus_on_finish_manage() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);  // Fake connection  // Fake connection
    list_init(&s.focus_history);
    s.focused_client = HANDLE_INVALID;
    hash_map_init(&s.window_to_client);
    hash_map_init(&s.frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) list_init(&s.layers[i]);

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    // 1. Manage first normal window -> should get focus
    void *h1_hot_ptr = NULL, *h1_cold_ptr = NULL;
    handle_t h1 = slotmap_alloc(&s.clients, &h1_hot_ptr, &h1_cold_ptr);
    client_hot_t* h1_hot = (client_hot_t*)h1_hot_ptr;
    memset(h1_hot, 0, sizeof(client_hot_t));
    memset(h1_cold_ptr, 0, sizeof(client_cold_t));
    render_init(&h1_hot->render_ctx);
    h1_hot->self = h1;
    h1_hot->xid = 101;
    h1_hot->type = WINDOW_TYPE_NORMAL;
    h1_hot->state = STATE_NEW;
    h1_hot->focus_override = -1;
    h1_hot->transient_for = HANDLE_INVALID;
    h1_hot->desktop = 0;
    list_init(&h1_hot->focus_node);
    list_init(&h1_hot->stacking_node);
    list_init(&h1_hot->transients_head);
    list_init(&h1_hot->transient_sibling);

    s.current_desktop = 0;
    stub_grab_button_count = 0;
    client_finish_manage(&s, h1);

    assert(s.focused_client == h1);
    assert(stub_grab_button_count == 3);  // Grabbed buttons 1, 2, 3

    // 2. Manage second normal window -> should NOT get focus (prevention of stealing)
    void *h2_hot_ptr = NULL, *h2_cold_ptr = NULL;
    handle_t h2 = slotmap_alloc(&s.clients, &h2_hot_ptr, &h2_cold_ptr);
    client_hot_t* h2_hot = (client_hot_t*)h2_hot_ptr;
    memset(h2_hot, 0, sizeof(client_hot_t));
    render_init(&h2_hot->render_ctx);
    h2_hot->self = h2;
    h2_hot->xid = 102;
    h2_hot->type = WINDOW_TYPE_NORMAL;
    h2_hot->state = STATE_NEW;
    h2_hot->focus_override = -1;
    h2_hot->transient_for = HANDLE_INVALID;
    h2_hot->desktop = 0;
    list_init(&h2_hot->focus_node);
    list_init(&h2_hot->stacking_node);
    list_init(&h2_hot->transients_head);
    list_init(&h2_hot->transient_sibling);

    client_finish_manage(&s, h2);
    assert(s.focused_client == h1);  // Focus stayed on h1

    // 3. Manage a dialog -> should get focus (allowed stealing)
    void *h3_hot_ptr = NULL, *h3_cold_ptr = NULL;
    handle_t h3 = slotmap_alloc(&s.clients, &h3_hot_ptr, &h3_cold_ptr);
    client_hot_t* h3_hot = (client_hot_t*)h3_hot_ptr;
    memset(h3_hot, 0, sizeof(client_hot_t));
    render_init(&h3_hot->render_ctx);
    h3_hot->self = h3;
    h3_hot->xid = 103;
    h3_hot->type = WINDOW_TYPE_DIALOG;
    h3_hot->state = STATE_NEW;
    h3_hot->focus_override = -1;
    h3_hot->transient_for = HANDLE_INVALID;
    h3_hot->desktop = 0;
    list_init(&h3_hot->focus_node);
    list_init(&h3_hot->stacking_node);
    list_init(&h3_hot->transients_head);
    list_init(&h3_hot->transient_sibling);

    client_finish_manage(&s, h3);
    assert(s.focused_client == h3);

    printf("test_focus_on_finish_manage passed\n");
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
    hash_map_destroy(&s.window_to_client);
    hash_map_destroy(&s.frame_to_client);
    free(s.conn);
}

void test_mru_cycling() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);  // Fake connection
    list_init(&s.focus_history);
    s.focused_client = HANDLE_INVALID;
    hash_map_init(&s.window_to_client);
    hash_map_init(&s.frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) list_init(&s.layers[i]);

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    // Create 3 windows
    handle_t h[3];
    for (int i = 0; i < 3; i++) {
        void *hot_ptr = NULL, *cold_ptr = NULL;
        h[i] = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
        client_hot_t* hot = (client_hot_t*)hot_ptr;
        memset(hot, 0, sizeof(client_hot_t));
        render_init(&hot->render_ctx);
        hot->self = h[i];
        hot->xid = 100 + i;
        hot->type = WINDOW_TYPE_NORMAL;
        hot->state = STATE_MAPPED;
        hot->focus_override = -1;
        hot->desktop = 0;
        list_init(&hot->focus_node);
        list_init(&hot->stacking_node);
        list_init(&hot->transients_head);
        list_init(&hot->transient_sibling);

        wm_set_focus(&s, h[i]);
    }

    // Initial MRU (top to bottom): h[2], h[1], h[0]
    assert(s.focused_client == h[2]);

    // Cycle Next (forward) -> should focus h[1]
    wm_cycle_focus(&s, true);
    assert(s.focused_client == h[1]);

    // Cycle Next (forward) -> should focus h[2] (because h[1] was moved to head, h[2] is now second)
    // Wait, MRU behavior:
    // Start: [2, 1, 0]
    // Cycle Next from 2 -> finds 1. wm_set_focus(1) -> [1, 2, 0]
    // Cycle Next from 1 -> finds 2. wm_set_focus(2) -> [2, 1, 0]
    // So it toggles. This is expected without a cycling session.
    wm_cycle_focus(&s, true);
    assert(s.focused_client == h[2]);

    // Let's test filtering: make h[1] a DOCK
    client_hot_t* hot1 = server_chot(&s, h[1]);
    hot1->type = WINDOW_TYPE_DOCK;

    // Start: [2, 1, 0], focused=2
    // Cycle Next from 2 -> skips 1 (DOCK) -> finds 0. wm_set_focus(0) -> [0, 2, 1]
    wm_cycle_focus(&s, true);
    assert(s.focused_client == h[0]);

    // Test workspace filtering
    client_hot_t* hot2 = server_chot(&s, h[2]);
    hot2->desktop = 1;
    s.current_desktop = 0;

    // Start: [0, 2, 1], focused=0
    // Cycle Next from 0 -> skips 2 (wrong desktop) -> skips 1 (DOCK) -> loops back to 0 -> stops
    // Wait, the loop stops when it hits start_node.
    wm_cycle_focus(&s, true);
    assert(s.focused_client == h[0]);

    printf("test_mru_cycling passed\n");
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
    hash_map_destroy(&s.window_to_client);
    hash_map_destroy(&s.frame_to_client);
    free(s.conn);
}

void test_move_interaction() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);  // Fake connection
    list_init(&s.focus_history);
    s.focused_client = HANDLE_INVALID;
    hash_map_init(&s.window_to_client);
    hash_map_init(&s.frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) list_init(&s.layers[i]);
    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    memset(hot, 0, sizeof(client_hot_t));
    hot->self = h;
    hot->xid = 100;
    hot->frame = 200;
    hot->state = STATE_MAPPED;
    hot->server.x = 10;
    hot->server.y = 10;
    hot->server.w = 100;
    hot->server.h = 100;
    hot->desired = hot->server;
    list_init(&hot->focus_node);
    list_init(&hot->stacking_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s.window_to_client, hot->xid, handle_to_ptr(h));
    hash_map_insert(&s.frame_to_client, hot->frame, handle_to_ptr(h));

    // Simulate Alt + Button 1 Press on window
    xcb_button_press_event_t ev = {0};
    ev.event = hot->xid;
    ev.detail = 1;
    ev.state = XCB_MOD_MASK_1;
    ev.root_x = 50;
    ev.root_y = 50;

    wm_handle_button_press(&s, &ev);

    assert(s.interaction_mode == INTERACTION_MOVE);
    assert(s.interaction_window == hot->frame);

    // Simulate Motion
    xcb_motion_notify_event_t mev = {0};
    mev.event = s.root;
    mev.root_x = 60;  // Moved by 10
    mev.root_y = 70;  // Moved by 20

    wm_handle_motion_notify(&s, &mev);

    assert(hot->desired.x == 10 + (60 - 50));
    assert(hot->desired.y == 10 + (70 - 50));
    assert(hot->dirty & DIRTY_GEOM);

    printf("test_move_interaction passed\n");
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
    hash_map_destroy(&s.window_to_client);
    hash_map_destroy(&s.frame_to_client);
    free(s.conn);
}

void test_title_update() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);  // Fake connection
    hash_map_init(&s.window_to_client);
    hash_map_init(&s.frame_to_client);
    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    memset(hot, 0, sizeof(client_hot_t));
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->self = h;
    hot->xid = 100;
    hot->state = STATE_MAPPED;
    hot->dirty = 0;
    arena_init(&cold->string_arena, 512);

    // Initialize required atoms
    atoms._NET_WM_NAME = 10;
    atoms.UTF8_STRING = 11;
    atoms.WM_NAME = 12;

    cookie_slot_t slot = {0};
    slot.client = h;
    slot.type = COOKIE_GET_PROPERTY;
    slot.data = atoms._NET_WM_NAME;

    // Simulate reply with new title
    struct {
        xcb_get_property_reply_t r;
        char title[6];
    } reply_data;
    memset(&reply_data, 0, sizeof(reply_data));
    reply_data.r.format = 8;
    reply_data.r.value_len = 5;
    reply_data.r.type = atoms.UTF8_STRING;
    memcpy(reply_data.title, "Hello", 5);

    wm_handle_reply(&s, &slot, &reply_data.r);

    assert(cold->title != NULL);
    assert(strcmp(cold->title, "Hello") == 0);
    assert(hot->dirty & DIRTY_FRAME_STYLE);

    printf("test_title_update passed\n");
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
    arena_destroy(&cold->string_arena);
    slotmap_destroy(&s.clients);
    hash_map_destroy(&s.window_to_client);
    hash_map_destroy(&s.frame_to_client);
    free(s.conn);
}

int main() {
    test_focus_on_finish_manage();
    test_mru_cycling();
    test_move_interaction();
    test_title_update();
    return 0;
}
