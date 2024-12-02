#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bbox.h"
#include "client.h"
#include "config.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

extern xcb_window_t stub_last_config_window;
extern uint16_t stub_last_config_mask;
extern xcb_window_t stub_last_config_sibling;
extern uint32_t stub_last_config_stack_mode;
extern int stub_configure_window_count;

extern xcb_atom_t stub_last_prop_atom;
extern uint32_t stub_last_prop_len;
extern uint8_t stub_last_prop_data[1024];

static bool init_server(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->is_test = true;
    s->conn = (xcb_connection_t*)malloc(1);
    config_init_defaults(&s->config);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s->layers[i]);
    arena_init(&s->tick_arena, 4096);
    list_init(&s->focus_history);
    if (slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return true;
    free(s->conn);
    return false;
}

static void cleanup_server(server_t* s) {
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (!s->clients.hdr[i].live) continue;
        handle_t h = handle_make(i, s->clients.hdr[i].gen);
        client_hot_t* hot = server_chot(s, h);
        if (!hot) continue;
        render_free(&hot->render_ctx);
        if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
    }
    slotmap_destroy(&s->clients);
    for (int i = 0; i < LAYER_COUNT; i++) {
        small_vec_destroy(&s->layers[i]);
    }
    arena_destroy(&s->tick_arena);
    config_destroy(&s->config);
    free(s->conn);
}

static handle_t add_client(server_t* s, xcb_window_t xid, xcb_window_t frame, int layer) {
    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    hot->self = h;
    hot->xid = xid;
    hot->frame = frame;
    hot->layer = layer;
    hot->state = STATE_MAPPED;
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);
    list_init(&hot->focus_node);
    return h;
}

static void assert_layer_order(const server_t* s, int layer, const handle_t* handles, size_t count) {
    const small_vec_t* v = &s->layers[layer];
    assert(v->length == count);
    for (size_t i = 0; i < count; i++) {
        assert((handle_t)(uintptr_t)v->items[i] == handles[i]);
    }
}

void test_stack_restack_single_and_sibling() {
    server_t s;
    if (!init_server(&s)) return;

    handle_t ha = add_client(&s, 10, 110, LAYER_NORMAL);
    client_hot_t* a = server_chot(&s, ha);

    stub_configure_window_count = 0;
    stack_raise(&s, ha);

    assert(stub_configure_window_count == 1);
    assert(stub_last_config_window == a->frame);
    assert(stub_last_config_mask & XCB_CONFIG_WINDOW_STACK_MODE);
    assert((stub_last_config_mask & XCB_CONFIG_WINDOW_SIBLING) == 0);
    assert(stub_last_config_stack_mode == XCB_STACK_MODE_ABOVE);
    assert(stub_last_config_sibling == 0);

    handle_t hb = add_client(&s, 20, 120, LAYER_NORMAL);
    client_hot_t* b = server_chot(&s, hb);
    stack_raise(&s, hb);

    {
        handle_t order[] = {ha, hb};
        assert_layer_order(&s, LAYER_NORMAL, order, 2);
    }
    assert(stub_last_config_window == b->frame);
    assert(stub_last_config_mask & XCB_CONFIG_WINDOW_SIBLING);
    assert(stub_last_config_sibling == a->frame);
    assert(stub_last_config_stack_mode == XCB_STACK_MODE_ABOVE);

    stack_lower(&s, hb);

    {
        handle_t order[] = {hb, ha};
        assert_layer_order(&s, LAYER_NORMAL, order, 2);
    }
    assert(stub_last_config_window == b->frame);
    assert(stub_last_config_mask & XCB_CONFIG_WINDOW_SIBLING);
    assert(stub_last_config_sibling == a->frame);
    assert(stub_last_config_stack_mode == XCB_STACK_MODE_BELOW);

    printf("test_stack_restack_single_and_sibling passed\n");

    cleanup_server(&s);
}

void test_stack_cross_layer_sibling() {
    server_t s;
    if (!init_server(&s)) return;

    handle_t h1 = add_client(&s, 10, 110, LAYER_NORMAL);
    handle_t h2 = add_client(&s, 20, 120, LAYER_NORMAL);
    stack_raise(&s, h1);
    stack_raise(&s, h2);

    handle_t h3 = add_client(&s, 30, 130, LAYER_ABOVE);
    client_hot_t* c = server_chot(&s, h3);
    client_hot_t* top = server_chot(&s, h2);
    stack_raise(&s, h3);

    assert(stub_last_config_window == c->frame);
    assert(stub_last_config_mask & XCB_CONFIG_WINDOW_SIBLING);
    assert(stub_last_config_sibling == top->frame);
    assert(stub_last_config_stack_mode == XCB_STACK_MODE_ABOVE);

    printf("test_stack_cross_layer_sibling passed\n");

    cleanup_server(&s);
}

void test_stack_raise_transients_restack_count() {
    server_t s;
    if (!init_server(&s)) return;

    handle_t hp = add_client(&s, 10, 110, LAYER_NORMAL);
    handle_t ht1 = add_client(&s, 20, 120, LAYER_NORMAL);
    handle_t ht2 = add_client(&s, 30, 130, LAYER_NORMAL);

    client_hot_t* p = server_chot(&s, hp);
    client_hot_t* t1 = server_chot(&s, ht1);
    client_hot_t* t2 = server_chot(&s, ht2);

    t1->transient_for = hp;
    list_insert(&t1->transient_sibling, p->transients_head.prev, &p->transients_head);
    t2->transient_for = hp;
    list_insert(&t2->transient_sibling, p->transients_head.prev, &p->transients_head);

    stub_configure_window_count = 0;
    stack_raise(&s, hp);

    assert(stub_configure_window_count == 3);
    {
        handle_t order[] = {hp, ht1, ht2};
        assert_layer_order(&s, LAYER_NORMAL, order, 3);
    }

    printf("test_stack_raise_transients_restack_count passed\n");

    cleanup_server(&s);
}

void test_root_stacking_property_order() {
    server_t s;
    if (!init_server(&s)) return;

    s.root = 1;
    atoms._NET_CLIENT_LIST_STACKING = 400;

    handle_t hb = add_client(&s, 10, 110, LAYER_BELOW);
    handle_t hn1 = add_client(&s, 20, 120, LAYER_NORMAL);
    handle_t hn2 = add_client(&s, 30, 130, LAYER_NORMAL);
    handle_t ha = add_client(&s, 40, 140, LAYER_ABOVE);

    stack_raise(&s, hb);
    stack_raise(&s, hn1);
    stack_raise(&s, hn2);
    stack_raise(&s, ha);

    s.root_dirty |= ROOT_DIRTY_CLIENT_LIST_STACKING;
    stub_last_prop_atom = 0;
    wm_flush_dirty(&s);

    assert(stub_last_prop_atom == atoms._NET_CLIENT_LIST_STACKING);
    assert(stub_last_prop_len == 4);
    uint32_t* wins = (uint32_t*)stub_last_prop_data;
    assert(wins[0] == 10);
    assert(wins[1] == 20);
    assert(wins[2] == 30);
    assert(wins[3] == 40);

    printf("test_root_stacking_property_order passed\n");

    cleanup_server(&s);
}

void test_focus_raise_on_focus() {
    server_t s;
    if (!init_server(&s)) return;

    s.config.focus_raise = true;

    handle_t h1 = add_client(&s, 10, 110, LAYER_NORMAL);
    handle_t h2 = add_client(&s, 20, 120, LAYER_NORMAL);
    stack_raise(&s, h1);
    stack_raise(&s, h2);

    wm_set_focus(&s, h1);

    handle_t order[] = {h2, h1};
    assert_layer_order(&s, LAYER_NORMAL, order, 2);

    printf("test_focus_raise_on_focus passed\n");
    cleanup_server(&s);
}

int main() {
    test_stack_restack_single_and_sibling();
    test_stack_cross_layer_sibling();
    test_stack_raise_transients_restack_count();
    test_root_stacking_property_order();
    test_focus_raise_on_focus();
    return 0;
}
