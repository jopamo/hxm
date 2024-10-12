#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "containers.h"
#include "event.h"
#include "handle_conv.h"
#include "wm.h"

// Stub required globals

void test_resize_logic() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);  // Mock connection for grabbing
    s.root = 1;

    // Init config
    s.config.theme.border_width = 5;
    s.config.theme.title_height = 20;

    // Init list heads
    list_init(&s.focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) {
        small_vec_init(&s.layers[i]);
    }

    // Init slotmap
    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) {
        fprintf(stderr, "Failed to init slotmap\n");
        exit(1);
    }

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;

    // Setup client
    hot->state = STATE_MAPPED;
    hot->frame = 999;
    hot->server.x = 100;
    hot->server.y = 100;
    hot->server.w = 200;
    hot->server.h = 200;
    hot->hints.min_w = 50;
    hot->hints.min_h = 50;
    hot->hints.max_w = 1000;
    hot->hints.max_h = 1000;
    hot->hints.base_w = 0;
    hot->hints.base_h = 0;
    hot->hints.inc_w = 1;
    hot->hints.inc_h = 1;

    // Init list nodes
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    list_init(&hot->transient_sibling);
    list_init(&hot->transients_head);
    list_init(&hot->focus_node);

    // Add to layer (stack_raise expects it to be in a list or will insert it)
    // Actually stack_raise checks if it's connected.
    // Let's manually insert it into layer 2 (NORMAL)
    hot->layer = LAYER_NORMAL;
    stack_raise(&s, h);

    // Init frame map
    hash_map_init(&s.frame_to_client);
    hash_map_insert(&s.frame_to_client, 999, handle_to_ptr(h));

    // ==========================================
    // Test 0: Hit testing via wm_handle_button_press
    // ==========================================

    xcb_button_press_event_t bev;
    memset(&bev, 0, sizeof(bev));
    bev.event = 999;  // Click on frame
    bev.root = 1;
    bev.detail = 1;  // Left click
    bev.state = 0;   // No mods
    bev.root_x = 500;
    bev.root_y = 500;

    // 0.1 Click on Left Border
    // border width is 5. Click at x=2.
    bev.event_x = 2;
    bev.event_y = 50;  // Middle Y

    wm_handle_button_press(&s, &bev);

    assert(s.interaction_mode == INTERACTION_RESIZE);
    assert(s.interaction_resize_dir == RESIZE_LEFT);
    printf("Test 0.1 Passed: Left Border Hit\n");

    // Reset
    s.interaction_mode = INTERACTION_NONE;
    s.interaction_resize_dir = RESIZE_NONE;

    // 0.2 Click on Top-Right Corner
    // w=200, bw=5 -> frame_w = 210.
    // Right border start at 210-5 = 205.
    // Top border < 5.
    bev.event_x = 208;  // Right border
    bev.event_y = 2;    // Top border

    wm_handle_button_press(&s, &bev);

    assert(s.interaction_mode == INTERACTION_RESIZE);
    assert(s.interaction_resize_dir == (RESIZE_TOP | RESIZE_RIGHT));
    printf("Test 0.2 Passed: Top-Right Corner Hit\n");

    // ==========================================
    // Test 1-5: Motion Logic (Reuse previous tests)
    // ==========================================
    // Setup interaction manually for motion tests to be consistent
    s.interaction_mode = INTERACTION_RESIZE;
    s.interaction_window = 999;
    s.interaction_start_x = 100;
    s.interaction_start_y = 100;
    s.interaction_start_w = 200;
    s.interaction_start_h = 200;
    s.interaction_pointer_x = 500;
    s.interaction_pointer_y = 500;

    // Test 1: Bottom Right Resize (dx=10, dy=10)
    s.interaction_resize_dir = RESIZE_BOTTOM | RESIZE_RIGHT;

    xcb_motion_notify_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.root_x = 510;
    ev.root_y = 510;

    wm_handle_motion_notify(&s, &ev);

    assert(hot->desired.w == 210);
    assert(hot->desired.h == 210);
    assert(hot->desired.x == 100);
    assert(hot->desired.y == 100);
    printf("Test 1 Passed: Bottom-Right Resize\n");

    // Test 2: Left Resize (dx=-10)
    s.interaction_resize_dir = RESIZE_LEFT;

    ev.root_x = 490;
    ev.root_y = 500;

    wm_handle_motion_notify(&s, &ev);

    assert(hot->desired.w == 210);
    assert(hot->desired.x == 90);
    assert(hot->desired.h == 200);
    assert(hot->desired.y == 100);
    printf("Test 2 Passed: Left Resize\n");

    // Test 3: Top Resize (dy=-10)
    s.interaction_resize_dir = RESIZE_TOP;

    ev.root_x = 500;
    ev.root_y = 490;

    wm_handle_motion_notify(&s, &ev);

    assert(hot->desired.h == 210);
    assert(hot->desired.y == 90);
    assert(hot->desired.w == 200);
    assert(hot->desired.x == 100);
    printf("Test 3 Passed: Top Resize\n");

    // Test 4: Top-Left Resize (dx=-20, dy=-20)
    s.interaction_resize_dir = RESIZE_TOP | RESIZE_LEFT;
    ev.root_x = 480;
    ev.root_y = 480;

    wm_handle_motion_notify(&s, &ev);

    assert(hot->desired.w == 220);
    assert(hot->desired.x == 80);
    assert(hot->desired.h == 220);
    assert(hot->desired.y == 80);
    printf("Test 4 Passed: Top-Left Resize\n");

    // Test 5: Min Size Constraint (Left Resize)
    s.interaction_resize_dir = RESIZE_LEFT;
    ev.root_x = 700;  // 500 + 200
    ev.root_y = 500;

    wm_handle_motion_notify(&s, &ev);

    assert(hot->desired.w == 50);
    assert(hot->desired.x == 250);
    printf("Test 5 Passed: Min Size Left Constraint\n");

    // Cleanup
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
    hash_map_destroy(&s.frame_to_client);
    slotmap_destroy(&s.clients);
    free(s.conn);
}

int main() {
    test_resize_logic();
    return 0;
}
