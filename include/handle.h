/*
 * handle.h - Packed generational handle
 *
 * A handle is a stable identifier: {index, generation} packed into 64 bits
 * Used to avoid pointer-stability problems and ABA-style bugs when indices are
 * reused
 *
 * Layout (little/big endian irrelevant because it's a value):
 *   bits  0..31  : index
 *   bits 32..63  : generation
 *
 * Conventions:
 * - HANDLE_INVALID is 0 (index=0, generation=0)
 * - index 0 is reserved as invalid
 * - generation 0 is reserved as invalid (slotmaps should start at 1 and never
 * return 0)
 */

#ifndef HANDLE_H
#define HANDLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef uint64_t handle_t;

#define HANDLE_INVALID ((handle_t)0u)

/* Prefer these constants over magic masks in call sites */
#define HANDLE_INDEX_BITS 32u
#define HANDLE_INDEX_MASK ((uint32_t)0xFFFFFFFFu)

/* Constructors / accessors */
static inline handle_t handle_make(uint32_t index, uint32_t generation) {
  return (((uint64_t)generation) << 32) | (uint64_t)index;
}

static inline uint32_t handle_index(handle_t h) {
  return (uint32_t)(h & (uint64_t)HANDLE_INDEX_MASK);
}

static inline uint32_t handle_generation(handle_t h) {
  return (uint32_t)(h >> 32);
}

/* Utilities */
static inline int handle_is_valid(handle_t h) {
  return h != HANDLE_INVALID;
}

static inline int handle_eq(handle_t a, handle_t b) {
  return a == b;
}

/* build-time sanity checks (C11+) */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(handle_t) == 8, "handle_t must be 64-bit");
_Static_assert(((handle_t)1u << 32) != 0, "64-bit shift must be supported");
#endif

#ifdef __cplusplus
}
#endif

#endif /* HANDLE_H */
