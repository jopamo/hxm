/* src/containers.c
 * Container layout and management
 */

#include "containers.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bbox.h"

/*
 * containers.c
 *
 * Includes:
 *  - arena allocator (bump allocator with linked blocks)
 *  - small vector (inline storage, grows to heap)
 *  - hash map (open addressing, linear probing, backshift delete)
 *
 * Notes:
 *  - arena allocations are 8-byte aligned
 *  - hash_map reserves key=0 as empty sentinel (use XCB_NONE replacement too)
 */

/* -----------------------------
 * Arena allocator
 * ----------------------------- */

void arena_init(struct arena* a, size_t block_size) {
    a->first = NULL;
    a->current = NULL;
    a->pos = 0;
    a->block_size = block_size ? block_size : 4096;
}

static arena_block_t* arena_add_block(struct arena* a, size_t min_size) {
    size_t payload = a->block_size;
    if (payload < min_size) payload = min_size;

    size_t size = sizeof(arena_block_t) + payload;
    arena_block_t* block = (arena_block_t*)malloc(size);
    if (!block) {
        LOG_ERROR("arena allocation failed");
        abort();
    }

    block->next = NULL;
    block->size = payload;
    block->used = 0;

    if (a->current) {
        a->current->next = block;
    } else {
        a->first = block;
    }

    a->current = block;
    a->pos = 0;
    return block;
}

void* arena_alloc(struct arena* a, size_t size) {
    if (!a) return NULL;

    // Align to 8 bytes
    size = (size + 7u) & ~7u;

    if (size == 0) size = 8;

    if (!a->current) {
        if (a->first) {
            a->current = a->first;
            a->pos = 0;
        } else {
            arena_add_block(a, size);
        }
    }

    if (a->pos + size > a->current->size) {
        if (a->current->next && size <= a->current->next->size) {
            a->current = a->current->next;
            a->pos = 0;
        } else {
            arena_add_block(a, size);
        }
    }

    void* ptr = (void*)(a->current->data + a->pos);
    a->pos += size;
    a->current->used = a->pos;
    return ptr;
}

char* arena_strndup(struct arena* a, const char* s, size_t n) {
    if (!s) return NULL;
    char* res = (char*)arena_alloc(a, n + 1);
    memcpy(res, s, n);
    res[n] = '\0';
    return res;
}

char* arena_strdup(struct arena* a, const char* s) {
    if (!s) return NULL;
    return arena_strndup(a, s, strlen(s));
}

void arena_reset(struct arena* a) {
    if (!a) return;

    a->current = a->first;
    a->pos = 0;

    arena_block_t* b = a->first;
    while (b) {
        b->used = 0;
        b = b->next;
    }
}

void arena_destroy(struct arena* a) {
    if (!a) return;

    arena_block_t* block = a->first;
    while (block) {
        arena_block_t* next = block->next;
        free(block);
        block = next;
    }

    a->first = NULL;
    a->current = NULL;
    a->pos = 0;
    a->block_size = 0;
}

/* -----------------------------
 * Small vector
 * ----------------------------- */

void small_vec_init(small_vec_t* v) {
    v->items = v->inline_storage;
    v->length = 0;
    v->capacity = SMALL_VEC_INLINE_CAP;
}

static void small_vec_grow(small_vec_t* v, size_t min_cap) {
    size_t new_cap = v->capacity ? v->capacity : SMALL_VEC_INLINE_CAP;
    while (new_cap < min_cap) new_cap *= 2;

    void** new_items;
    if (v->items == v->inline_storage) {
        new_items = (void**)malloc(new_cap * sizeof(void*));
        if (new_items) memcpy(new_items, v->inline_storage, v->length * sizeof(void*));
    } else {
        new_items = (void**)realloc(v->items, new_cap * sizeof(void*));
    }

    if (!new_items) {
        LOG_ERROR("small_vec allocation failed");
        abort();
    }

    v->items = new_items;
    v->capacity = new_cap;
}

void small_vec_push(small_vec_t* v, void* item) {
    if (v->length == v->capacity) {
        small_vec_grow(v, v->capacity ? (v->capacity * 2) : SMALL_VEC_INLINE_CAP);
    }
    v->items[v->length++] = item;
}

void* small_vec_pop(small_vec_t* v) {
    if (v->length == 0) return NULL;
    return v->items[--v->length];
}

void* small_vec_get(const small_vec_t* v, size_t idx) {
    if (!v || idx >= v->length) return NULL;
    return v->items[idx];
}

void small_vec_clear(small_vec_t* v) { v->length = 0; }

void small_vec_destroy(small_vec_t* v) {
    if (v->items != v->inline_storage) free(v->items);
    v->items = v->inline_storage;
    v->length = 0;
    v->capacity = SMALL_VEC_INLINE_CAP;
}

/* -----------------------------
 * Hash map
 * ----------------------------- */

static inline uint32_t hash_key(uint64_t key) {
    // MurmurHash3 finalizer for 64-bit keys -> 32-bit hash
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return (uint32_t)key;
}

static inline size_t probe_next(size_t idx, size_t mask) { return (idx + 1) & mask; }

static size_t round_up_pow2(size_t n) {
    if (n < 16) return 16;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_MAX > 0xffffffffu
    n |= n >> 32;
#endif
    n++;
    return n;
}

static void hash_map_resize(hash_map_t* map, size_t new_capacity) {
    new_capacity = round_up_pow2(new_capacity);

    hash_map_entry_t* new_entries = (hash_map_entry_t*)calloc(new_capacity, sizeof(hash_map_entry_t));
    if (!new_entries) {
        LOG_ERROR("hash_map allocation failed");
        abort();
    }

    if (map->entries && map->capacity) {
        for (size_t i = 0; i < map->capacity; i++) {
            hash_map_entry_t* entry = &map->entries[i];
            if (entry->key != 0) {
                size_t mask = new_capacity - 1;
                size_t idx = entry->hash & mask;
                while (new_entries[idx].key != 0) idx = probe_next(idx, mask);
                new_entries[idx] = *entry;
            }
        }
    }

    free(map->entries);
    map->entries = new_entries;
    map->capacity = new_capacity;
    map->max_load = (new_capacity * 3) / 4;
}

void hash_map_init(hash_map_t* map) {
    map->capacity = 0;
    map->size = 0;
    map->max_load = 0;
    map->entries = NULL;
}

void hash_map_destroy(hash_map_t* map) {
    free(map->entries);
    map->entries = NULL;
    map->capacity = 0;
    map->size = 0;
    map->max_load = 0;
}

bool hash_map_insert(hash_map_t* map, uint64_t key, void* value) {
    assert(key != 0 && "key=0 is reserved for hash_map_t");

    if (map->capacity == 0 || map->size >= map->max_load) {
        size_t new_cap = map->capacity ? (map->capacity * 2) : 16;
        hash_map_resize(map, new_cap);
    }

    uint32_t hash = hash_key(key);
    size_t mask = map->capacity - 1;
    size_t idx = hash & mask;

    while (map->entries[idx].key != 0) {
        if (map->entries[idx].key == key) {
            map->entries[idx].value = value;
            return true;
        }
        idx = probe_next(idx, mask);
    }

    map->entries[idx].key = key;
    map->entries[idx].value = value;
    map->entries[idx].hash = hash;
    map->size++;
    return false;
}

void* hash_map_get(const hash_map_t* map, uint64_t key) {
    assert(key != 0 && "key=0 is reserved for hash_map_t");
    if (!map || map->capacity == 0) return NULL;

    uint32_t hash = hash_key(key);
    size_t mask = map->capacity - 1;
    size_t idx = hash & mask;

    while (map->entries[idx].key != 0) {
        if (map->entries[idx].key == key) return map->entries[idx].value;
        idx = probe_next(idx, mask);
    }

    return NULL;
}

bool hash_map_remove(hash_map_t* map, uint64_t key) {
    assert(key != 0 && "key=0 is reserved for hash_map_t");
    if (!map || map->capacity == 0) return false;

    size_t mask = map->capacity - 1;
    uint32_t hash = hash_key(key);
    size_t idx = hash & mask;

    while (map->entries[idx].key != 0) {
        if (map->entries[idx].key == key) {
            size_t hole = idx;
            size_t j = probe_next(idx, mask);

            while (map->entries[j].key != 0) {
                uint32_t h = map->entries[j].hash;
                size_t home = h & mask;

                bool should_move;
                if (home <= j) {
                    should_move = (home <= hole && hole < j);
                } else {
                    should_move = (hole < j) || (home <= hole);
                }

                if (should_move) {
                    map->entries[hole] = map->entries[j];
                    hole = j;
                }

                j = probe_next(j, mask);
            }

            map->entries[hole].key = 0;
            map->entries[hole].value = NULL;
            map->entries[hole].hash = 0;

            map->size--;
            return true;
        }

        idx = probe_next(idx, mask);
    }

    return false;
}

size_t hash_map_size(const hash_map_t* map) { return map ? map->size : 0; }
