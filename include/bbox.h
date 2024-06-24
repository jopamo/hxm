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