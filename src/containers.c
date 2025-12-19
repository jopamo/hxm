#include "containers.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "bbox.h"

// Arena allocator
void arena_init(struct arena* a, size_t block_size) {
    a->first = NULL;
    a->current = NULL;
    a->pos = 0;
    a->block_size = block_size;
}

static arena_block_t* arena_add_block(struct arena* a) {
    size_t size = sizeof(arena_block_t) + a->block_size;
    arena_block_t* block = malloc(size);
    if (!block) {
        LOG_ERROR("arena allocation failed");
        abort();
    }
    block->next = NULL;
    block->size = a->block_size;
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
    // Align to 8 bytes
    size = (size + 7) & ~7;

    if (a->current == NULL) {
        if (a->first) {
            a->current = a->first;
            a->pos = 0;
        } else {
            arena_add_block(a);
        }
    }

    if (a->pos + size > a->block_size) {
        if (a->current->next) {
            a->current = a->current->next;
            a->pos = 0;
        } else {
            arena_add_block(a);
        }
    }

    void* ptr = a->current->data + a->pos;
    a->pos += size;
    a->current->used = a->pos;
    return ptr;
}

char* arena_strndup(struct arena* a, const char* s, size_t n) {
    char* res = arena_alloc(a, n + 1);
    memcpy(res, s, n);
    res[n] = '\0';
    return res;
}

void arena_reset(struct arena* a) {
    a->current = a->first;
    a->pos = 0;
    // We don't strictly need to zero out 'used' in all blocks here,
    // as arena_alloc will reset it when it moves to a block.
    // But for sanity, let's keep it consistent.
    arena_block_t* b = a->first;
    while (b) {
        b->used = 0;
        b = b->next;
    }
}

void arena_destroy(struct arena* a) {
    arena_block_t* block = a->first;
    while (block) {
        arena_block_t* next = block->next;
        free(block);
        block = next;
    }
    a->first = NULL;
    a->current = NULL;
    a->pos = 0;
}

// Small vector
void small_vec_init(small_vec_t* v) {
    v->items = v->inline_storage;
    v->length = 0;
    v->capacity = SMALL_VEC_INLINE_CAP;
}

void small_vec_push(small_vec_t* v, void* item) {
    if (v->length == v->capacity) {
        size_t new_cap = v->capacity * 2;
        void** new_items;
        if (v->items == v->inline_storage) {
            new_items = malloc(new_cap * sizeof(void*));
            if (new_items) {
                memcpy(new_items, v->inline_storage, v->length * sizeof(void*));
            }
        } else {
            new_items = realloc(v->items, new_cap * sizeof(void*));
        }
        if (!new_items) {
            LOG_ERROR("small_vec allocation failed");
            abort();
        }
        v->items = new_items;
        v->capacity = new_cap;
    }
    v->items[v->length++] = item;
}

void* small_vec_pop(small_vec_t* v) {
    if (v->length == 0) return NULL;
    return v->items[--v->length];
}

void small_vec_clear(small_vec_t* v) { v->length = 0; }

void small_vec_destroy(small_vec_t* v) {
    if (v->items != v->inline_storage) {
        free(v->items);
    }
    v->items = v->inline_storage;
    v->length = 0;
    v->capacity = SMALL_VEC_INLINE_CAP;
}

// Hash map utilities
static inline uint32_t hash_key(uint64_t key) {
    // Simple mixing (MurmurHash3 mixer for 64-bit)
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return (uint32_t)key;
}

static void hash_map_resize(hash_map_t* map, size_t new_capacity) {
    hash_map_entry_t* new_entries = calloc(new_capacity, sizeof(hash_map_entry_t));
    if (!new_entries) {
        LOG_ERROR("hash_map allocation failed");
        abort();
    }

    for (size_t i = 0; i < map->capacity; i++) {
        hash_map_entry_t* entry = &map->entries[i];
        if (entry->key != 0) {  // 0 is reserved as empty/XCB_NONE replacement
            size_t idx = entry->hash & (new_capacity - 1);
            while (new_entries[idx].key != 0) {
                idx = (idx + 1) & (new_capacity - 1);
            }
            new_entries[idx] = *entry;
        }
    }

    free(map->entries);
    map->entries = new_entries;
    map->capacity = new_capacity;
    map->max_load = new_capacity * 3 / 4;
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
    if (map->size >= map->max_load) {
        size_t new_cap = map->capacity ? map->capacity * 2 : 16;
        hash_map_resize(map, new_cap);
    }

    uint32_t hash = hash_key(key);
    size_t idx = hash & (map->capacity - 1);
    while (map->entries[idx].key != 0) {
        if (map->entries[idx].key == key) {
            map->entries[idx].value = value;
            return true;
        }
        idx = (idx + 1) & (map->capacity - 1);
    }

    map->entries[idx].key = key;
    map->entries[idx].value = value;
    map->entries[idx].hash = hash;
    map->size++;
    return false;
}

void* hash_map_get(const hash_map_t* map, uint64_t key) {
    assert(key != 0 && "key=0 is reserved for hash_map_t");
    if (map->capacity == 0) return NULL;
    uint32_t hash = hash_key(key);
    size_t idx = hash & (map->capacity - 1);
    while (map->entries[idx].key != 0) {
        if (map->entries[idx].key == key) {
            return map->entries[idx].value;
        }
        idx = (idx + 1) & (map->capacity - 1);
    }
    return NULL;
}

static inline size_t probe_next(size_t idx, size_t mask) { return (idx + 1) & mask; }

bool hash_map_remove(hash_map_t* map, uint64_t key) {
    assert(key != 0 && "key=0 is reserved for hash_map_t");
    if (map->capacity == 0) return false;

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

size_t hash_map_size(const hash_map_t* map) { return map->size; }