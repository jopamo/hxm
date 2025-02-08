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

    void *hot_ptr, *cold_ptr;
    handle_t h1 = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    (void)h1;
    client_hot_t* c1 = (client_hot_t*)hot_ptr;
    client_cold_t* cold1 = (client_cold_t*)cold_ptr;
    c1->state = STATE_MAPPED;
    cold1->strut.top = 30;

    handle_t h2 = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    (void)h2;
    client_hot_t* c2 = (client_hot_t*)hot_ptr;
    client_cold_t* cold2 = (client_cold_t*)cold_ptr;
    c2->state = STATE_MAPPED;
    cold2->strut.left = 50;

    rect_t wa;
    wm_compute_workarea(&s, &wa);

    assert(wa.x == 50);
    assert(wa.y == 30);
    assert(wa.w == 1870);
    assert(wa.h == 1050);

    printf("test_workarea_compute passed\n");

    slotmap_destroy(&s.clients);
}

int main(void) {
    test_workarea_compute();
    return 0;
}
