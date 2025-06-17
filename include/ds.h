/*
 * ds.h - Data structures
 *
 * Primitives used throughout the WM:
 * - arena: fast, resettable allocator for per-tick temporaries
 * - small_vec: pointer vector with small inline storage
 * - hash_map: uint64_t -> void* open-addressing hash map (key 0 reserved)
 *
 * Design goals:
 * - predictable performance
 * - minimal dependencies
 * - safe-by-default APIs (clear ownership + invariants)
 *
 * Notes:
 * - These are not thread-safe
 * - arena allocations are freed in bulk via arena_reset/arena_destroy
 * - hash_map uses key=0 as empty tombstone sentinel, so 0 is forbidden as a key
 */

#ifndef CONTAINERS_H
#define CONTAINERS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------- Arena ---------------- */

typedef struct arena_block {
    struct arena_block* next;
    size_t size;
    size_t used;
    unsigned char data[];
} arena_block_t;

typedef struct arena {
    arena_block_t* first;
    arena_block_t* current;
    size_t pos;

    /* Default block size for new allocations */
    size_t block_size;
} arena_t;

/* Initialize arena with a block size hint (0 => sensible default) */
void arena_init(arena_t* a, size_t block_size);

/* Allocate size bytes with suitable alignment for max_align_t
 * Returns NULL on allocation failure
 */
void* arena_alloc(arena_t* a, size_t size);

/* Allocate and copy strings into the arena (NUL-terminated) */
char* arena_strndup(arena_t* a, const char* s, size_t n);
char* arena_strdup(arena_t* a, const char* s);

/* Reset arena for reuse (keeps first block for amortization) */
void arena_reset(arena_t* a);

/* Free all memory associated with the arena */
void arena_destroy(arena_t* a);

/* ---------------- Small vector ---------------- */

#ifndef SMALL_VEC_INLINE_CAP
#define SMALL_VEC_INLINE_CAP 8
#endif

typedef struct small_vec {
    void** items;
    size_t length;
    size_t capacity;
    void* inline_storage[SMALL_VEC_INLINE_CAP];
} small_vec_t;

/* Basic operations */
void small_vec_init(small_vec_t* v);
void small_vec_destroy(small_vec_t* v);

void small_vec_clear(small_vec_t* v);

void small_vec_push(small_vec_t* v, void* item);
void* small_vec_pop(small_vec_t* v);

void* small_vec_get(const small_vec_t* v, size_t idx);

/* Remove an item by pointer identity (swap with last)
 * If item is not found, does nothing
 */
void small_vec_remove_swap(small_vec_t* v, void* item);

/* Convenience helpers */
static inline size_t small_vec_len(const small_vec_t* v) { return v ? v->length : 0u; }
static inline bool small_vec_empty(const small_vec_t* v) { return !v || v->length == 0u; }

/* ---------------- Hash map ----------------
 *
 * Open addressing with linear probing
 * Key type: uint64_t
 * Value type: void*
 *
 * Invariants:
 * - key=0 is reserved as an empty marker and must not be inserted
 *
 * Notes:
 * - This API does not take ownership of values
 */

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

/* Initialize empty map */
void hash_map_init(hash_map_t* map);

/* Free internal storage */
void hash_map_destroy(hash_map_t* map);

/* Insert or replace (returns true on success)
 * If key already exists, its value is replaced
 */
bool hash_map_insert(hash_map_t* map, uint64_t key, void* value);

/* Get value for key or NULL */
void* hash_map_get(const hash_map_t* map, uint64_t key);

/* Remove key if present (returns true if removed) */
bool hash_map_remove(hash_map_t* map, uint64_t key);

static inline size_t hash_map_size(const hash_map_t* map) { return map ? map->size : 0u; }
static inline bool hash_map_empty(const hash_map_t* map) { return !map || map->size == 0u; }

/* expose capacity for diagnostics */
static inline size_t hash_map_capacity(const hash_map_t* map) { return map ? map->capacity : 0u; }

#ifdef __cplusplus
}
#endif

#endif /* CONTAINERS_H */
