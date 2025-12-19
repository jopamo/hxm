#ifndef COOKIE_JAR_H
#define COOKIE_JAR_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

#include "handle.h"

typedef enum cookie_type {
    COOKIE_NONE = 0,
    COOKIE_GET_WINDOW_ATTRIBUTES,
    COOKIE_GET_GEOMETRY,
    COOKIE_GET_PROPERTY,
    COOKIE_QUERY_TREE,
} cookie_type_t;

typedef struct cookie_slot {
    uint32_t sequence;
    uint8_t type;
    handle_t client;
    uintptr_t data;  // Extra data (e.g. atom for property)
    uint64_t timestamp_ns;
} cookie_slot_t;

#define COOKIE_JAR_SIZE 256

typedef struct cookie_jar {
    cookie_slot_t slots[COOKIE_JAR_SIZE];
    uint32_t head;
    uint32_t tail;
} cookie_jar_t;

void cookie_jar_init(cookie_jar_t* cj);
bool cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data);
bool cookie_jar_poll(cookie_jar_t* cj, xcb_connection_t* conn, cookie_slot_t* out_slot, void** out_reply);

#endif  // COOKIE_JAR_H
