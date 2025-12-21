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

extern xcb_window_t stub_last_send_event_destination;
extern char stub_last_event[32];
extern int stub_kill_client_count;
extern uint32_t stub_last_kill_client_resource;

void test_transient_stacking() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    hash_map_init(&s.window_to_client);
    hash_map_init(&s.frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) list_init(&s.layers[i]);

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    // Allocate Parent
    void *p_hot_ptr = NULL, *p_cold_ptr = NULL;
    handle_t hp = slotmap_alloc(&s.clients, &p_hot_ptr, &p_cold_ptr);
    client_hot_t* hp_hot = (client_hot_t*)p_hot_ptr;
    hp_hot->self = hp;
    hp_hot->xid = 1;
    hp_hot->frame = 10;
    hp_hot->state = STATE_MAPPED;
    hp_hot->layer = LAYER_NORMAL;
    list_init(&hp_hot->transients_head);
    list_init(&hp_hot->stacking_node);
    stack_raise(&s, hp);

    // Allocate Transient
    void *t_hot_ptr = NULL, *t_cold_ptr = NULL;
    handle_t ht = slotmap_alloc(&s.clients, &t_hot_ptr, &t_cold_ptr);
    client_hot_t* ht_hot = (client_hot_t*)t_hot_ptr;
    ht_hot->self = ht;
    ht_hot->xid = 2;
    ht_hot->frame = 20;
    ht_hot->state = STATE_MAPPED;
    ht_hot->layer = LAYER_NORMAL;
    ht_hot->transient_for = hp;
    list_init(&ht_hot->transients_head);
    list_init(&ht_hot->transient_sibling);
    list_init(&ht_hot->stacking_node);
    list_insert(&ht_hot->transient_sibling, hp_hot->transients_head.prev, &hp_hot->transients_head);

    stack_place_above(&s, ht, hp);

    // Verify order: P then T
    assert(hp_hot->stacking_node.next == &ht_hot->stacking_node);
    assert(ht_hot->stacking_node.prev == &hp_hot->stacking_node);

    // Raise parent, should raise transient too
    stack_raise(&s, hp);
    // After raise, T should still be above P, and both at the end of the layer list
    assert(hp_hot->stacking_node.next == &ht_hot->stacking_node);
    assert(ht_hot->stacking_node.next == &s.layers[LAYER_NORMAL]);

    printf("test_transient_stacking passed\n");
    printf("test_transient_stacking passed\n");
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
    free(s.conn);
}

void test_transient_focus_return() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);
    list_init(&s.focus_history);
    hash_map_init(&s.window_to_client);
    hash_map_init(&s.frame_to_client);

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    // Parent
    void *p_hot_ptr = NULL, *p_cold_ptr = NULL;
    handle_t hp = slotmap_alloc(&s.clients, &p_hot_ptr, &p_cold_ptr);
    client_hot_t* hp_hot = (client_hot_t*)p_hot_ptr;
    hp_hot->self = hp;
    hp_hot->xid = 1;
    hp_hot->frame = 10;
    hp_hot->state = STATE_MAPPED;
    list_init(&hp_hot->focus_node);
    list_init(&hp_hot->transients_head);

    // Transient
    void *t_hot_ptr = NULL, *t_cold_ptr = NULL;
    handle_t ht = slotmap_alloc(&s.clients, &t_hot_ptr, &t_cold_ptr);
    client_hot_t* ht_hot = (client_hot_t*)t_hot_ptr;
    ht_hot->self = ht;
    ht_hot->xid = 2;
    ht_hot->frame = 20;
    ht_hot->state = STATE_MAPPED;
    ht_hot->transient_for = hp;
    list_init(&ht_hot->focus_node);
    list_init(&ht_hot->transients_head);
    list_init(&ht_hot->transient_sibling);

    wm_set_focus(&s, hp);
    wm_set_focus(&s, ht);

    assert(s.focused_client == ht);

    // Unmanage transient
    client_unmanage(&s, ht);

    // Focus should return to parent
    assert(s.focused_client == hp);

    printf("test_transient_focus_return passed\n");
    printf("test_transient_stacking passed\n");
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
    free(s.conn);
}

int main() {
    test_transient_stacking();
    test_transient_focus_return();
    return 0;
}