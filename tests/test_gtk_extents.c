#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

// External stubs
extern xcb_window_t stub_last_config_window;
extern int32_t stub_last_config_x;
extern int32_t stub_last_config_y;
extern uint32_t stub_last_config_w;
extern uint32_t stub_last_config_h;
extern int stub_configure_window_count;

void test_gtk_extents_inflation() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.conn = (xcb_connection_t*)malloc(1);

    // Init config
    config_init_defaults(&s.config);
    // Even if we have border width, GTK extents should override/hide it (undecorated)
    s.config.theme.border_width = 5;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    hot->self = h;
    hot->xid = 100;
    hot->frame = 200;
    hot->state = STATE_MAPPED;

    // Set visual geometry
    hot->desired.x = 50;
    hot->desired.y = 50;
    hot->desired.w = 400;
    hot->desired.h = 300;

    // Enable GTK extents
    hot->gtk_frame_extents_set = true;
    hot->gtk_extents.left = 10;
    hot->gtk_extents.right = 10;
    hot->gtk_extents.top = 20;
    hot->gtk_extents.bottom = 20;

    // Mark dirty
    hot->dirty = DIRTY_GEOM;

    // Flush
    stub_last_config_window = 0;
    stub_configure_window_count = 0;
    wm_flush_dirty(&s);

    // wm_flush_dirty configures frame FIRST, then client.
    // So stub_last_config_* will hold CLIENT configuration.
    // To check frame, we'd need to instrument stubs better or check order.
    // However, client is configured LAST.

    // Check Client (XID) configuration
    // It should be 0,0 relative to frame, and size inflated.
    // Inflated size: W = 400 + 10 + 10 = 420. H = 300 + 20 + 20 = 340.

    assert(stub_last_config_window == 100);  // XID
    assert(stub_last_config_x == 0);
    assert(stub_last_config_y == 0);
    assert(stub_last_config_w == 420);
    assert(stub_last_config_h == 340);
    assert(stub_configure_window_count == 2);

    printf("test_gtk_extents_inflation (client) passed\n");

    // Note: We can't easily verify frame configuration because the stub overwrites the values.
    // But since the logic is in the same block, if client is correct, frame calculation is likely correct too.
    // We can verify hot->server which should match client inflated size.

    assert(hot->server.w == 420);
    assert(hot->server.h == 340);
    // hot->server.x/y should match FRAME position (inflated).
    // Frame X = 50 - 10 = 40.
    // Frame Y = 50 - 20 = 30.
    assert(hot->server.x == 40);
    assert(hot->server.y == 30);

    printf("test_gtk_extents_inflation (server state) passed\n");

    render_free(&hot->render_ctx);
    if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
    slotmap_destroy(&s.clients);
    config_destroy(&s.config);
    free(s.conn);
}

int main() {
    test_gtk_extents_inflation();
    return 0;
}
