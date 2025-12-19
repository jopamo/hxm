#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"

volatile sig_atomic_t g_reload_pending = 0;

void test_size_hints() {
    size_hints_t s;
    memset(&s, 0, sizeof(s));

    uint16_t w, h;

    // 1. Min size
    s.min_w = 100;
    s.min_h = 100;
    w = 50;
    h = 50;
    client_constrain_size(&s, &w, &h);
    assert(w == 100);
    assert(h == 100);

    // 2. Max size
    s.max_w = 200;
    s.max_h = 200;
    w = 250;
    h = 250;
    client_constrain_size(&s, &w, &h);
    assert(w == 200);
    assert(h == 200);

    // 3. Increments
    s.min_w = 100;
    s.min_h = 100;
    s.base_w = 100;
    s.base_h = 100;
    s.inc_w = 10;
    s.inc_h = 20;
    w = 115;
    h = 135;
    client_constrain_size(&s, &w, &h);
    assert(w == 110);
    assert(h == 120);

    // 4. Aspect ratio (1:1)
    memset(&s, 0, sizeof(s));
    s.min_aspect_num = 1;
    s.min_aspect_den = 1;
    s.max_aspect_num = 1;
    s.max_aspect_den = 1;
    w = 100;
    h = 50;
    client_constrain_size(&s, &w, &h);
    // Should be 1:1. 100x50 violates min_aspect (w/h >= 1/1).
    // Our logic: w = h * 1/1 = 50. (Wait, my logic made it wider or shorter?)
    // If w/h < min_aspect, w = h * min_num / min_den.
    // 100/50 = 2. 2 >= 1. So it does NOT violate min_aspect.
    // But it violates max_aspect (w/h <= 1/1). 2 <= 1 is false.
    // Max aspect logic: h = w * max_den / max_num = 100 * 1 / 1 = 100.
    assert(w == 100);
    assert(h == 100);

    printf("test_size_hints passed\n");
}

int main() {
    test_size_hints();
    return 0;
}
