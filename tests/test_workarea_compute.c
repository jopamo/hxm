#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"

void test_workarea_compute(void) {
    server_t s;
    memset(&s, 0, sizeof(s));

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) {
        fprintf(stderr, "Failed to init slotmap\n");
        return;
    }
    small_vec_init(&s.active_clients);
    s.conn = xcb_connect(NULL, NULL);

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h1 = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    assert(h1 != HANDLE_INVALID);
    assert(hot_ptr != NULL);
    assert(cold_ptr != NULL);
    (void)h1;
    client_hot_t* c1 = (client_hot_t*)hot_ptr;
    client_cold_t* cold1 = (client_cold_t*)cold_ptr;
    c1->state = STATE_MAPPED;
    c1->type = WINDOW_TYPE_DOCK;
    cold1->strut.top = 30;
    small_vec_push(&s.active_clients, handle_to_ptr(h1));

    handle_t h2 = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    assert(h2 != HANDLE_INVALID);
    assert(hot_ptr != NULL);
    assert(cold_ptr != NULL);
    (void)h2;
    client_hot_t* c2 = (client_hot_t*)hot_ptr;
    client_cold_t* cold2 = (client_cold_t*)cold_ptr;
    c2->state = STATE_MAPPED;
    c2->type = WINDOW_TYPE_DOCK;
    cold2->strut.left = 50;
    small_vec_push(&s.active_clients, handle_to_ptr(h2));

    rect_t wa;
    wm_compute_workarea(&s, &wa);

    printf("wa: x=%d y=%d w=%u h=%u\n", wa.x, wa.y, wa.w, wa.h);
    assert(wa.x == 50);
    assert(wa.y == 30);
    assert(wa.w == 1870);
    assert(wa.h == 1050);

    printf("test_workarea_compute passed\n");

    small_vec_destroy(&s.active_clients);
    slotmap_destroy(&s.clients);
    xcb_disconnect(s.conn);
}

void test_workarea_no_strut_for_dock(void) {
    server_t s;
    memset(&s, 0, sizeof(s));

    if (!slotmap_init(&s.clients, 8, sizeof(client_hot_t), sizeof(client_cold_t))) {
        fprintf(stderr, "Failed to init slotmap\n");
        return;
    }
    small_vec_init(&s.active_clients);
    s.conn = xcb_connect(NULL, NULL);

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    assert(h != HANDLE_INVALID);
    client_hot_t* c = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(c, 0, sizeof(*c));
    memset(cold, 0, sizeof(*cold));
    c->state = STATE_MAPPED;
    c->type = WINDOW_TYPE_DOCK;
    small_vec_push(&s.active_clients, handle_to_ptr(h));

    rect_t wa;
    wm_compute_workarea(&s, &wa);

    assert(wa.x == 0);
    assert(wa.y == 0);
    assert(wa.w == 1920);
    assert(wa.h == 1080);

    printf("test_workarea_no_strut_for_dock passed\n");

    small_vec_destroy(&s.active_clients);
    slotmap_destroy(&s.clients);
    xcb_disconnect(s.conn);
}

int main(void) {
    test_workarea_compute();
    test_workarea_no_strut_for_dock();
    return 0;
}
