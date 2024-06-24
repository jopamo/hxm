#ifndef HANDLE_CONV_H
#define HANDLE_CONV_H

#include <stdint.h>

#include "handle.h"

static inline void* handle_to_ptr(handle_t h) { return (void*)(uintptr_t)h; }

static inline handle_t ptr_to_handle(void* p) { return (handle_t)(uintptr_t)p; }

#endif  // HANDLE_CONV_H