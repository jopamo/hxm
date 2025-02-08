#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"

void test_property_dirty_bits(void) {
    server_t s;
    memset(&s, 0, sizeof(s));

    // Mock atoms
    atoms.WM_NAME = 1;
    atoms._NET_WM_NAME = 2;
    atoms.WM_NORMAL_HINTS = 3;

    // Minimal init for slotmap and maps
    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) {
        fprintf(stderr, "Failed to init slotmap\n");
        return;
    }
    hash_map_init(&s.window_to_client);

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    hot->xid = 123;
    hot->dirty = DIRTY_NONE;

    hash_map_insert(&s.window_to_client, 123, handle_to_ptr(h));

    xcb_property_notify_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window = 123;
    ev.atom = atoms.WM_NAME;

    wm_handle_property_notify(&s, h, &ev);
    assert(hot->dirty & DIRTY_TITLE);

    ev.atom = atoms.WM_NORMAL_HINTS;
    wm_handle_property_notify(&s, h, &ev);
    assert(hot->dirty & DIRTY_HINTS);

    // Mock flush (can't really test cookie push without mock connection, but we can verify logic flow if we had one)
    // For unit test, verifying bits are set is sufficient for now.

    printf("test_property_dirty_bits passed\n");

    slotmap_destroy(&s.clients);
    hash_map_destroy(&s.window_to_client);
}

int main(void) {
    test_property_dirty_bits();
    return 0;
}
