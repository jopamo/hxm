/*
 * handle_conv.h - Lossless round-trip conversions between handle_t and void*
 *
 * Use case:
 * - Some APIs only accept a void* user pointer (callbacks, generic containers)
 * - You can stash a handle_t inside that void* and recover it later
 *
 * Requirements:
 * - handle_t must be an integer type (not a struct)
 * - sizeof(handle_t) <= sizeof(uintptr_t)
 * - The resulting pointer must never be dereferenced
 *
 * Portability notes:
 * - Converting integer <-> pointer is implementation-defined in C
 * - This header is intended for environments where uintptr_t round-tripping is
 *   supported (typical on modern hosted platforms)
 */

#ifndef HANDLE_CONV_H
#define HANDLE_CONV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "handle.h"

#if !defined(UINTPTR_MAX)
#error "uintptr_t is required"
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(handle_t) <= sizeof(uintptr_t), "handle_t must fit within uintptr_t for pointer round-trip");
#endif

static inline void* handle_to_ptr(handle_t h) { return (void*)(uintptr_t)h; }

static inline handle_t ptr_to_handle(const void* p) { return (handle_t)(uintptr_t)p; }

/* Convenience helpers that map invalid <-> NULL
 * If HANDLE_INVALID is not 0, these still provide a stable mapping
 */
static inline void* handle_to_ptr_nullable(handle_t h) {
    if (h == HANDLE_INVALID) return NULL;
    return (void*)(uintptr_t)h;
}

static inline handle_t ptr_to_handle_nullable(const void* p) {
    if (!p) return HANDLE_INVALID;
    return (handle_t)(uintptr_t)p;
}

#ifdef __cplusplus
}
#endif

#endif /* HANDLE_CONV_H */
