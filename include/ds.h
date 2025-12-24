#ifndef CONTAINERS_H
#define CONTAINERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xcb/xcb.h>

// Arena allocator for per-tick temporary allocations
struct arena_block {
    struct arena_block* next;
    size_t size;
    size_t used;
    char data[];
};

typedef struct arena_block arena_block_t;

struct arena {
    arena_block_t* first;
    arena_block_t* current;
    size_t pos;
    size_t block_size;
};

void arena_init(struct arena* a, size_t block_size);
void* arena_alloc(struct arena* a, size_t size);
char* arena_strndup(struct arena* a, const char* s, size_t n);
char* arena_strdup(struct arena* a, const char* s);
void arena_reset(struct arena* a);
void arena_destroy(struct arena* a);

// Small vector with inline storage
#define SMALL_VEC_INLINE_CAP 8

typedef struct small_vec {
    void** items;
    size_t length;
    size_t capacity;
    void* inline_storage[SMALL_VEC_INLINE_CAP];
} small_vec_t;

void small_vec_init(small_vec_t* v);
void small_vec_push(small_vec_t* v, void* item);
void* small_vec_pop(small_vec_t* v);
void* small_vec_get(const small_vec_t* v, size_t idx);
void small_vec_clear(small_vec_t* v);
void small_vec_destroy(small_vec_t* v);
void small_vec_remove_swap(small_vec_t* v, void* item);

// Hash map: uint64_t -> void*
// INVARIANT: key=0 is reserved to indicate an empty slot and MUST NOT be used as a valid key.
typedef struct hash_map_entry {
    uint64_t key;
    void* value;
    uint32_t hash;
} hash_map_entry_t;

typedef struct hash_map {
    hash_map_entry_t* entries;
    size_t capacity;
    size_t size;
    size_t max_load;
} hash_map_t;

void hash_map_init(hash_map_t* map);
void hash_map_destroy(hash_map_t* map);
bool hash_map_insert(hash_map_t* map, uint64_t key, void* value);
void* hash_map_get(const hash_map_t* map, uint64_t key);
bool hash_map_remove(hash_map_t* map, uint64_t key);
size_t hash_map_size(const hash_map_t* map);

#endif  // CONTAINERS_H