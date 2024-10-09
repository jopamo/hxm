#ifndef SLOTMAP_H
#define SLOTMAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "handle.h"

typedef struct slot_hdr {
    uint32_t gen;
    uint32_t next_free;
    uint8_t live;
    uint8_t _pad[3];
} slot_hdr_t;

typedef struct slotmap {
    slot_hdr_t* hdr;
    void* hot;
    void* cold;
    uint32_t cap;
    uint32_t free_head;
    size_t hot_sz;
    size_t cold_sz;

#ifdef SLOTMAP_TRACK_USED
    uint32_t used;
#endif
} slotmap_t;

typedef void (*slotmap_visit_fn)(void* hot, void* cold, handle_t h, void* user);

static inline uint32_t slotmap_capacity(const slotmap_t* sm) { return sm->cap; }

static inline bool slotmap_init(slotmap_t* sm, uint32_t cap, size_t hot_sz, size_t cold_sz) {
    memset(sm, 0, sizeof(*sm));
    sm->cap = cap;
    sm->hot_sz = hot_sz;
    sm->cold_sz = cold_sz;

    sm->hdr = (slot_hdr_t*)calloc(cap, sizeof(slot_hdr_t));
    sm->hot = calloc(cap, hot_sz);
    sm->cold = calloc(cap, cold_sz);

    if (!sm->hdr || !sm->hot || !sm->cold) {
        free(sm->hdr);
        free(sm->hot);
        free(sm->cold);
        memset(sm, 0, sizeof(*sm));
        return false;
    }

    // Index 0 is reserved for INVALID
    for (uint32_t i = 1; i < cap; i++) {
        sm->hdr[i].next_free = (i + 1u < cap) ? (i + 1u) : 0u;
        sm->hdr[i].gen = 1;  // Start generation at 1
        sm->hdr[i].live = 0;
    }
    sm->free_head = (cap > 1) ? 1u : 0u;

#ifdef SLOTMAP_TRACK_USED
    sm->used = 0;
#endif

    return true;
}

static inline void slotmap_destroy(slotmap_t* sm) {
    free(sm->hdr);
    free(sm->hot);
    free(sm->cold);
    memset(sm, 0, sizeof(*sm));
}

static inline bool slotmap_live(const slotmap_t* sm, handle_t h) {
    uint32_t idx = handle_index(h);
    if (idx == 0 || idx >= sm->cap) return false;
    const slot_hdr_t* sh = &sm->hdr[idx];
    return sh->live && sh->gen == handle_generation(h);
}

/*
 * Unchecked address computation helpers
 * These do not validate live/gen and may be used in hot paths
 * Prefer slotmap_hot_checked / slotmap_cold_checked at boundaries
 */
static inline void* slotmap_hot_unchecked(const slotmap_t* sm, uint32_t idx) {
    return (uint8_t*)sm->hot + (size_t)idx * sm->hot_sz;
}

static inline void* slotmap_cold_unchecked(const slotmap_t* sm, uint32_t idx) {
    return (uint8_t*)sm->cold + (size_t)idx * sm->cold_sz;
}

/*
 * Backwards compatible helpers
 * These compute addresses from a handle without validating it
 * Consider migrating call sites to slotmap_hot_checked / slotmap_cold_checked
 */
static inline void* slotmap_hot(const slotmap_t* sm, handle_t h) {
    uint32_t idx = handle_index(h);
    return slotmap_hot_unchecked(sm, idx);
}

static inline void* slotmap_cold(const slotmap_t* sm, handle_t h) {
    uint32_t idx = handle_index(h);
    return slotmap_cold_unchecked(sm, idx);
}

/*
 * Checked accessors
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

/*
 * Index iteration support
 * Useful for tests/debug and for teardown that needs to free per-slot resources
 */
static inline bool slotmap_is_used_idx(const slotmap_t* sm, uint32_t idx) {
    if (idx == 0 || idx >= sm->cap) return false;
    return sm->hdr[idx].live != 0;
}

static inline void* slotmap_hot_at(const slotmap_t* sm, uint32_t idx) {
    if (idx >= sm->cap) return NULL;
    return slotmap_hot_unchecked(sm, idx);
}

static inline void* slotmap_cold_at(const slotmap_t* sm, uint32_t idx) {
    if (idx >= sm->cap) return NULL;
    return slotmap_cold_unchecked(sm, idx);
}

static inline handle_t slotmap_handle_at(const slotmap_t* sm, uint32_t idx) {
    if (idx == 0 || idx >= sm->cap) return HANDLE_INVALID;
    const slot_hdr_t* sh = &sm->hdr[idx];
    if (!sh->live) return HANDLE_INVALID;
    return handle_make(idx, sh->gen);
}

/*
 * foreach helper
 * Visits live slots in ascending index order
 * Safe even if the callback reads hot/cold
 * Callback must not call slotmap_alloc/free on the same slotmap
 */
static inline void slotmap_for_each_used(slotmap_t* sm, slotmap_visit_fn fn, void* user) {
    for (uint32_t idx = 1; idx < sm->cap; idx++) {
        slot_hdr_t* sh = &sm->hdr[idx];
        if (!sh->live) continue;

        void* hot = slotmap_hot_unchecked(sm, idx);
        void* cold = slotmap_cold_unchecked(sm, idx);
        handle_t h = handle_make(idx, sh->gen);
        fn(hot, cold, h, user);
    }
}

static inline handle_t slotmap_alloc(slotmap_t* sm, void** out_hot, void** out_cold) {
    uint32_t idx = sm->free_head;
    if (idx == 0) return HANDLE_INVALID;

    slot_hdr_t* sh = &sm->hdr[idx];
    sm->free_head = sh->next_free;

    sh->live = 1;
    sh->next_free = 0;

#ifdef SLOTMAP_TRACK_USED
    sm->used++;
#endif

    void* hot = slotmap_hot_unchecked(sm, idx);
    void* cold = slotmap_cold_unchecked(sm, idx);
    memset(hot, 0, sm->hot_sz);
    memset(cold, 0, sm->cold_sz);

    if (out_hot) *out_hot = hot;
    if (out_cold) *out_cold = cold;

    return handle_make(idx, sh->gen);
}

static inline void slotmap_free(slotmap_t* sm, handle_t h) {
    uint32_t idx = handle_index(h);
    if (idx == 0 || idx >= sm->cap) return;

    slot_hdr_t* sh = &sm->hdr[idx];
    if (!sh->live || sh->gen != handle_generation(h)) return;

    sh->live = 0;

#ifdef SLOTMAP_TRACK_USED
    if (sm->used) sm->used--;
#endif

    sh->gen++;
    if (sh->gen == 0) sh->gen = 1;  // Avoid 0 generation

    sh->next_free = sm->free_head;
    sm->free_head = idx;
}

/*
 * Returns the number of live items
 */
static inline uint32_t slotmap_count(const slotmap_t* sm) {
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

/*
 * Optional: expose live count if tracking is enabled
 */
static inline uint32_t slotmap_used_count(const slotmap_t* sm) {
#ifdef SLOTMAP_TRACK_USED
    return sm->used;
#else
    (void)sm;
    return 0;
#endif
}

#endif  // SLOTMAP_H
