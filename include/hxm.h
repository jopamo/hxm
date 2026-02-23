/*
 * hxm.h - Core utilities, logging, and platform primitives
 *
 * This header is the common foundation for the project
 *
 * Provides:
 * - Intrusive doubly-linked list primitives
 * - Dirty region logic (rect union, clamping)
 * - Logging macros with compile-time elimination
 * - Performance counters
 * - Simple rate limiter
 * - Global signal flags
 *
 * Contracts:
 * - Single owner of truth: in-memory model is authoritative; X is I/O
 * - No synchronous X queries in hot paths
 * - Bounded tick: each loop iteration processes at most a bounded amount of
 * work
 * - Coalesce by default
 * - Batch X requests
 */

#ifndef HXM_H
#define HXM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration only for users that want X event type values */
#include <xcb/xcb.h>

/* ---------- Build attributes and common macros ---------- */

#if defined(__GNUC__) || defined(__clang__)
#define HXM_ATTR_PRINTF(fmt_idx, va_idx) __attribute__((format(printf, fmt_idx, va_idx)))
#define HXM_LIKELY(x) __builtin_expect(!!(x), 1)
#define HXM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define HXM_ATTR_PRINTF(fmt_idx, va_idx)
#define HXM_LIKELY(x) (x)
#define HXM_UNLIKELY(x) (x)
#endif

#ifndef HXM_ARRAY_LEN
#define HXM_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef HXM_MIN
#define HXM_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef HXM_MAX
#define HXM_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef HXM_CLAMP
#define HXM_CLAMP(x, lo, hi) (HXM_MAX((lo), HXM_MIN((x), (hi))))
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define HXM_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#else
#define HXM_STATIC_ASSERT(cond, msg)
#endif

/* ---------- Global signal flags ---------- */

extern volatile sig_atomic_t g_reload_pending;
extern volatile sig_atomic_t g_shutdown_pending;
extern volatile sig_atomic_t g_restart_pending;

/* ---------- Intrusive doubly-linked list ---------- */

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

static inline bool list_empty(const list_node_t* head) {
  return head->next == head;
}

static inline void list_insert_between(list_node_t* node, list_node_t* prev, list_node_t* next) {
  node->prev = prev;
  node->next = next;
  prev->next = node;
  next->prev = node;
}

static inline void list_insert(list_node_t* node, list_node_t* prev, list_node_t* next) {
  list_insert_between(node, prev, next);
}

static inline void list_push_front(list_node_t* head, list_node_t* node) {
  list_insert_between(node, head, head->next);
}

static inline void list_push_back(list_node_t* head, list_node_t* node) {
  list_insert_between(node, head->prev, head);
}

static inline void list_remove(list_node_t* node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
  node->prev = node;
  node->next = node;
}

static inline bool list_is_linked(const list_node_t* node) {
  return node->next != node;
}

#define HXM_OFFSETOF(type, member) offsetof(type, member)

#ifndef container_of
#define container_of(ptr, type, member) ((type*)((char*)(ptr) - HXM_OFFSETOF(type, member)))
#endif

#define list_entry(ptr, type, member) container_of((ptr), type, member)

#define list_for_each(pos, head) for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_for_each_safe(pos, tmp, head) for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head); (pos) = (tmp), (tmp) = (pos)->next)

/* ---------- Opaque container forward declarations ---------- */

typedef struct hash_map hash_map_t;
typedef struct arena arena_t;
typedef struct small_vec small_vec_t;

/* ---------- Dirty rectangle region utils ---------- */

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
  dirty_region_t r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  r.valid = (w > 0u) && (h > 0u);
  return r;
}

static inline void dirty_region_union(dirty_region_t* dst, const dirty_region_t* src) {
  if (!src || !src->valid)
    return;

  if (!dst->valid) {
    *dst = *src;
    return;
  }

  int32_t x1 = (int32_t)dst->x;
  int32_t y1 = (int32_t)dst->y;
  int32_t x2 = x1 + (int32_t)dst->w;
  int32_t y2 = y1 + (int32_t)dst->h;

  int32_t sx1 = (int32_t)src->x;
  int32_t sy1 = (int32_t)src->y;
  int32_t sx2 = sx1 + (int32_t)src->w;
  int32_t sy2 = sy1 + (int32_t)src->h;

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
  if (!r->valid)
    return;

  int32_t x1 = (int32_t)r->x;
  int32_t y1 = (int32_t)r->y;
  int32_t x2 = x1 + (int32_t)r->w;
  int32_t y2 = y1 + (int32_t)r->h;

  int32_t bx1 = (int32_t)bx;
  int32_t by1 = (int32_t)by;
  int32_t bx2 = bx1 + (int32_t)bw;
  int32_t by2 = by1 + (int32_t)bh;

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

/* Monotonic clock helper, implemented elsewhere */
uint64_t monotonic_time_ns(void);

/* ---------- Logging ---------- */

#ifndef HXM_DIAG
#define HXM_DIAG 0
#endif

#if HXM_DIAG

#define HXM_LOG_LEVEL_DEBUG 0
#define HXM_LOG_LEVEL_INFO 1
#define HXM_LOG_LEVEL_WARN 2
#define HXM_LOG_LEVEL_ERROR 3

enum log_level {
  LOG_DEBUG = HXM_LOG_LEVEL_DEBUG,
  LOG_INFO = HXM_LOG_LEVEL_INFO,
  LOG_WARN = HXM_LOG_LEVEL_WARN,
  LOG_ERROR = HXM_LOG_LEVEL_ERROR,
};

/* Compile-time log level selection
 * Define HXM_LOG_MIN_LEVEL to one of HXM_LOG_LEVEL_*
 * Default keeps DEBUG/INFO/WARN/ERROR in diagnostics builds.
 */
#ifndef HXM_LOG_MIN_LEVEL
#ifdef HXM_LOG_LEVEL
#define HXM_LOG_MIN_LEVEL HXM_LOG_LEVEL
#else
#define HXM_LOG_MIN_LEVEL HXM_LOG_LEVEL_DEBUG
#endif
#endif

#ifndef HXM_LOG_LEVEL
#define HXM_LOG_LEVEL HXM_LOG_MIN_LEVEL
#endif

/* The logger backend should be implemented in a .c file
 * It must be safe to call from the main thread
 */
void hxm_log(enum log_level level, const char* fmt, ...) HXM_ATTR_PRINTF(2, 3);

#define HXM_LOG_ENABLED(level) ((level) >= HXM_LOG_MIN_LEVEL)

#if HXM_LOG_ENABLED(HXM_LOG_LEVEL_DEBUG)
#if defined(HXM_VERBOSE_LOGS) && HXM_VERBOSE_LOGS
#define LOG_DEBUG(...) hxm_log(LOG_DEBUG, __VA_ARGS__)
#define TRACE_LOG(...) hxm_log(LOG_DEBUG, __VA_ARGS__)
#else
#define LOG_DEBUG(...) \
  do {                 \
  } while (0)
#define TRACE_LOG(...) \
  do {                 \
  } while (0)
#endif
#else
#define LOG_DEBUG(...) \
  do {                 \
  } while (0)
#define TRACE_LOG(...) \
  do {                 \
  } while (0)
#endif

#if HXM_LOG_ENABLED(HXM_LOG_LEVEL_INFO)
#define LOG_INFO(...) hxm_log(LOG_INFO, __VA_ARGS__)
#else
#define LOG_INFO(...) \
  do {                \
  } while (0)
#endif

#if HXM_LOG_ENABLED(HXM_LOG_LEVEL_WARN)
#define LOG_WARN(...) hxm_log(LOG_WARN, __VA_ARGS__)
#define TRACE_WARN(...) hxm_log(LOG_WARN, __VA_ARGS__)
#else
#define LOG_WARN(...) \
  do {                \
  } while (0)
#define TRACE_WARN(...) \
  do {                  \
  } while (0)
#endif

#if HXM_LOG_ENABLED(HXM_LOG_LEVEL_ERROR)
#define LOG_ERROR(...) hxm_log(LOG_ERROR, __VA_ARGS__)
#else
#define LOG_ERROR(...) \
  do {                 \
  } while (0)
#endif

#define TRACE_ONLY(...) \
  do {                  \
    __VA_ARGS__;        \
  } while (0)

/* Compile-time flag for whether TRACE_LOG emits code. */
#if HXM_LOG_ENABLED(HXM_LOG_LEVEL_DEBUG) && defined(HXM_VERBOSE_LOGS) && HXM_VERBOSE_LOGS
#define HXM_TRACE_LOGS 1
#else
#define HXM_TRACE_LOGS 0
#endif

#else

void hxm_err(const char* fmt, ...) HXM_ATTR_PRINTF(1, 2);

#define LOG_ERROR(...) hxm_err(__VA_ARGS__)
#define LOG_WARN(...) \
  do {                \
  } while (0)
#define LOG_INFO(...) \
  do {                \
  } while (0)
#define LOG_DEBUG(...) \
  do {                 \
  } while (0)
#define TRACE_LOG(...) \
  do {                 \
  } while (0)
#define TRACE_WARN(...) \
  do {                  \
  } while (0)
#define TRACE_ONLY(...) \
  do {                  \
  } while (0)

#define HXM_TRACE_LOGS 0

#endif

#if HXM_DIAG
#define HXM_DIAG_FIELD(x) x
#else
#define HXM_DIAG_FIELD(x)
#endif

/* ---------- Perf counters ---------- */

#if HXM_DIAG
struct counters {
  uint64_t events_seen[256];
  uint64_t events_unhandled[256];
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
void counters_tick_record(uint64_t dt_ns);
void counters_dump(void);

#define HXM_COUNTER_EVENT_SEEN(type) (counters.events_seen[(uint8_t)(type)]++)
#define HXM_COUNTER_EVENT_UNHANDLED(type) (counters.events_unhandled[(uint8_t)(type)]++)
#define HXM_COUNTER_COALESCED_DROP(type) (counters.coalesced_drops[(uint8_t)(type)]++)

#define HXM_COUNTER_X_FLUSH() (counters.x_flush_count++)
#define HXM_COUNTER_RESTACK() (counters.restacks_applied++)
#define HXM_COUNTER_CONFIG_APPLIED() (counters.config_requests_applied++)

#else

static inline void counters_init(void) {}
static inline void counters_tick_record(uint64_t dt_ns) {
  (void)dt_ns;
}

#define HXM_COUNTER_EVENT_SEEN(type) ((void)0)
#define HXM_COUNTER_EVENT_UNHANDLED(type) ((void)0)
#define HXM_COUNTER_COALESCED_DROP(type) ((void)0)

#define HXM_COUNTER_X_FLUSH() ((void)0)
#define HXM_COUNTER_RESTACK() ((void)0)
#define HXM_COUNTER_CONFIG_APPLIED() ((void)0)

#endif

/* ---------- Rate limiter ---------- */

typedef struct rl {
  uint64_t last_ns;
  uint32_t suppressed;
} rl_t;

#define RL_INIT {0u, 0u}

static inline void rl_reset(rl_t* rl) {
  rl->last_ns = 0;
  rl->suppressed = 0;
}

static inline bool rl_allow(rl_t* rl, uint64_t now_ns, uint64_t interval_ns) {
  if (interval_ns == 0) {
    rl->last_ns = now_ns;
    return true;
  }

  if (now_ns - rl->last_ns >= interval_ns) {
    rl->last_ns = now_ns;
    return true;
  }

  rl->suppressed++;
  return false;
}

#ifdef __cplusplus
}
#endif

#endif /* HXM_H */
