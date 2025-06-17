/*
 * cookie_jar.h - Asynchronous X11 reply handling
 *
 * The cookie jar tracks in-flight XCB requests (by sequence number) and dispatches
 * their replies later without blocking the main loop
 *
 * Intended usage:
 * - When sending an async request, capture its sequence number (cookie.sequence)
 * - Register it with cookie_jar_push along with type/context and a handler callback
 * - Periodically call cookie_jar_drain from the main loop to poll and dispatch replies
 *
 * Key properties:
 * - Non-blocking: uses xcb_poll_for_reply
 * - Bounded work per tick: max replies per drain call
 * - Timeout: stale requests are expired to avoid leaks
 *
 * Contracts:
 * - Not thread-safe
 * - Callbacks must not call xcb_wait_for_reply / xcb_get_*_reply for the same sequence
 * - The reply pointer passed to handler is owned by XCB and must be free()'d by the handler
 *   (XCB replies are heap allocated; this is the conventional contract)
 * - err pointer (if non-NULL) is owned by XCB and must be free()'d by the handler
 *
 * Implementation notes:
 * - COOKIE_JAR_CAP must be a power of two
 * - Sequence numbers are 32-bit in XCB, but only the low 16 bits are used on the wire
 *   XCB exposes 32-bit sequences and this module treats them as opaque u32 identifiers
 */

#ifndef COOKIE_JAR_H
#define COOKIE_JAR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xcb/xcb.h>

#include "handle.h"

/* Cookie categories for diagnostics/dispatch */
typedef enum cookie_type {
    COOKIE_NONE = 0,

    COOKIE_GET_WINDOW_ATTRIBUTES,
    COOKIE_GET_GEOMETRY,
    COOKIE_GET_PROPERTY,
    COOKIE_GET_PROPERTY_FRAME_EXTENTS,

    COOKIE_QUERY_TREE,
    COOKIE_QUERY_POINTER,

    COOKIE_SYNC_QUERY_COUNTER,

    COOKIE_CHECK_MANAGE_MAP_REQUEST
} cookie_type_t;

struct server;
struct cookie_slot;

/* Handler invoked when a reply becomes available or a timeout/error occurs
 *
 * reply:
 * - Non-NULL when a reply is available
 * - May be NULL if request timed out or error prevented a reply
 *
 * err:
 * - Non-NULL when the server returned an error for the request
 * - May be NULL if no error was reported
 *
 * Ownership:
 * - If reply is non-NULL, the handler must free(reply)
 * - If err is non-NULL, the handler must free(err)
 */
typedef void (*cookie_handler_fn)(struct server* s, const struct cookie_slot* slot, void* reply,
                                  xcb_generic_error_t* err);

typedef struct cookie_slot {
    uint32_t sequence;  /* xcb cookie sequence */
    cookie_type_t type; /* categorization */
    handle_t client;    /* associated client handle, if any */
    uintptr_t data;     /* opaque extra data (e.g. atom) */

    uint64_t timestamp_ns; /* enqueue time */
    uint64_t txn_id;       /* optional transaction/group id */

    cookie_handler_fn handler;
    bool live;
} cookie_slot_t;

/* Must be power of two */
#ifndef COOKIE_JAR_CAP
#define COOKIE_JAR_CAP 1024u
#endif

#ifndef COOKIE_JAR_MAX_REPLIES_PER_TICK
#define COOKIE_JAR_MAX_REPLIES_PER_TICK 64u
#endif

/* Default timeout for an in-flight cookie (nanoseconds) */
#ifndef COOKIE_JAR_TIMEOUT_NS
#define COOKIE_JAR_TIMEOUT_NS (5ull * 1000ull * 1000ull * 1000ull)
#endif

typedef struct cookie_jar {
    cookie_slot_t* slots;
    size_t cap;
    size_t live_count;
    size_t scan_cursor;
} cookie_jar_t;

/* Initialize/destroy */
void cookie_jar_init(cookie_jar_t* cj);
void cookie_jar_destroy(cookie_jar_t* cj);

/* Push a cookie into the jar
 * Returns false if jar is full or parameters invalid
 *
 * sequence must be non-zero (0 is reserved for "invalid/no cookie")
 * handler must be non-NULL
 */
bool cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data,
                     uint64_t txn_id, cookie_handler_fn handler);

/* Drain ready replies (non-blocking)
 * Polls up to max_replies ready replies and dispatches handlers
 * Also expires timed out cookies (handler called with reply=NULL)
 *
 * If max_replies is 0, COOKIE_JAR_MAX_REPLIES_PER_TICK is used
 */
void cookie_jar_drain(cookie_jar_t* cj, xcb_connection_t* conn, struct server* s, size_t max_replies);

static inline bool cookie_jar_has_pending(const cookie_jar_t* cj) { return cj && cj->live_count > 0; }

/* Optional helpers for diagnostics */
static inline size_t cookie_jar_capacity(const cookie_jar_t* cj) { return cj ? cj->cap : 0u; }

#ifdef __cplusplus
}
#endif

#endif /* COOKIE_JAR_H */
