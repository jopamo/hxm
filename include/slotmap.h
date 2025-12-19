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
} slotmap_t;

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
        return false;
    }

    // Index 0 is reserved for INVALID
    for (uint32_t i = 1; i < cap; i++) {
        sm->hdr[i].next_free = (i + 1u < cap) ? (i + 1u) : 0u;
        sm->hdr[i].gen = 1;  // Start generation at 1
    }
    sm->free_head = (cap > 1) ? 1u : 0u;
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

static inline void* slotmap_hot(const slotmap_t* sm, handle_t h) {
    uint32_t idx = handle_index(h);
    return (uint8_t*)sm->hot + (size_t)idx * sm->hot_sz;
}

static inline void* slotmap_cold(const slotmap_t* sm, handle_t h) {
    uint32_t idx = handle_index(h);
    return (uint8_t*)sm->cold + (size_t)idx * sm->cold_sz;
}

static inline handle_t slotmap_alloc(slotmap_t* sm, void** out_hot, void** out_cold) {
    uint32_t idx = sm->free_head;
    if (idx == 0) return HANDLE_INVALID;

    slot_hdr_t* sh = &sm->hdr[idx];
    sm->free_head = sh->next_free;

    sh->live = 1;
    sh->next_free = 0;

    void* hot = (uint8_t*)sm->hot + (size_t)idx * sm->hot_sz;
    void* cold = (uint8_t*)sm->cold + (size_t)idx * sm->cold_sz;
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
    sh->gen++;
    if (sh->gen == 0) sh->gen = 1;  // Avoid 0 generation if possible

    sh->next_free = sm->free_head;
    sm->free_head = idx;
}

#endif  // SLOTMAP_H