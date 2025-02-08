#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"
#include "xcb_utils.h"

void test_transient_cycle_prevention(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.conn = (xcb_connection_t*)malloc(1);
    atoms.WM_TRANSIENT_FOR = 100;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    // Create Client A
    void *hot_ptr_a = NULL, *cold_ptr_a = NULL;
    handle_t ha = slotmap_alloc(&s.clients, &hot_ptr_a, &cold_ptr_a);
    client_hot_t* hot_a = (client_hot_t*)hot_ptr_a;
    client_cold_t* cold_a = (client_cold_t*)cold_ptr_a;
    hot_a->xid = 10;
    hot_a->self = ha;
    hot_a->state = STATE_MAPPED;
    list_init(&hot_a->transients_head);
    list_init(&hot_a->transient_sibling);
    hash_map_init(&s.window_to_client);
    hash_map_insert(&s.window_to_client, 10, handle_to_ptr(ha));

    // Create Client B
    void *hot_ptr_b = NULL, *cold_ptr_b = NULL;
    handle_t hb = slotmap_alloc(&s.clients, &hot_ptr_b, &cold_ptr_b);
    client_hot_t* hot_b = (client_hot_t*)hot_ptr_b;
    client_cold_t* cold_b = (client_cold_t*)cold_ptr_b;
    hot_b->xid = 20;
    hot_b->self = hb;
    hot_b->state = STATE_MAPPED;
    list_init(&hot_b->transients_head);
    list_init(&hot_b->transient_sibling);
    hash_map_insert(&s.window_to_client, 20, handle_to_ptr(hb));

    // 1. Make A transient for B
    // Mock reply for A
    struct {
        xcb_get_property_reply_t reply;
        xcb_window_t win;
    } mock_a;
    memset(&mock_a, 0, sizeof(mock_a));
    mock_a.reply.format = 32;  // window is 32-bit
    mock_a.reply.type = XCB_ATOM_WINDOW;
    mock_a.reply.value_len = 1;
    mock_a.win = 20;  // B's window

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = ha;
    slot.data = ((uint64_t)10 << 32) | atoms.WM_TRANSIENT_FOR;

    wm_handle_reply(&s, &slot, &mock_a, NULL);

    assert(hot_a->transient_for == hb);
    // Check linkage (A should be in B's transients list)
    assert(hot_b->transients_head.next != &hot_b->transients_head);
    // Actually finding A in B's list is harder without iterating, but list shouldn't be empty

    // 2. Try to make B transient for A (Cycle!)
    // Mock reply for B
    struct {
        xcb_get_property_reply_t reply;
        xcb_window_t win;
    } mock_b;
    memset(&mock_b, 0, sizeof(mock_b));
    mock_b.reply.format = 32;
    mock_b.reply.type = XCB_ATOM_WINDOW;
    mock_b.reply.value_len = 1;
    mock_b.win = 10;  // A's window

    slot.client = hb;
    slot.data = ((uint64_t)20 << 32) | atoms.WM_TRANSIENT_FOR;

    wm_handle_reply(&s, &slot, &mock_b, NULL);

    // Assert that the cycle was prevented
    // hot_b->transient_for should be HANDLE_INVALID (or at least NOT ha)
    if (hot_b->transient_for == ha) {
        printf("FAIL: Cycle B->A created!\n");
        exit(1);
    }
    assert(hot_b->transient_for == HANDLE_INVALID);

    // 3. Try Self-Transient (A transient for A)
    // Reset A
    hot_a->transient_for = HANDLE_INVALID;
    // (Cleaning up list linkage is complex manually, let's just make a new client C)

    void *hot_ptr_c = NULL, *cold_ptr_c = NULL;
    handle_t hc = slotmap_alloc(&s.clients, &hot_ptr_c, &cold_ptr_c);
    client_hot_t* hot_c = (client_hot_t*)hot_ptr_c;
    hot_c->xid = 30;
    hot_c->self = hc;
    list_init(&hot_c->transients_head);
    list_init(&hot_c->transient_sibling);
    hash_map_insert(&s.window_to_client, 30, handle_to_ptr(hc));

    struct {
        xcb_get_property_reply_t reply;
        xcb_window_t win;
    } mock_c;
    memset(&mock_c, 0, sizeof(mock_c));
    mock_c.reply.format = 32;
    mock_c.reply.type = XCB_ATOM_WINDOW;
    mock_c.reply.value_len = 1;
    mock_c.win = 30;  // C's window

    slot.client = hc;
    slot.data = ((uint64_t)30 << 32) | atoms.WM_TRANSIENT_FOR;

    wm_handle_reply(&s, &slot, &mock_c, NULL);

    if (hot_c->transient_for == hc) {
        printf("FAIL: Self-transient C->C created!\n");
        exit(1);
    }
    assert(hot_c->transient_for == HANDLE_INVALID);

    printf("test_transient_cycle_prevention passed\n");

    // Cleanup
    arena_destroy(&cold_a->string_arena);  // Actually empty but for correctness
    arena_destroy(&cold_b->string_arena);
    // ...
    hash_map_destroy(&s.window_to_client);
    slotmap_destroy(&s.clients);
    free(s.conn);
}

void test_transient_orphan_handled(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.conn = (xcb_connection_t*)malloc(1);
    atoms.WM_TRANSIENT_FOR = 100;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    hash_map_init(&s.window_to_client);

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 40;
    hot->self = h;
    hot->state = STATE_MAPPED;
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);
    hash_map_insert(&s.window_to_client, 40, handle_to_ptr(h));

    struct {
        xcb_get_property_reply_t reply;
        xcb_window_t win;
    } mock;
    memset(&mock, 0, sizeof(mock));
    mock.reply.format = 32;
    mock.reply.type = XCB_ATOM_WINDOW;
    mock.reply.value_len = 1;
    mock.win = 9999;

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)40 << 32) | atoms.WM_TRANSIENT_FOR;

    wm_handle_reply(&s, &slot, &mock, NULL);

    assert(cold->transient_for_xid == 9999);
    assert(hot->transient_for == HANDLE_INVALID);
    assert(hot->transient_sibling.next == &hot->transient_sibling);
    assert(hot->transient_sibling.prev == &hot->transient_sibling);

    printf("test_transient_orphan_handled passed\n");

    hash_map_destroy(&s.window_to_client);
    slotmap_destroy(&s.clients);
    free(s.conn);
}

int main(void) {
    test_transient_cycle_prevention();
    test_transient_orphan_handled();
    return 0;
}
