#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ds.h"

// Mock allocation failure
static bool g_fail_alloc = false;

void* ds_malloc(size_t size) {
  if (g_fail_alloc)
    return NULL;
  return malloc(size);
}

void* ds_realloc(void* ptr, size_t size) {
  if (g_fail_alloc)
    return NULL;
  return realloc(ptr, size);
}

void* ds_calloc(size_t nmemb, size_t size) {
  if (g_fail_alloc)
    return NULL;
  return calloc(nmemb, size);
}

#define TEST_ASSERT(cond)                                                       \
  do {                                                                          \
    if (!(cond)) {                                                              \
      fprintf(stderr, "ASSERT FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      abort();                                                                  \
    }                                                                           \
  } while (0)

static uintptr_t ptr_u(const void* p) {
  return (uintptr_t)p;
}

static void fill_pattern(void* p, size_t n, uint8_t pat) {
  if (!p || n == 0)
    return;
  memset(p, pat, n);
}

static void expect_pattern(const void* p, size_t n, uint8_t pat) {
  if (!p || n == 0)
    return;
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) {
    if (b[i] != pat) {
      fprintf(stderr, "pattern mismatch at +%zu: got 0x%02x expected 0x%02x\n", i, b[i], pat);
      abort();
    }
  }
}

static uint64_t prng_u64(uint64_t* state) {
  // xorshift64*
  uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 2685821657736338717ull;
}

static void test_arena_basic(void) {
  struct arena a;
  arena_init(&a, 1024);

  void* p1 = arena_alloc(&a, 128);
  TEST_ASSERT(p1 != NULL);
  void* p2 = arena_alloc(&a, 512);
  TEST_ASSERT(p2 != NULL);
  TEST_ASSERT((char*)p2 >= (char*)p1 + 128);

  arena_reset(&a);
  void* p3 = arena_alloc(&a, 128);
  TEST_ASSERT(p3 != NULL);

  arena_destroy(&a);

  // multiple blocks
  arena_init(&a, 64);
  void* p4 = arena_alloc(&a, 40);
  void* p5 = arena_alloc(&a, 40);
  TEST_ASSERT(p4 != NULL);
  TEST_ASSERT(p5 != NULL);
  TEST_ASSERT(p4 != p5);

  arena_destroy(&a);
  printf("test_arena_basic passed\n");
}

static void test_arena_reuse_blocks(void) {
  struct arena a;
  arena_init(&a, 100);

  void* p1 = arena_alloc(&a, 60);
  void* p2 = arena_alloc(&a, 60);
  TEST_ASSERT(p1 && p2);
  TEST_ASSERT(p1 != p2);

  arena_block_t* first_block = a.first;
  arena_block_t* second_block = a.first->next;
  TEST_ASSERT(first_block != NULL);
  TEST_ASSERT(second_block != NULL);
  TEST_ASSERT(a.current == second_block);

  arena_reset(&a);
  TEST_ASSERT(a.current == first_block);

  void* p3 = arena_alloc(&a, 60);
  TEST_ASSERT(p3 == first_block->data);

  void* p4 = arena_alloc(&a, 60);
  TEST_ASSERT(p4 == second_block->data);
  TEST_ASSERT(a.current == second_block);
  TEST_ASSERT(second_block->next == NULL);

  arena_destroy(&a);
  printf("test_arena_reuse_blocks passed\n");
}

static void test_arena_alignment_and_overlap(void) {
  struct arena a;
  arena_init(&a, 256);

  // allocate a bunch of small chunks and ensure monotonic + non-overlap
  const size_t n = 32;
  void* ptrs[n];
  size_t sizes[n];

  for (size_t i = 0; i < n; i++) {
    sizes[i] = (i % 7) + 1;  // 1..7 bytes
    ptrs[i] = arena_alloc(&a, sizes[i]);
    TEST_ASSERT(ptrs[i] != NULL);

    // "default alignment" check: most arenas at least align to pointer size
    TEST_ASSERT((ptr_u(ptrs[i]) % sizeof(void*)) == 0);

    fill_pattern(ptrs[i], sizes[i], (uint8_t)(0xA0 + i));
  }

  // verify patterns and no overlap corruption
  for (size_t i = 0; i < n; i++) {
    expect_pattern(ptrs[i], sizes[i], (uint8_t)(0xA0 + i));
  }

  arena_destroy(&a);
  printf("test_arena_alignment_and_overlap passed\n");
}

static void test_arena_zero_and_large_alloc(void) {
  struct arena a;
  arena_init(&a, 128);

  // zero-size alloc should not crash
  void* z1 = arena_alloc(&a, 0);
  void* z2 = arena_alloc(&a, 0);
  TEST_ASSERT(z1 != NULL);
  TEST_ASSERT(z2 != NULL);

  // large alloc bigger than block size
  // If your arena supports a dedicated oversized block, this should succeed
  // If not, you might choose to return NULL; accept either but require
  // consistency
  void* big = arena_alloc(&a, 4096);
  if (big) {
    fill_pattern(big, 64, 0x5A);
    expect_pattern(big, 64, 0x5A);
  }

  arena_destroy(&a);
  printf("test_arena_zero_and_large_alloc passed\n");
}

static void test_arena_reset_semantics(void) {
  struct arena a;
  arena_init(&a, 64);

  // force multiple blocks, write sentinels in each allocation
  void* saved[16];
  for (int i = 0; i < 16; i++) {
    saved[i] = arena_alloc(&a, 40);
    TEST_ASSERT(saved[i] != NULL);
    ((uint32_t*)saved[i])[0] = (uint32_t)(0xC0FFEE00u + (uint32_t)i);
  }

  arena_block_t* first = a.first;
  TEST_ASSERT(first != NULL);
  TEST_ASSERT(first->next != NULL);

  arena_reset(&a);
  TEST_ASSERT(a.current == first);

  // after reset, first alloc should come from first block start
  void* p = arena_alloc(&a, 16);
  TEST_ASSERT(p == first->data);

  arena_destroy(&a);
  printf("test_arena_reset_semantics passed\n");
}

static void test_hash_map_basic(void) {
  hash_map_t map;
  hash_map_init(&map);

  TEST_ASSERT(hash_map_get(&map, 1) == NULL);
  TEST_ASSERT(hash_map_remove(&map, 1) == false);

  hash_map_insert(&map, 1, (void*)0x11);
  hash_map_insert(&map, 2, (void*)0x22);
  hash_map_insert(&map, 100, (void*)0x100);

  TEST_ASSERT(hash_map_get(&map, 1) == (void*)0x11);
  TEST_ASSERT(hash_map_get(&map, 2) == (void*)0x22);
  TEST_ASSERT(hash_map_get(&map, 100) == (void*)0x100);
  TEST_ASSERT(hash_map_size(&map) == 3);

  TEST_ASSERT(hash_map_remove(&map, 2) == true);
  TEST_ASSERT(hash_map_get(&map, 2) == NULL);
  TEST_ASSERT(hash_map_size(&map) == 2);
  TEST_ASSERT(hash_map_get(&map, 1) == (void*)0x11);
  TEST_ASSERT(hash_map_get(&map, 100) == (void*)0x100);

  hash_map_destroy(&map);
  printf("test_hash_map_basic passed\n");
}

static void test_hash_map_update_and_reinsert(void) {
  hash_map_t map;
  hash_map_init(&map);

  hash_map_insert(&map, 7, (void*)0x77);
  TEST_ASSERT(hash_map_size(&map) == 1);
  TEST_ASSERT(hash_map_get(&map, 7) == (void*)0x77);

  // update existing key should not increase size
  hash_map_insert(&map, 7, (void*)0x99);
  TEST_ASSERT(hash_map_size(&map) == 1);
  TEST_ASSERT(hash_map_get(&map, 7) == (void*)0x99);

  // remove then reinsert
  TEST_ASSERT(hash_map_remove(&map, 7) == true);
  TEST_ASSERT(hash_map_get(&map, 7) == NULL);
  TEST_ASSERT(hash_map_size(&map) == 0);

  hash_map_insert(&map, 7, (void*)0xAB);
  TEST_ASSERT(hash_map_get(&map, 7) == (void*)0xAB);
  TEST_ASSERT(hash_map_size(&map) == 1);

  // remove missing key should be false
  TEST_ASSERT(hash_map_remove(&map, 123456) == false);
  TEST_ASSERT(hash_map_size(&map) == 1);

  hash_map_destroy(&map);
  printf("test_hash_map_update_and_reinsert passed\n");
}

static void test_hash_map_stress_linear_probe_tombstones(void) {
  hash_map_t map;
  hash_map_init(&map);

  // Create a lot of entries, then delete a lot, then reinsert
  // This catches the classic "remove breaks probe chain" and tombstone bugs
  const uint64_t base = 1000;
  const uint64_t n = 4000;

  for (uint64_t i = 0; i < n; i++) {
    uint64_t k = base + i;
    hash_map_insert(&map, k, (void*)(uintptr_t)(k ^ 0xDEADBEEF));
  }
  TEST_ASSERT(hash_map_size(&map) == n);

  // remove evens
  for (uint64_t i = 0; i < n; i += 2) {
    uint64_t k = base + i;
    TEST_ASSERT(hash_map_remove(&map, k) == true);
  }
  TEST_ASSERT(hash_map_size(&map) == (n / 2));

  // odds still readable
  for (uint64_t i = 1; i < n; i += 2) {
    uint64_t k = base + i;
    TEST_ASSERT(hash_map_get(&map, k) == (void*)(uintptr_t)(k ^ 0xDEADBEEF));
  }

  // reinsert evens with new values, should work even with tombstones
  for (uint64_t i = 0; i < n; i += 2) {
    uint64_t k = base + i;
    hash_map_insert(&map, k, (void*)(uintptr_t)(k ^ 0xBADC0FFE));
  }
  TEST_ASSERT(hash_map_size(&map) == n);

  // verify all
  for (uint64_t i = 0; i < n; i++) {
    uint64_t k = base + i;
    void* want = (void*)(uintptr_t)((i % 2 == 0) ? (k ^ 0xBADC0FFE) : (k ^ 0xDEADBEEF));
    TEST_ASSERT(hash_map_get(&map, k) == want);
  }

  hash_map_destroy(&map);
  printf("test_hash_map_stress_linear_probe_tombstones passed\n");
}

static void test_hash_map_prng_sequence(void) {
  hash_map_t map;
  hash_map_init(&map);

  // model with a simple sorted array of keys/values
  // keep it small so we can verify semantics precisely
  enum { MAX = 2048 };
  uint64_t keys[MAX];
  void* vals[MAX];
  size_t used = 0;

  uint64_t rng = 0x123456789abcdef0ull;

  for (int step = 0; step < 20000; step++) {
    uint64_t r = prng_u64(&rng);
    uint64_t k = (r % 4096) + 1;
    int op = (int)((r >> 32) % 3);

    if (op == 0) {
      // insert/update
      void* v = (void*)(uintptr_t)(r | 1ull);
      hash_map_insert(&map, k, v);

      bool found = false;
      for (size_t i = 0; i < used; i++) {
        if (keys[i] == k) {
          vals[i] = v;
          found = true;
          break;
        }
      }
      if (!found) {
        if (used < MAX) {
          keys[used] = k;
          vals[used] = v;
          used++;
        }
      }
    }
    else if (op == 1) {
      // remove
      bool removed = hash_map_remove(&map, k);

      bool in_model = false;
      for (size_t i = 0; i < used; i++) {
        if (keys[i] == k) {
          // delete by swap-with-last
          keys[i] = keys[used - 1];
          vals[i] = vals[used - 1];
          used--;
          in_model = true;
          break;
        }
      }
      TEST_ASSERT(removed == in_model);
    }
    else {
      // get
      void* got = hash_map_get(&map, k);
      void* want = NULL;
      for (size_t i = 0; i < used; i++) {
        if (keys[i] == k) {
          want = vals[i];
          break;
        }
      }
      TEST_ASSERT(got == want);
    }
  }

  // final full verification
  for (uint64_t k = 1; k <= 4096; k++) {
    void* got = hash_map_get(&map, k);
    void* want = NULL;
    for (size_t i = 0; i < used; i++) {
      if (keys[i] == k) {
        want = vals[i];
        break;
      }
    }
    TEST_ASSERT(got == want);
  }

  hash_map_destroy(&map);
  printf("test_hash_map_prng_sequence passed\n");
}

static void test_arena_strings(void) {
  struct arena a;
  arena_init(&a, 1024);

  const char* s1 = "hello world";
  char* d1 = arena_strdup(&a, s1);
  TEST_ASSERT(d1 != NULL);
  TEST_ASSERT(strcmp(d1, s1) == 0);
  TEST_ASSERT((void*)d1 != (void*)s1);

  const char* s2 = "foobar";
  char* d2 = arena_strndup(&a, s2, 3);
  TEST_ASSERT(d2 != NULL);
  TEST_ASSERT(strcmp(d2, "foo") == 0);

  arena_destroy(&a);
  printf("test_arena_strings passed\n");
}

static void test_small_vec_basic(void) {
  small_vec_t v;
  small_vec_init(&v);

  TEST_ASSERT(v.length == 0);
  TEST_ASSERT(small_vec_pop(&v) == NULL);

  small_vec_push(&v, (void*)0x1);
  small_vec_push(&v, (void*)0x2);
  TEST_ASSERT(v.length == 2);
  TEST_ASSERT(small_vec_get(&v, 0) == (void*)0x1);
  TEST_ASSERT(small_vec_get(&v, 1) == (void*)0x2);
  TEST_ASSERT(small_vec_get(&v, 2) == NULL);

  TEST_ASSERT(small_vec_pop(&v) == (void*)0x2);
  TEST_ASSERT(v.length == 1);
  TEST_ASSERT(small_vec_pop(&v) == (void*)0x1);
  TEST_ASSERT(v.length == 0);

  small_vec_push(&v, (void*)0x3);
  small_vec_clear(&v);
  TEST_ASSERT(v.length == 0);

  small_vec_destroy(&v);
  printf("test_small_vec_basic passed\n");
}

static void test_small_vec_growth(void) {
  small_vec_t v;
  small_vec_init(&v);

  // Default inline cap is 8. Push 100 items to force heap alloc.
  for (size_t i = 0; i < 100; i++) {
    small_vec_push(&v, (void*)(uintptr_t)(i + 1));
  }

  TEST_ASSERT(v.length == 100);
  TEST_ASSERT(v.items != v.inline_storage);
  TEST_ASSERT(v.capacity >= 100);

  for (size_t i = 0; i < 100; i++) {
    TEST_ASSERT(small_vec_get(&v, i) == (void*)(uintptr_t)(i + 1));
  }

  // Pop everything back
  for (size_t i = 0; i < 100; i++) {
    void* p = small_vec_pop(&v);
    TEST_ASSERT(p == (void*)(uintptr_t)(100 - i));
  }
  TEST_ASSERT(v.length == 0);

  small_vec_destroy(&v);
  printf("test_small_vec_growth passed\n");
}

static void test_alloc_fail_arena(void) {
  printf("Running test_alloc_fail_arena...\n");
  pid_t pid = fork();
  if (pid == 0) {
    g_fail_alloc = true;
    freopen("/dev/null", "w", stderr);

    struct arena a;
    arena_init(&a, 1024);
    arena_alloc(&a, 128);  // Should abort
    exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
  // abort usually causes SIGABRT (signal 6)
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
    printf("test_alloc_fail_arena passed (SIGABRT)\n");
  }
  else {
    printf("FAIL: test_alloc_fail_arena - status %d\n", status);
    exit(1);
  }
}

static void test_alloc_fail_small_vec(void) {
  printf("Running test_alloc_fail_small_vec...\n");
  pid_t pid = fork();
  if (pid == 0) {
    freopen("/dev/null", "w", stderr);
    small_vec_t v;
    small_vec_init(&v);

    // Push until growth triggers malloc
    // Then fail realloc?
    // Let's test first malloc
    g_fail_alloc = true;

    // Inline cap 8. Push 9.
    for (int i = 0; i < 9; i++)
      small_vec_push(&v, (void*)1);

    exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
    printf("test_alloc_fail_small_vec passed (SIGABRT)\n");
  }
  else {
    printf("FAIL: test_alloc_fail_small_vec - status %d\n", status);
    exit(1);
  }
}

static void test_alloc_fail_hash_map(void) {
  printf("Running test_alloc_fail_hash_map...\n");
  pid_t pid = fork();
  if (pid == 0) {
    freopen("/dev/null", "w", stderr);
    g_fail_alloc = true;

    hash_map_t map;
    hash_map_init(&map);
    hash_map_insert(&map, 1, (void*)1);  // Should abort on resize

    exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
    printf("test_alloc_fail_hash_map passed (SIGABRT)\n");
  }
  else {
    printf("FAIL: test_alloc_fail_hash_map - status %d\n", status);
    exit(1);
  }
}

int main(void) {
  test_arena_basic();
  test_arena_reuse_blocks();
  test_arena_alignment_and_overlap();
  test_arena_zero_and_large_alloc();
  test_arena_reset_semantics();
  test_arena_strings();

  test_small_vec_basic();
  test_small_vec_growth();

  test_hash_map_basic();
  test_hash_map_update_and_reinsert();
  test_hash_map_stress_linear_probe_tombstones();
  test_hash_map_prng_sequence();

  test_alloc_fail_arena();
  test_alloc_fail_small_vec();
  test_alloc_fail_hash_map();

  return 0;
}
