#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

extern int stub_grab_button_count;
extern void xcb_stubs_reset(void);
extern int stub_map_window_count;
extern int stub_unmap_window_count;
extern xcb_atom_t stub_last_prop_atom;
extern uint32_t stub_last_prop_len;
extern uint8_t stub_last_prop_data[4096];
extern int stub_set_input_focus_count;
extern xcb_window_t stub_last_input_focus_window;
extern uint8_t stub_last_input_focus_revert;

static void setup_server_for_manage(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->is_test = true;
    xcb_stubs_reset();
    s->conn = xcb_connect(NULL, NULL);
    atoms_init(s->conn);

    s->root = 1;
    s->root_depth = 24;
    s->root_visual = 1;
    s->root_visual_type = xcb_get_visualtype(s->conn, 0);

    s->current_desktop = 0;
    s->desktop_count = 4;
    s->focused_client = HANDLE_INVALID;

    list_init(&s->focus_history);
    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s->layers[i]);

    s->config.theme.border_width = 1;
    s->config.theme.title_height = 10;

    slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
}

static handle_t alloc_test_client(server_t* s, xcb_window_t xid, int32_t desktop) {
    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold_ptr, 0, sizeof(client_cold_t));
    render_init(&hot->render_ctx);
    hot->self = h;
    hot->xid = xid;
    hot->state = STATE_NEW;
    hot->type = WINDOW_TYPE_NORMAL;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    hot->focus_override = -1;
    hot->transient_for = HANDLE_INVALID;
    hot->desktop = desktop;
    hot->initial_state = XCB_ICCCM_WM_STATE_NORMAL;
    hot->desired = (rect_t){0, 0, 200, 150};
    hot->visual_id = s->root_visual;
    hot->depth = s->root_depth;
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);
    return h;
}

static void set_client_mapped(server_t* s, handle_t h, xcb_window_t frame) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;
    hot->state = STATE_MAPPED;
    hot->frame = frame;
    hot->server.x = 0;
    hot->server.y = 0;
    hot->server.w = 100;
    hot->server.h = 80;
}

void test_focus_on_finish_manage(void) {
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
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s.layers[i]);

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
    h1_hot->stacking_index = -1;
    h1_hot->stacking_layer = -1;
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
    h2_hot->stacking_index = -1;
    h2_hot->stacking_layer = -1;
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
    h3_hot->stacking_index = -1;
    h3_hot->stacking_layer = -1;
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

void test_mru_cycling(void) {
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
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s.layers[i]);

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
        hot->stacking_index = -1;
        hot->stacking_layer = -1;
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

void test_move_interaction(void) {
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
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s.layers[i]);
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
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
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

void test_title_update(void) {
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

    wm_handle_reply(&s, &slot, &reply_data.r, NULL);

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

void test_finish_manage_visibility(void) {
    server_t s;
    setup_server_for_manage(&s);

    // Not visible when on a different desktop.
    handle_t h1 = alloc_test_client(&s, 201, 1);
    stub_map_window_count = 0;
    stub_unmap_window_count = 0;
    client_finish_manage(&s, h1);
    client_hot_t* hot1 = server_chot(&s, h1);
    assert(hot1->state == STATE_UNMAPPED);
    assert(stub_map_window_count == 0);
    assert(stub_unmap_window_count == 0);

    // Not visible when requested to start iconic.
    handle_t h2 = alloc_test_client(&s, 202, 0);
    client_hot_t* hot2 = server_chot(&s, h2);
    hot2->initial_state = XCB_ICCCM_WM_STATE_ICONIC;
    stub_map_window_count = 0;
    stub_unmap_window_count = 0;
    client_finish_manage(&s, h2);
    assert(hot2->state == STATE_UNMAPPED);
    assert(stub_map_window_count == 0);
    assert(stub_unmap_window_count == 0);

    // Visible on current desktop and normal initial state maps client + frame.
    handle_t h3 = alloc_test_client(&s, 203, 0);
    stub_map_window_count = 0;
    stub_unmap_window_count = 0;
    client_finish_manage(&s, h3);
    client_hot_t* hot3 = server_chot(&s, h3);
    assert(hot3->state == STATE_MAPPED);
    assert(stub_map_window_count == 2);
    assert(stub_unmap_window_count == 0);

    printf("test_finish_manage_visibility passed\n");
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

void test_finish_manage_show_desktop_hides(void) {
    server_t s;
    setup_server_for_manage(&s);
    s.showing_desktop = true;

    handle_t h = alloc_test_client(&s, 301, 0);
    stub_map_window_count = 0;
    stub_unmap_window_count = 0;
    client_finish_manage(&s, h);

    client_hot_t* hot = server_chot(&s, h);
    assert(hot->show_desktop_hidden == true);
    assert(hot->state == STATE_UNMAPPED);
    assert(stub_map_window_count == 2);
    assert(stub_unmap_window_count == 1);

    printf("test_finish_manage_show_desktop_hides passed\n");
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

void test_finish_manage_focus_override(void) {
    server_t s;
    setup_server_for_manage(&s);

    handle_t h1 = alloc_test_client(&s, 401, 0);
    client_finish_manage(&s, h1);
    assert(s.focused_client == h1);

    // Override off prevents a dialog from stealing focus.
    handle_t h2 = alloc_test_client(&s, 402, 0);
    client_hot_t* hot2 = server_chot(&s, h2);
    hot2->type = WINDOW_TYPE_DIALOG;
    hot2->focus_override = 0;
    client_finish_manage(&s, h2);
    assert(s.focused_client == h1);

    // Override on forces focus even for a normal window.
    handle_t h3 = alloc_test_client(&s, 403, 0);
    client_hot_t* hot3 = server_chot(&s, h3);
    hot3->focus_override = 1;
    client_finish_manage(&s, h3);
    assert(s.focused_client == h3);

    printf("test_finish_manage_focus_override passed\n");
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

void test_iconify_updates_focus(void) {
    server_t s;
    setup_server_for_manage(&s);

    handle_t h1 = alloc_test_client(&s, 501, 0);
    client_hot_t* hot1 = server_chot(&s, h1);
    hot1->state = STATE_MAPPED;
    hot1->frame = 1501;

    handle_t h2 = alloc_test_client(&s, 502, 0);
    client_hot_t* hot2 = server_chot(&s, h2);
    hot2->state = STATE_MAPPED;
    hot2->frame = 1502;

    wm_set_focus(&s, h1);
    wm_set_focus(&s, h2);
    assert(s.focused_client == h2);

    stub_unmap_window_count = 0;
    stub_last_prop_atom = 0;
    stub_last_prop_len = 0;
    memset(stub_last_prop_data, 0, sizeof(stub_last_prop_data));

    wm_client_iconify(&s, h2);

    assert(hot2->state == STATE_UNMAPPED);
    assert(stub_unmap_window_count == 1);
    assert(s.focused_client == h1);
    assert(stub_last_prop_atom == atoms.WM_STATE);
    assert(stub_last_prop_len == 2);
    uint32_t* vals = (uint32_t*)stub_last_prop_data;
    assert(vals[0] == XCB_ICCCM_WM_STATE_ICONIC);

    printf("test_iconify_updates_focus passed\n");
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

void test_restore_maps_window(void) {
    server_t s;
    setup_server_for_manage(&s);

    handle_t h = alloc_test_client(&s, 601, 0);
    client_hot_t* hot = server_chot(&s, h);
    hot->state = STATE_UNMAPPED;
    hot->frame = 1601;

    stub_map_window_count = 0;
    stub_last_prop_atom = 0;
    stub_last_prop_len = 0;
    memset(stub_last_prop_data, 0, sizeof(stub_last_prop_data));

    wm_client_restore(&s, h);

    assert(hot->state == STATE_MAPPED);
    assert(stub_map_window_count == 2);
    assert(stub_last_prop_atom == atoms.WM_STATE);
    assert(stub_last_prop_len == 2);
    uint32_t* vals = (uint32_t*)stub_last_prop_data;
    assert(vals[0] == XCB_ICCCM_WM_STATE_NORMAL);

    printf("test_restore_maps_window passed\n");
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

void test_set_focus_ignores_unmapped(void) {
    server_t s;
    setup_server_for_manage(&s);

    handle_t h1 = alloc_test_client(&s, 701, 0);
    handle_t h2 = alloc_test_client(&s, 702, 0);
    set_client_mapped(&s, h1, 1701);
    set_client_mapped(&s, h2, 1702);

    wm_set_focus(&s, h1);
    assert(s.focused_client == h1);

    client_hot_t* hot2 = server_chot(&s, h2);
    hot2->state = STATE_UNMAPPED;
    wm_set_focus(&s, h2);
    assert(s.focused_client == h1);

    printf("test_set_focus_ignores_unmapped passed\n");
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

void test_set_focus_revert_policy_and_root_fallback(void) {
    server_t s;
    setup_server_for_manage(&s);

    handle_t h1 = alloc_test_client(&s, 703, 0);
    set_client_mapped(&s, h1, 1703);
    client_cold_t* cold = server_ccold(&s, h1);
    cold->can_focus = true;

    stub_set_input_focus_count = 0;
    wm_set_focus(&s, h1);
    assert(stub_set_input_focus_count == 1);
    assert(stub_last_input_focus_window == 703);
    assert(stub_last_input_focus_revert == XCB_INPUT_FOCUS_POINTER_ROOT);

    client_unmanage(&s, h1);
    assert(s.focused_client == HANDLE_INVALID);
    assert(stub_last_input_focus_window == s.root);
    assert(stub_last_input_focus_revert == XCB_INPUT_FOCUS_POINTER_ROOT);

    printf("test_set_focus_revert_policy_and_root_fallback passed\n");
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

void test_unmanage_focus_prefers_parent(void) {
    server_t s;
    setup_server_for_manage(&s);

    handle_t parent = alloc_test_client(&s, 801, 0);
    handle_t child = alloc_test_client(&s, 802, 0);
    set_client_mapped(&s, parent, 1801);
    set_client_mapped(&s, child, 1802);

    client_hot_t* child_hot = server_chot(&s, child);
    child_hot->transient_for = parent;

    wm_set_focus(&s, parent);
    wm_set_focus(&s, child);
    assert(s.focused_client == child);

    client_unmanage(&s, child);
    assert(s.focused_client == parent);

    printf("test_unmanage_focus_prefers_parent passed\n");
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

void test_unmanage_focus_falls_back_to_mru(void) {
    server_t s;
    setup_server_for_manage(&s);

    handle_t parent = alloc_test_client(&s, 901, 0);
    handle_t other = alloc_test_client(&s, 902, 0);
    handle_t child = alloc_test_client(&s, 903, 0);
    set_client_mapped(&s, parent, 1901);
    set_client_mapped(&s, other, 1902);
    set_client_mapped(&s, child, 1903);

    client_hot_t* parent_hot = server_chot(&s, parent);
    parent_hot->state = STATE_UNMAPPED;

    client_hot_t* child_hot = server_chot(&s, child);
    child_hot->transient_for = parent;

    wm_set_focus(&s, other);
    wm_set_focus(&s, child);
    assert(s.focused_client == child);

    client_unmanage(&s, child);
    assert(s.focused_client == other);

    printf("test_unmanage_focus_falls_back_to_mru passed\n");
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

int main(void) {
    test_focus_on_finish_manage();
    test_mru_cycling();
    test_move_interaction();
    test_title_update();
    test_finish_manage_visibility();
    test_finish_manage_show_desktop_hides();
    test_finish_manage_focus_override();
    test_iconify_updates_focus();
    test_restore_maps_window();
    test_set_focus_ignores_unmapped();
    test_set_focus_revert_policy_and_root_fallback();
    test_unmanage_focus_prefers_parent();
    test_unmanage_focus_falls_back_to_mru();
    return 0;
}
