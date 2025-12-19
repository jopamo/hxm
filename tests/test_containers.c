#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "containers.h"

void test_arena() {
    struct arena a;
    arena_init(&a, 1024);

    void* p1 = arena_alloc(&a, 128);
    assert(p1 != NULL);
    void* p2 = arena_alloc(&a, 512);
    assert(p2 != NULL);
    assert((char*)p2 >= (char*)p1 + 128);

    arena_reset(&a);
    void* p3 = arena_alloc(&a, 128);
    // After reset, we might get the same pointer or a different one depending on implementation,
    // but it must be valid.
    assert(p3 != NULL);

    // Test multiple blocks
    arena_init(&a, 64);
    void* p4 = arena_alloc(&a, 40);
    void* p5 = arena_alloc(&a, 40);  // Should trigger new block
    assert(p4 != NULL);
    assert(p5 != NULL);
    assert(p4 != p5);

    arena_destroy(&a);
    printf("test_arena passed\n");
}

void test_hash_map() {
    hash_map_t map;
    hash_map_init(&map);

    assert(hash_map_get(&map, 1) == NULL);
    assert(hash_map_remove(&map, 1) == false);

    hash_map_insert(&map, 1, (void*)0x11);
    hash_map_insert(&map, 2, (void*)0x22);
    hash_map_insert(&map, 100, (void*)0x100);

    assert(hash_map_get(&map, 1) == (void*)0x11);
    assert(hash_map_get(&map, 2) == (void*)0x22);
    assert(hash_map_get(&map, 100) == (void*)0x100);
    assert(hash_map_size(&map) == 3);

    assert(hash_map_remove(&map, 2) == true);
    assert(hash_map_get(&map, 2) == NULL);
    assert(hash_map_size(&map) == 2);
    assert(hash_map_get(&map, 1) == (void*)0x11);
    assert(hash_map_get(&map, 100) == (void*)0x100);

    // Test collision handling (assuming simple hash)
    // We can't easily force collision without knowing hash,
    // but we can insert many items.
    for (uint64_t i = 1000; i < 2000; i++) {
        hash_map_insert(&map, i, (void*)i);
    }
    assert(hash_map_size(&map) == 1002);

    for (uint64_t i = 1000; i < 2000; i++) {
        assert(hash_map_get(&map, i) == (void*)i);
    }

    // Remove half
    for (uint64_t i = 1000; i < 1500; i++) {
        assert(hash_map_remove(&map, i) == true);
    }
    assert(hash_map_size(&map) == 502);

    for (uint64_t i = 1000; i < 1500; i++) {
        assert(hash_map_get(&map, i) == NULL);
    }
    for (uint64_t i = 1500; i < 2000; i++) {
        assert(hash_map_get(&map, i) == (void*)i);
    }

    hash_map_destroy(&map);
    printf("test_hash_map passed\n");
}

void test_arena_reuse() {
    struct arena a;
    arena_init(&a, 100);

    // Initial allocation
    void* p1 = arena_alloc(&a, 60);
    void* p2 = arena_alloc(&a, 60);  // Should trigger new block
    assert(p1 != p2);

    // Record the blocks
    arena_block_t* first_block = a.first;
    arena_block_t* second_block = a.first->next;
    assert(first_block != NULL);
    assert(second_block != NULL);
    assert(a.current == second_block);

    arena_reset(&a);
    assert(a.current == first_block);

    // After reset, the first alloc should use the first block again.
    void* p3 = arena_alloc(&a, 60);
    assert(p3 == first_block->data);

    // The second alloc should use the second block we already have.
    void* p4 = arena_alloc(&a, 60);
    assert(p4 == second_block->data);
    assert(a.current == second_block);
    assert(second_block->next == NULL);  // No third block should have been allocated

    arena_destroy(&a);
    printf("test_arena_reuse passed (blocks reused correctly)\n");
}

void test_hash_map_collision_removal() {
    hash_map_t map;
    hash_map_init(&map);

    // We need to force collisions.
    // Our hash function is a mixer, but with power-of-2 capacity,
    // we just need items whose hashes have the same lower bits.
    // Capacity starts at 16 if we insert enough.

    for (int i = 1; i <= 20; i++) {
        hash_map_insert(&map, i, (void*)(uintptr_t)(i + 1));
    }
    // Capacity should now be 32 (next power of 2 after 16*2).
    // max_load = 32 * 3/4 = 24. 20 items fits in 32.

    // Let's find some keys that collide.
    // Since it's linear probing, any keys that hash to the same area will collide.

    // Check all keys are there
    for (int i = 1; i <= 20; i++) {
        assert(hash_map_get(&map, i) == (void*)(uintptr_t)(i + 1));
    }

    // Remove an item that is likely in the middle of a probe chain.
    hash_map_remove(&map, 10);
    assert(hash_map_get(&map, 10) == NULL);

    // Verify all others are still reachable
    for (int i = 1; i <= 20; i++) {
        if (i == 10) continue;
        assert(hash_map_get(&map, i) == (void*)(uintptr_t)(i + 1));
    }

    hash_map_destroy(&map);
    printf("test_hash_map_collision_removal passed\n");
}

int main() {
    test_arena();
    test_arena_reuse();
    test_hash_map();
    test_hash_map_collision_removal();
    return 0;
}
