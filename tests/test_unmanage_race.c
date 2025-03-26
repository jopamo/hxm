#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

void test_idempotent_unmanage(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);

    list_init(&s.focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s.layers[i]);

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    assert(h != HANDLE_INVALID);
    assert(hot_ptr != NULL);
    assert(cold_ptr != NULL);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 123;
    hot->state = STATE_MAPPED;
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    list_init(&hot->transient_sibling);
    list_init(&hot->transients_head);
    list_init(&hot->focus_node);
    arena_init(&cold->string_arena, 512);

    // 1. First call to unmanage
    client_unmanage(&s, h);
    assert(!slotmap_live(&s.clients, h));

    // 2. Second call to unmanage (should be safe and no-op)
    // We need a way to detect if it did anything.
    // Since slotmap_live will be false, client_unmanage should return early.
    client_unmanage(&s, h);

    printf("test_idempotent_unmanage passed\n");
    slotmap_destroy(&s.clients);
    free(s.conn);
}

void test_destroy_unmanage_race(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);

    list_init(&s.focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s.layers[i]);

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    assert(h != HANDLE_INVALID);
    assert(hot_ptr != NULL);
    assert(cold_ptr != NULL);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 123;
    hot->state = STATE_MAPPED;
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    list_init(&hot->transient_sibling);
    list_init(&hot->transients_head);
    list_init(&hot->focus_node);
    arena_init(&cold->string_arena, 512);

    // Simulate DestroyNotify followed by UnmapNotify
    hot->state = STATE_DESTROYED;
    client_unmanage(&s, h);

    // UnmapNotify would call it again
    client_unmanage(&s, h);

    printf("test_destroy_unmanage_race passed\n");
    slotmap_destroy(&s.clients);
    free(s.conn);
}

int main(void) {
    test_idempotent_unmanage();
    test_destroy_unmanage_race();
    return 0;
}
