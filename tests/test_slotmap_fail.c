#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int g_fail_at = 0;
static int g_call_count = 0;

static void* test_calloc(size_t n, size_t sz) {
    g_call_count++;
    if (g_call_count == g_fail_at) return NULL;

    size_t total = n * sz;
    void* p = malloc(total);
    if (!p) return NULL;
    memset(p, 0, total);
    return p;
}

#define calloc test_calloc
#include "slotmap.h"
#undef calloc

static void assert_slotmap_init_fails_at(int fail_at) {
    slotmap_t sm;
    g_call_count = 0;
    g_fail_at = fail_at;

    bool ok = slotmap_init(&sm, 4, sizeof(int), sizeof(int));
    assert(!ok);
    assert(sm.hdr == NULL);
    assert(sm.hot == NULL);
    assert(sm.cold == NULL);
    assert(sm.cap == 0);
}

int main(void) {
    assert_slotmap_init_fails_at(1);
    assert_slotmap_init_fails_at(2);
    assert_slotmap_init_fails_at(3);
    return 0;
}
