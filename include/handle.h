#ifndef HANDLE_H
#define HANDLE_H

#include <stdint.h>

// Handle-based identity to avoid pointer-stability issues and ABA problems.
// Packed into 64 bits for easy storage in hash maps and lists.
typedef uint64_t handle_t;

#define HANDLE_INVALID 0

static inline handle_t handle_make(uint32_t index, uint32_t generation) { return ((uint64_t)generation << 32) | index; }

static inline uint32_t handle_index(handle_t h) { return (uint32_t)(h & 0xFFFFFFFF); }

static inline uint32_t handle_generation(handle_t h) { return (uint32_t)(h >> 32); }

#endif  // HANDLE_H
