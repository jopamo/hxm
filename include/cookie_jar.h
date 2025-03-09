#ifndef COOKIE_JAR_H
#define COOKIE_JAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xcb/xcb.h>

#include "handle.h"

typedef enum cookie_type {
    COOKIE_NONE = 0,
    COOKIE_GET_WINDOW_ATTRIBUTES,
    COOKIE_GET_GEOMETRY,
    COOKIE_GET_PROPERTY,
    COOKIE_GET_PROPERTY_FRAME_EXTENTS,
    COOKIE_QUERY_TREE,
    COOKIE_QUERY_POINTER,
} cookie_type_t;

struct server;
struct cookie_slot;

typedef void (*cookie_handler_fn)(struct server* s, const struct cookie_slot* slot, void* reply,
                                  xcb_generic_error_t* err);

typedef struct cookie_slot {
    uint32_t sequence;
    cookie_type_t type;
    handle_t client;
    uintptr_t data;  // Extra data (e.g. atom for property)
    uint64_t timestamp_ns;
    uint64_t txn_id;
    cookie_handler_fn handler;
    bool live;
} cookie_slot_t;

#define COOKIE_JAR_CAP 1024  // Must be a power of two
#define COOKIE_JAR_MAX_REPLIES_PER_TICK 64

typedef struct cookie_jar {
    cookie_slot_t* slots;
    size_t cap;
    size_t live_count;
    size_t scan_cursor;
} cookie_jar_t;

void cookie_jar_init(cookie_jar_t* cj);
void cookie_jar_destroy(cookie_jar_t* cj);
bool cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data,
                     uint64_t txn_id, cookie_handler_fn handler);
void cookie_jar_drain(cookie_jar_t* cj, xcb_connection_t* conn, struct server* s, size_t max_replies);
static inline bool cookie_jar_has_pending(const cookie_jar_t* cj) { return cj->live_count > 0; }

#endif  // COOKIE_JAR_H
