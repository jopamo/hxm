#include <assert.h>
#include <stdio.h>

#include "bbox.h"

void test_dirty_region_union_and_clamp() {
    dirty_region_t r;
    dirty_region_reset(&r);

    dirty_region_union_rect(&r, 10, 10, 20, 20);
    assert(r.valid);
    assert(r.x == 10);
    assert(r.y == 10);
    assert(r.w == 20);
    assert(r.h == 20);

    dirty_region_t r2 = dirty_region_make(25, 5, 10, 10);
    dirty_region_union(&r, &r2);
    assert(r.valid);
    assert(r.x == 10);
    assert(r.y == 5);
    assert(r.w == 25);
    assert(r.h == 25);

    dirty_region_clamp(&r, 0, 0, 20, 20);
    assert(r.valid);
    assert(r.x == 10);
    assert(r.y == 5);
    assert(r.w == 10);
    assert(r.h == 15);

    printf("test_dirty_region_union_and_clamp passed\n");
}

void test_dirty_region_invalid_inputs() {
    dirty_region_t r = dirty_region_make(0, 0, 5, 5);
    dirty_region_t invalid;
    dirty_region_reset(&invalid);

    dirty_region_union(&r, &invalid);
    assert(r.valid);
    assert(r.x == 0);
    assert(r.y == 0);
    assert(r.w == 5);
    assert(r.h == 5);

    dirty_region_t zero = dirty_region_make(0, 0, 0, 10);
    assert(!zero.valid);

    dirty_region_clamp(&r, 40, 40, 10, 10);
    assert(!r.valid);
    assert(r.w == 0);
    assert(r.h == 0);

    printf("test_dirty_region_invalid_inputs passed\n");
}

int main() {
    test_dirty_region_union_and_clamp();
    test_dirty_region_invalid_inputs();
    return 0;
}
