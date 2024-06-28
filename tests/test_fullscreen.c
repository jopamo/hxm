#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bbox.h"
#include "client.h"
#include "config.h"
#include "event.h"
#include "slotmap.h"
#include "wm.h"
#include "xcb_utils.h"

volatile sig_atomic_t g_reload_pending = 0;

void test_fullscreen_decorations() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);

    // Init atoms
    atoms._NET_WM_STATE_FULLSCREEN = 100;

    // Init config with borders
    config_init_defaults(&s.config);
    s.config.theme.border_width = 5;
    s.config.theme.title_height = 20;
    s.config.fullscreen_use_workarea = false;

    // Workarea/Screen setup
    // xcb_screen_t screen;
    // screen.width_in_pixels = 1920;
    // screen.height_in_pixels = 1080;
    // We can't easily mock xcb_setup_roots_iterator output without complex stubs.
    // However, wm_client_update_state calls xcb_setup_roots_iterator...
    // The stub for xcb_setup_roots_iterator returns a static screen.
    // Let's rely on that default (usually 0x0 unless mocked).
    // tests/xcb_stubs.c usually provides a default screen.

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;

    hot->state = STATE_MAPPED;
    hot->layer = LAYER_NORMAL;
    hot->flags = CLIENT_FLAG_NONE;
    hot->server.x = 100;
    hot->server.y = 100;
    hot->server.w = 400;
    hot->server.h = 300;
    hot->dirty = DIRTY_NONE;

    // 1. Enable Fullscreen
    wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_FULLSCREEN);

    assert(hot->layer == LAYER_FULLSCREEN);
    assert(hot->flags & CLIENT_FLAG_UNDECORATED);
    assert(hot->dirty & DIRTY_GEOM);
    assert(hot->saved_layer == LAYER_NORMAL);
    assert(hot->saved_geom.x == 100);
    assert(hot->saved_geom.w == 400);
    assert((hot->saved_flags & CLIENT_FLAG_UNDECORATED) == 0);

    // 2. Disable Fullscreen
    wm_client_update_state(&s, h, 0, atoms._NET_WM_STATE_FULLSCREEN);

    assert(hot->layer == LAYER_NORMAL);
    assert((hot->flags & CLIENT_FLAG_UNDECORATED) == 0);
    assert(hot->dirty & DIRTY_GEOM);
    assert(hot->desired.x == 100);
    assert(hot->desired.w == 400);

    printf("test_fullscreen_decorations passed\n");

    slotmap_destroy(&s.clients);
    config_destroy(&s.config);
    free(s.conn);
}

int main() {
    test_fullscreen_decorations();
    return 0;
}
