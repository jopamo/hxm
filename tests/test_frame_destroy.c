#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "client.h"
#include "ds.h"
#include "event.h"
#include "handle_conv.h"
#include "src/wm_internal.h"  // For wm_start_interaction
#include "wm.h"

// Returns the managed client for a handle
static client_hot_t* must_get_client(server_t* s, handle_t h) {
    client_hot_t* c = server_chot(s, h);
    assert(c != NULL);
    return c;
}

// A tiny helper to synthesize an XCB generic event buffer
static xcb_generic_event_t make_destroy_event(uint8_t response_type, xcb_window_t win, xcb_window_t event_win) {
    xcb_destroy_notify_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = response_type;
    ev.window = win;
    ev.event = event_win;
    xcb_generic_event_t ge;
    memset(&ge, 0, sizeof(ge));
    memcpy(&ge, &ev, sizeof(ev));
    return ge;
}

static xcb_generic_event_t make_unmap_event(xcb_window_t win, xcb_window_t event_win) {
    xcb_unmap_notify_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_UNMAP_NOTIFY;
    ev.window = win;
    ev.event = event_win;
    ev.from_configure = 0;
    xcb_generic_event_t ge;
    memset(&ge, 0, sizeof(ge));
    memcpy(&ge, &ev, sizeof(ev));
    return ge;
}

static void server_init_for_test(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->is_test = true;
    s->root_depth = 24;
    s->root_visual_type = xcb_get_visualtype(NULL, 0);
    s->conn = (xcb_connection_t*)malloc(1);  // Mock connection
    s->root = 497;                           // Arbitrary root

    // Init config
    s->config.theme.border_width = 5;
    s->config.theme.title_height = 20;

    // Init list heads
    list_init(&s->focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) {
        small_vec_init(&s->layers[i]);
    }

    // Init slotmap
    if (!slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) {
        fprintf(stderr, "Failed to init slotmap\n");
        exit(1);
    }

    // Init lookups
    hash_map_init(&s->frame_to_client);
    hash_map_init(&s->window_to_client);
}

static void server_destroy_for_test(server_t* s) {
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (s->clients.hdr[i].live) {
            handle_t h = handle_make(i, s->clients.hdr[i].gen);
            client_hot_t* hot = server_chot(s, h);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    hash_map_destroy(&s->frame_to_client);
    hash_map_destroy(&s->window_to_client);
    slotmap_destroy(&s->clients);
    free(s->conn);
}

static handle_t test_create_managed_client(server_t* s, xcb_window_t client_xid, xcb_window_t frame_xid) {
    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
    assert(h != HANDLE_INVALID);
    assert(hot_ptr != NULL);
    assert(cold_ptr != NULL);
    client_hot_t* hot = (client_hot_t*)hot_ptr;

    hot->state = STATE_MAPPED;  // Use STATE_MAPPED which maps to a mapped state usually
    hot->xid = client_xid;
    hot->frame = frame_xid;

    // Some sensible defaults
    hot->server.x = 100;
    hot->server.y = 100;
    hot->server.w = 200;
    hot->server.h = 200;

    // Init list nodes
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    list_init(&hot->transient_sibling);
    list_init(&hot->transients_head);
    list_init(&hot->focus_node);

    // Register maps
    hash_map_insert(&s->window_to_client, client_xid, handle_to_ptr(h));
    // If your window_to_client also tracks frames (some WMs do, some don't, usually frame_to_client is separate)
    // Based on test_resize.c, there is s->frame_to_client.
    hash_map_insert(&s->frame_to_client, frame_xid, handle_to_ptr(h));

    // Also usually we want the client in a layer
    hot->layer = LAYER_NORMAL;
    stack_raise(s, h);

    return h;
}

static void test_frame_destroy_during_interaction_cancels_only(void) {
    printf("Running test_frame_destroy_during_interaction_cancels_only...\n");
    server_t s;
    server_init_for_test(&s);

    const xcb_window_t client_xid = 0x00600017;
    const xcb_window_t frame_xid = 0x00400024;

    handle_t h = test_create_managed_client(&s, client_xid, frame_xid);
    client_hot_t* c = must_get_client(&s, h);

    // Start an interactive resize on this client
    wm_start_interaction(&s, h, c, /*start_move*/ false, /*resize_dir*/ 9, /*root_x*/ 1919, /*root_y*/ 0, 0);

    assert(s.interaction_mode == INTERACTION_RESIZE);
    assert(s.interaction_handle == h);

    // Inject DestroyNotify for the frame window
    xcb_generic_event_t ge = make_destroy_event(XCB_DESTROY_NOTIFY, frame_xid, s.root);

    // We need to call the specific handler because event_process_one might need more setup or is static
    // wm_handle_destroy_notify is in wm.h
    wm_handle_destroy_notify(&s, (xcb_destroy_notify_event_t*)&ge);

    // Interaction must be canceled
    assert(s.interaction_mode == INTERACTION_NONE);
    assert(s.interaction_handle == HANDLE_INVALID);

    // Client must still be managed
    client_hot_t* c_after = must_get_client(&s, h);
    assert(c_after->xid == client_xid);

    printf("Passed.\n");
    server_destroy_for_test(&s);
}

static void test_frame_unmap_does_not_unmanage_client(void) {
    printf("Running test_frame_unmap_does_not_unmanage_client...\n");
    server_t s;
    server_init_for_test(&s);

    const xcb_window_t client_xid = 0x00600017;
    const xcb_window_t frame_xid = 0x00400024;

    handle_t h = test_create_managed_client(&s, client_xid, frame_xid);

    // Sanity
    assert(server_chot(&s, h) != NULL);

    // Inject UnmapNotify for the frame window
    xcb_generic_event_t ge = make_unmap_event(frame_xid, s.root);

    // Call handler directly
    wm_handle_unmap_notify(&s, (xcb_unmap_notify_event_t*)&ge);

    // Client must still be managed
    client_hot_t* c = must_get_client(&s, h);
    assert(c->xid == client_xid);

    printf("Passed.\n");
    server_destroy_for_test(&s);
}

static void test_unmanage_cancels_interaction_before_frame_destroy(void) {
    printf("Running test_unmanage_cancels_interaction_before_frame_destroy...\n");
    server_t s;
    server_init_for_test(&s);

    const xcb_window_t client_xid = 0x00600017;
    const xcb_window_t frame_xid = 0x00400024;

    handle_t h = test_create_managed_client(&s, client_xid, frame_xid);
    client_hot_t* c = must_get_client(&s, h);

    wm_start_interaction(&s, h, c, false, 9, 10, 10, 0);

    // Now unmanage the client explicitly
    // unmanage_client is likely `client_unmanage` or `unmanage_window`?
    // Checking client.h or similar. Based on list of files, src/client.c likely has it.
    // include/client.h likely declares it.
    // Let's assume client_unmanage(server_t* s, handle_t h) based on naming convention
    // or maybe it's just triggered by UnmapNotify/DestroyNotify on the client window.

    // Wait, the test says "unmanage the client explicitly".
    // I need to find the function for unmanaging.
    // Usually it is client_unmanage or wm_unmanage.
    // Looking at include/wm.h, there isn't an explicit unmanage function exposed there,
    // but wm_handle_unmap_notify calls it.

    // Let's look at src/client.c or include/client.h to find the unmanage function.
    // For now I will assume `client_unmanage` exists and is accessible.
    // If not I will fix it.

    client_unmanage(&s, h);

    // Must not still be interacting after unmanage
    assert(s.interaction_mode == INTERACTION_NONE);
    // assert(s.interaction_handle == 0);

    // Client removed
    assert(server_chot(&s, h) == NULL);

    printf("Passed.\n");
    server_destroy_for_test(&s);
}

int main(void) {
    test_frame_destroy_during_interaction_cancels_only();
    test_frame_unmap_does_not_unmanage_client();
    test_unmanage_cancels_interaction_before_frame_destroy();
    return 0;
}
