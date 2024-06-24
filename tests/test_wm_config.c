#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"

void test_configure_request_managed() {
    server_t s;
    memset(&s, 0, sizeof(s));

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
    hot->frame = 456;
    hot->desired = (rect_t){10, 10, 100, 100};
    hot->hints.min_w = 50;
    hot->hints.min_h = 50;
    hot->hints.max_w = 200;
    hot->hints.max_h = 200;

    hash_map_insert(&s.window_to_client, 123, handle_to_ptr(h));

    pending_config_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.window = 123;
    ev.x = 20;
    ev.y = 20;
    ev.width = 300;  // Above max
    ev.height = 30;  // Below min
    ev.mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

    wm_handle_configure_request(&s, h, &ev);

    assert(hot->desired.x == 20);
    assert(hot->desired.y == 20);
    assert(hot->desired.w == 200);  // Capped at max
    assert(hot->desired.h == 50);   // Capped at min
    assert(hot->dirty & DIRTY_GEOM);

    printf("test_configure_request_managed passed\n");

    slotmap_destroy(&s.clients);
    hash_map_destroy(&s.window_to_client);
}

int main() {
    test_configure_request_managed();
    return 0;
}
