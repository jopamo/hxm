#ifndef BBOX_H
#define BBOX_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xcb/xcb.h>

// Global signals
extern volatile sig_atomic_t g_reload_pending;
extern volatile sig_atomic_t g_shutdown_pending;
extern volatile sig_atomic_t g_restart_pending;

// Core invariants
// - Single owner of truth: in-memory model is authoritative; X is just I/O
// - No synchronous queries in hot paths
// - Bounded tick: each loop iteration processes at most X events + queued actions
// - Coalesce by default
// - Batch X requests

// Platform primitives
// Intrusive doubly-linked list
typedef struct list_node {
    struct list_node* prev;
    struct list_node* next;
} list_node_t;

#define LIST_INIT(head) {&(head), &(head)}
#define LIST_HEAD(name) list_node_t name = LIST_INIT(name)

static inline void list_init(list_node_t* head) {
    head->prev = head;
    head->next = head;
}

static inline void list_insert(list_node_t* node, list_node_t* prev, list_node_t* next) {
    node->prev = prev;
    node->next = next;
    prev->next = node;
    next->prev = node;
}

static inline void list_remove(list_node_t* node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node;
    node->next = node;
}

static inline bool list_empty(list_node_t* head) { return head->next == head; }

// Hash map (xid -> pointer) placeholder
typedef struct hash_map hash_map_t;

// Arena allocator for per-tick temps
typedef struct arena arena_t;

// Small-vector for batched requests
typedef struct small_vec small_vec_t;

// Dirty rectangle region utils
typedef struct dirty_region {
    int16_t x, y;
    uint16_t w, h;
    bool valid;
} dirty_region_t;

static inline void dirty_region_reset(dirty_region_t* r) {
    r->x = 0;
    r->y = 0;
    r->w = 0;
    r->h = 0;
    r->valid = false;
}

static inline dirty_region_t dirty_region_make(int16_t x, int16_t y, uint16_t w, uint16_t h) {
    dirty_region_t r = {x, y, w, h, (w > 0 && h > 0)};
    return r;
}

static inline void dirty_region_union(dirty_region_t* dst, const dirty_region_t* src) {
    if (!src || !src->valid) return;
    if (!dst->valid) {
        *dst = *src;
        return;
    }

    int32_t x1 = dst->x;
    int32_t y1 = dst->y;
    int32_t x2 = dst->x + (int32_t)dst->w;
    int32_t y2 = dst->y + (int32_t)dst->h;

    int32_t sx1 = src->x;
    int32_t sy1 = src->y;
    int32_t sx2 = src->x + (int32_t)src->w;
    int32_t sy2 = src->y + (int32_t)src->h;

    int32_t nx1 = (x1 < sx1) ? x1 : sx1;
    int32_t ny1 = (y1 < sy1) ? y1 : sy1;
    int32_t nx2 = (x2 > sx2) ? x2 : sx2;
    int32_t ny2 = (y2 > sy2) ? y2 : sy2;

    int32_t nw = nx2 - nx1;
    int32_t nh = ny2 - ny1;
    if (nw <= 0 || nh <= 0) {
        dirty_region_reset(dst);
        return;
    }

    dst->x = (int16_t)nx1;
    dst->y = (int16_t)ny1;
    dst->w = (uint16_t)nw;
    dst->h = (uint16_t)nh;
    dst->valid = true;
}

static inline void dirty_region_union_rect(dirty_region_t* dst, int16_t x, int16_t y, uint16_t w, uint16_t h) {
    dirty_region_t src = dirty_region_make(x, y, w, h);
    dirty_region_union(dst, &src);
}

static inline void dirty_region_clamp(dirty_region_t* r, int16_t bx, int16_t by, uint16_t bw, uint16_t bh) {
    if (!r->valid) return;

    int32_t x1 = r->x;
    int32_t y1 = r->y;
    int32_t x2 = r->x + (int32_t)r->w;
    int32_t y2 = r->y + (int32_t)r->h;

    int32_t bx1 = bx;
    int32_t by1 = by;
    int32_t bx2 = bx + (int32_t)bw;
    int32_t by2 = by + (int32_t)bh;

    int32_t nx1 = (x1 > bx1) ? x1 : bx1;
    int32_t ny1 = (y1 > by1) ? y1 : by1;
    int32_t nx2 = (x2 < bx2) ? x2 : bx2;
    int32_t ny2 = (y2 < by2) ? y2 : by2;

    int32_t nw = nx2 - nx1;
    int32_t nh = ny2 - ny1;
    if (nw <= 0 || nh <= 0) {
        dirty_region_reset(r);
        return;
    }

    r->x = (int16_t)nx1;
    r->y = (int16_t)ny1;
    r->w = (uint16_t)nw;
    r->h = (uint16_t)nh;
    r->valid = true;
}

// Monotonic clock helper
uint64_t monotonic_time_ns(void);

// Logging
enum log_level {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

void bbox_log(enum log_level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(...) bbox_log(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) bbox_log(LOG_INFO, __VA_ARGS__)
#define LOG_WARN(...) bbox_log(LOG_WARN, __VA_ARGS__)
#define LOG_ERROR(...) bbox_log(LOG_ERROR, __VA_ARGS__)

#ifdef BBOX_DEBUG_TRACE
#define TRACE_LOG(...) LOG_DEBUG(__VA_ARGS__)
#define TRACE_WARN(...) LOG_WARN(__VA_ARGS__)
#define TRACE_ONLY(...) \
    do {                \
        __VA_ARGS__;    \
    } while (0)
#else
#define TRACE_LOG(...) \
    do {               \
    } while (0)
#define TRACE_WARN(...) \
    do {                \
    } while (0)
#define TRACE_ONLY(...) \
    do {                \
    } while (0)
#endif

// Perf counters
struct counters {
    uint64_t events_seen[256];  // X event type index
    uint64_t coalesced_drops[256];
    uint64_t config_requests_applied;
    uint64_t restacks_applied;
    uint64_t tick_duration_min;
    uint64_t tick_duration_sum;
    uint64_t tick_duration_max;
    uint64_t tick_count;
    uint64_t x_flush_count;
};

extern struct counters counters;

void counters_init(void);
void counters_dump(void);

#endif  // BBOX_H
