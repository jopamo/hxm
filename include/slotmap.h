/*
 * slotmap.h - Generational Arena (Slot Map)
 *
 * A container that provides stable, non-dangling references (handles) to objects
 * stored in an arena, even when objects are deleted and indices are reused
 *
 * Core idea:
 * - Slots are addressed by an index
 * - Each slot has a generation counter
 * - A handle encodes {index, generation}
 * - Access with a stale handle fails validation and returns NULL
 *
 * Notes:
 * - Not thread-safe
 * - Index 0 is reserved for HANDLE_INVALID
 * - Supports separate "hot" and "cold" storage per slot
 * - Optional debug features via macros below
 */

#ifndef SLOTMAP_H
#define SLOTMAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "handle.h"

#ifndef HANDLE_INVALID
#error "slotmap.h requires handle.h to define HANDLE_INVALID"
#endif

/* Configuration macros
 *
 * Define any of these before including slotmap.h
 *
 * - SLOTMAP_TRACK_USED
 *     Track live count in O(1)
 *
 * - SLOTMAP_ZERO_ON_ALLOC (default 1)
 *     Zero hot/cold storage on allocation
 *
 * - SLOTMAP_POISON_ON_FREE
 *     Fill hot/cold storage with a pattern on free (debug)
 *
 * - SLOTMAP_ASSERTS (default 1)
 *     Enable internal sanity assertions
 */

#ifndef SLOTMAP_ZERO_ON_ALLOC
#define SLOTMAP_ZERO_ON_ALLOC 1
#endif

#ifndef SLOTMAP_ASSERTS
#define SLOTMAP_ASSERTS 1
#endif

#if SLOTMAP_ASSERTS
#define SLOTMAP_ASSERT(x) assert(x)
#else
#define SLOTMAP_ASSERT(x) ((void)0)
#endif

typedef struct slot_hdr {
    uint32_t gen;
    uint32_t next_free;
    uint8_t live;
    uint8_t _pad[3];
} slot_hdr_t;

/* Optional allocator override
 * If you never call slotmap_init_ex, the default libc allocator is used
 */
typedef void* (*slotmap_malloc_fn)(void* ctx, size_t sz);
typedef void* (*slotmap_calloc_fn)(void* ctx, size_t n, size_t sz);
typedef void (*slotmap_free_fn)(void* ctx, void* p);

typedef struct slotmap_allocator {
    void* ctx;
    slotmap_malloc_fn malloc_fn;
    slotmap_calloc_fn calloc_fn;
    slotmap_free_fn free_fn;
} slotmap_allocator_t;

static inline void* slotmap__libc_malloc(void* ctx, size_t sz) {
    (void)ctx;
    return malloc(sz);
}

static inline void* slotmap__libc_calloc(void* ctx, size_t n, size_t sz) {
    (void)ctx;
    return calloc(n, sz);
}

static inline void slotmap__libc_free(void* ctx, void* p) {
    (void)ctx;
    free(p);
}

static inline slotmap_allocator_t slotmap_allocator_default(void) {
    slotmap_allocator_t a;
    a.ctx = NULL;
    a.malloc_fn = slotmap__libc_malloc;
    a.calloc_fn = slotmap__libc_calloc;
    a.free_fn = slotmap__libc_free;
    return a;
}

typedef struct slotmap {
    slot_hdr_t* hdr;
    void* hot;
    void* cold;

    uint32_t cap;
    uint32_t free_head;

    size_t hot_sz;
    size_t cold_sz;

    slotmap_allocator_t a;

#ifdef SLOTMAP_TRACK_USED
    uint32_t used;
#endif
} slotmap_t;

typedef void (*slotmap_visit_fn)(void* hot, void* cold, handle_t h, void* user);

static inline uint32_t slotmap_capacity(const slotmap_t* sm) { return sm ? sm->cap : 0u; }

static inline bool slotmap__mul_overflow_size(size_t a, size_t b, size_t* out) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(a, b, out);
#endif
#endif
    if (a == 0 || b == 0) {
        *out = 0;
        return false;
    }
    if (a > (SIZE_MAX / b)) return true;
    *out = a * b;
    return false;
}

static inline void slotmap__poison(void* p, size_t n, uint8_t byte) {
#ifdef SLOTMAP_POISON_ON_FREE
    if (p && n) memset(p, byte, n);
#else
    (void)p;
    (void)n;
    (void)byte;
#endif
}

static inline void slotmap__reset(slotmap_t* sm) {
    if (!sm) return;
    memset(sm, 0, sizeof(*sm));
}

/* Initialize with explicit allocator */
static inline bool slotmap_init_ex(slotmap_t* sm, uint32_t cap, size_t hot_sz, size_t cold_sz, slotmap_allocator_t a) {
    if (!sm) return false;

    slotmap__reset(sm);

    if (!a.malloc_fn || !a.calloc_fn || !a.free_fn) return false;

    /* cap must include the reserved index 0 */
    if (cap == 0) return false;

    sm->cap = cap;
    sm->hot_sz = hot_sz;
    sm->cold_sz = cold_sz;
    sm->a = a;

    /* Allocate header array */
    sm->hdr = (slot_hdr_t*)sm->a.calloc_fn(sm->a.ctx, (size_t)cap, sizeof(slot_hdr_t));
    if (!sm->hdr) {
        slotmap__reset(sm);
        return false;
    }

    /* Allocate hot/cold storage if requested */
    if (hot_sz != 0) {
        size_t bytes = 0;
        if (slotmap__mul_overflow_size((size_t)cap, hot_sz, &bytes)) {
            sm->a.free_fn(sm->a.ctx, sm->hdr);
            slotmap__reset(sm);
            return false;
        }
        sm->hot = sm->a.calloc_fn(sm->a.ctx, 1u, bytes);
        if (!sm->hot) {
            sm->a.free_fn(sm->a.ctx, sm->hdr);
            slotmap__reset(sm);
            return false;
        }
    }

    if (cold_sz != 0) {
        size_t bytes = 0;
        if (slotmap__mul_overflow_size((size_t)cap, cold_sz, &bytes)) {
            sm->a.free_fn(sm->a.ctx, sm->hot);
            sm->a.free_fn(sm->a.ctx, sm->hdr);
            slotmap__reset(sm);
            return false;
        }
        sm->cold = sm->a.calloc_fn(sm->a.ctx, 1u, bytes);
        if (!sm->cold) {
            sm->a.free_fn(sm->a.ctx, sm->hot);
            sm->a.free_fn(sm->a.ctx, sm->hdr);
            slotmap__reset(sm);
            return false;
        }
    }

    /* Build free list
     * Index 0 is reserved
     */
    sm->hdr[0].gen = 0;
    sm->hdr[0].live = 0;
    sm->hdr[0].next_free = 0;

    for (uint32_t i = 1; i < cap; i++) {
        sm->hdr[i].gen = 1;
        sm->hdr[i].live = 0;
        sm->hdr[i].next_free = (i + 1u < cap) ? (i + 1u) : 0u;
    }
    sm->free_head = (cap > 1) ? 1u : 0u;

#ifdef SLOTMAP_TRACK_USED
    sm->used = 0;
#endif

    return true;
}

/* Initialize using libc allocator */
static inline bool slotmap_init(slotmap_t* sm, uint32_t cap, size_t hot_sz, size_t cold_sz) {
    return slotmap_init_ex(sm, cap, hot_sz, cold_sz, slotmap_allocator_default());
}

static inline void slotmap_destroy(slotmap_t* sm) {
    if (!sm) return;
    if (sm->a.free_fn) {
        sm->a.free_fn(sm->a.ctx, sm->cold);
        sm->a.free_fn(sm->a.ctx, sm->hot);
        sm->a.free_fn(sm->a.ctx, sm->hdr);
    }
    slotmap__reset(sm);
}

static inline bool slotmap_live(const slotmap_t* sm, handle_t h) {
    if (!sm || !sm->hdr) return false;
    uint32_t idx = handle_index(h);
    if (idx == 0 || idx >= sm->cap) return false;
    const slot_hdr_t* sh = &sm->hdr[idx];
    return (sh->live != 0) && (sh->gen == handle_generation(h));
}

/* Unchecked address computation helpers
 * These do not validate live/gen and may be used in hot paths
 */
static inline void* slotmap_hot_unchecked(const slotmap_t* sm, uint32_t idx) {
    if (!sm || sm->hot_sz == 0 || !sm->hot) return NULL;
    return (uint8_t*)sm->hot + (size_t)idx * sm->hot_sz;
}

static inline void* slotmap_cold_unchecked(const slotmap_t* sm, uint32_t idx) {
    if (!sm || sm->cold_sz == 0 || !sm->cold) return NULL;
    return (uint8_t*)sm->cold + (size_t)idx * sm->cold_sz;
}

/* Backwards compatible helpers
 * Compute addresses from a handle without validating it
 */
static inline void* slotmap_hot(const slotmap_t* sm, handle_t h) { return slotmap_hot_unchecked(sm, handle_index(h)); }

static inline void* slotmap_cold(const slotmap_t* sm, handle_t h) {
    return slotmap_cold_unchecked(sm, handle_index(h));
}

/* Checked accessors
 * Return NULL if handle is invalid or stale
 */
static inline void* slotmap_hot_checked(const slotmap_t* sm, handle_t h) {
    if (!slotmap_live(sm, h)) return NULL;
    return slotmap_hot(sm, h);
}

static inline void* slotmap_cold_checked(const slotmap_t* sm, handle_t h) {
    if (!slotmap_live(sm, h)) return NULL;
    return slotmap_cold(sm, h);
}

/* Index iteration support */
static inline bool slotmap_is_used_idx(const slotmap_t* sm, uint32_t idx) {
    if (!sm || !sm->hdr) return false;
    if (idx == 0 || idx >= sm->cap) return false;
    return sm->hdr[idx].live != 0;
}

static inline void* slotmap_hot_at(const slotmap_t* sm, uint32_t idx) {
    if (!sm || idx >= sm->cap) return NULL;
    return slotmap_hot_unchecked(sm, idx);
}

static inline void* slotmap_cold_at(const slotmap_t* sm, uint32_t idx) {
    if (!sm || idx >= sm->cap) return NULL;
    return slotmap_cold_unchecked(sm, idx);
}

static inline handle_t slotmap_handle_at(const slotmap_t* sm, uint32_t idx) {
    if (!sm || !sm->hdr) return HANDLE_INVALID;
    if (idx == 0 || idx >= sm->cap) return HANDLE_INVALID;
    const slot_hdr_t* sh = &sm->hdr[idx];
    if (sh->live == 0) return HANDLE_INVALID;
    return handle_make(idx, sh->gen);
}

/* Visits live slots in ascending index order
 * Callback must not call slotmap_alloc/slotmap_free/slotmap_reserve on the same slotmap
 */
static inline void slotmap_for_each_used(slotmap_t* sm, slotmap_visit_fn fn, void* user) {
    if (!sm || !sm->hdr || !fn) return;
    for (uint32_t idx = 1; idx < sm->cap; idx++) {
        slot_hdr_t* sh = &sm->hdr[idx];
        if (sh->live == 0) continue;

        void* hot = slotmap_hot_unchecked(sm, idx);
        void* cold = slotmap_cold_unchecked(sm, idx);
        handle_t h = handle_make(idx, sh->gen);
        fn(hot, cold, h, user);
    }
}

/* Reserve capacity (strong safety)
 * Allocates fresh buffers and copies existing content so failure leaves sm unchanged
 */
static inline bool slotmap_reserve(slotmap_t* sm, uint32_t new_cap) {
    if (!sm || !sm->hdr) return false;
    if (new_cap <= sm->cap) return true;
    if (new_cap == 0) return false;

    slotmap_allocator_t a = sm->a;
    if (!a.calloc_fn || !a.malloc_fn || !a.free_fn) return false;

    size_t hdr_bytes = 0;
    if (slotmap__mul_overflow_size((size_t)new_cap, sizeof(slot_hdr_t), &hdr_bytes)) return false;

    slot_hdr_t* new_hdr = (slot_hdr_t*)a.calloc_fn(a.ctx, 1u, hdr_bytes);
    if (!new_hdr) return false;

    void* new_hot = NULL;
    void* new_cold = NULL;

    if (sm->hot_sz != 0) {
        size_t bytes = 0;
        if (slotmap__mul_overflow_size((size_t)new_cap, sm->hot_sz, &bytes)) {
            a.free_fn(a.ctx, new_hdr);
            return false;
        }
        new_hot = a.malloc_fn(a.ctx, bytes);
        if (!new_hot) {
            a.free_fn(a.ctx, new_hdr);
            return false;
        }
        memset(new_hot, 0, bytes);
    }

    if (sm->cold_sz != 0) {
        size_t bytes = 0;
        if (slotmap__mul_overflow_size((size_t)new_cap, sm->cold_sz, &bytes)) {
            a.free_fn(a.ctx, new_hot);
            a.free_fn(a.ctx, new_hdr);
            return false;
        }
        new_cold = a.malloc_fn(a.ctx, bytes);
        if (!new_cold) {
            a.free_fn(a.ctx, new_hot);
            a.free_fn(a.ctx, new_hdr);
            return false;
        }
        memset(new_cold, 0, bytes);
    }

    /* Copy old content */
    memcpy(new_hdr, sm->hdr, (size_t)sm->cap * sizeof(slot_hdr_t));

    if (sm->hot_sz != 0 && sm->hot) {
        memcpy(new_hot, sm->hot, (size_t)sm->cap * sm->hot_sz);
    }
    if (sm->cold_sz != 0 && sm->cold) {
        memcpy(new_cold, sm->cold, (size_t)sm->cap * sm->cold_sz);
    }

    /* Initialize new headers and push them onto free list */
    uint32_t old_cap = sm->cap;
    for (uint32_t i = old_cap; i < new_cap; i++) {
        new_hdr[i].gen = 1;
        new_hdr[i].live = 0;
        new_hdr[i].next_free = sm->free_head;
        sm->free_head = i;
    }

    /* Swap in */
    a.free_fn(a.ctx, sm->cold);
    a.free_fn(a.ctx, sm->hot);
    a.free_fn(a.ctx, sm->hdr);

    sm->hdr = new_hdr;
    sm->hot = new_hot;
    sm->cold = new_cold;
    sm->cap = new_cap;

    return true;
}

static inline bool slotmap_is_full(const slotmap_t* sm) { return !sm || sm->free_head == 0; }

static inline handle_t slotmap_alloc(slotmap_t* sm, void** out_hot, void** out_cold) {
    if (!sm || !sm->hdr) return HANDLE_INVALID;

    uint32_t idx = sm->free_head;
    if (idx == 0) return HANDLE_INVALID;

    slot_hdr_t* sh = &sm->hdr[idx];
    SLOTMAP_ASSERT(sh->live == 0);

    sm->free_head = sh->next_free;

    sh->live = 1;
    sh->next_free = 0;

#ifdef SLOTMAP_TRACK_USED
    sm->used++;
#endif

    void* hot = slotmap_hot_unchecked(sm, idx);
    void* cold = slotmap_cold_unchecked(sm, idx);

#if SLOTMAP_ZERO_ON_ALLOC
    if (hot && sm->hot_sz) memset(hot, 0, sm->hot_sz);
    if (cold && sm->cold_sz) memset(cold, 0, sm->cold_sz);
#endif

    if (out_hot) *out_hot = hot;
    if (out_cold) *out_cold = cold;

    return handle_make(idx, sh->gen);
}

/* Convenience allocator that grows on demand
 * Growth strategy: double capacity (min 2)
 */
static inline handle_t slotmap_alloc_grow(slotmap_t* sm, void** out_hot, void** out_cold) {
    handle_t h = slotmap_alloc(sm, out_hot, out_cold);
    if (h != HANDLE_INVALID) return h;

    if (!sm) return HANDLE_INVALID;

    uint32_t old_cap = sm->cap;
    uint32_t new_cap = (old_cap < 2u) ? 2u : old_cap * 2u;
    if (new_cap <= old_cap) return HANDLE_INVALID;

    if (!slotmap_reserve(sm, new_cap)) return HANDLE_INVALID;
    return slotmap_alloc(sm, out_hot, out_cold);
}

static inline void slotmap_free(slotmap_t* sm, handle_t h) {
    if (!sm || !sm->hdr) return;

    uint32_t idx = handle_index(h);
    if (idx == 0 || idx >= sm->cap) return;

    slot_hdr_t* sh = &sm->hdr[idx];
    if (sh->live == 0) return;
    if (sh->gen != handle_generation(h)) return;

    sh->live = 0;

#ifdef SLOTMAP_TRACK_USED
    if (sm->used) sm->used--;
#endif

    /* Invalidate all outstanding handles for this index */
    sh->gen++;
    if (sh->gen == 0) sh->gen = 1;

    /* Poison storage (debug) */
    slotmap__poison(slotmap_hot_unchecked(sm, idx), sm->hot_sz, 0xDD);
    slotmap__poison(slotmap_cold_unchecked(sm, idx), sm->cold_sz, 0xDD);

    /* Push back to free list */
    sh->next_free = sm->free_head;
    sm->free_head = idx;
}

/* Clear all slots and invalidate all existing handles
 * This rebuilds the free list
 */
static inline void slotmap_clear(slotmap_t* sm) {
    if (!sm || !sm->hdr) return;

    for (uint32_t i = 1; i < sm->cap; i++) {
        slot_hdr_t* sh = &sm->hdr[i];

        /* Bump generation to invalidate any previously created handle */
        sh->gen++;
        if (sh->gen == 0) sh->gen = 1;

        sh->live = 0;
        sh->next_free = (i + 1u < sm->cap) ? (i + 1u) : 0u;

        slotmap__poison(slotmap_hot_unchecked(sm, i), sm->hot_sz, 0xDD);
        slotmap__poison(slotmap_cold_unchecked(sm, i), sm->cold_sz, 0xDD);
    }

    sm->free_head = (sm->cap > 1) ? 1u : 0u;

#ifdef SLOTMAP_TRACK_USED
    sm->used = 0;
#endif
}

/* Clear with a destructor callback over currently-live slots
 * The callback may free resources owned by hot/cold data
 * Callback must not call slotmap_alloc/free/reserve on the same slotmap
 */
static inline void slotmap_clear_with(slotmap_t* sm, slotmap_visit_fn dtor, void* user) {
    if (!sm || !sm->hdr) return;
    if (dtor) slotmap_for_each_used(sm, dtor, user);
    slotmap_clear(sm);
}

/* Returns the number of live items */
static inline uint32_t slotmap_count(const slotmap_t* sm) {
    if (!sm || !sm->hdr) return 0;

#ifdef SLOTMAP_TRACK_USED
    return sm->used;
#else
    uint32_t c = 0;
    for (uint32_t i = 1; i < sm->cap; i++) {
        if (sm->hdr[i].live) c++;
    }
    return c;
#endif
}

/* If tracking is disabled, returns 0 */
static inline uint32_t slotmap_used_count(const slotmap_t* sm) {
#ifdef SLOTMAP_TRACK_USED
    return sm ? sm->used : 0u;
#else
    (void)sm;
    return 0u;
#endif
}

/* Debug-only consistency check
 * Verifies:
 * - free list indices are in range
 * - free list slots are not live
 * - no obvious self-loop at head
 *
 * This is intentionally lightweight and not a full cycle detector
 */
static inline bool slotmap_validate_basic(const slotmap_t* sm) {
    if (!sm) return false;
    if (sm->cap == 0) return false;
    if (!sm->hdr) return false;

    uint32_t slow = sm->free_head;
    uint32_t fast = sm->free_head;

    /* Floyd cycle detection (basic) */
    while (fast != 0) {
        if (fast >= sm->cap) return false;
        if (sm->hdr[fast].live != 0) return false;
        fast = sm->hdr[fast].next_free;

        if (fast == 0) break;
        if (fast >= sm->cap) return false;
        if (sm->hdr[fast].live != 0) return false;
        fast = sm->hdr[fast].next_free;

        if (slow != 0) {
            if (slow >= sm->cap) return false;
            slow = sm->hdr[slow].next_free;
        }

        if (fast != 0 && fast == slow) return false;
    }

    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* SLOTMAP_H */
