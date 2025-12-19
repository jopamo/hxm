#include "cookie_jar.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcbext.h>

#include "bbox.h"

void cookie_jar_init(cookie_jar_t* cj) { memset(cj, 0, sizeof(*cj)); }

bool cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data) {
    uint32_t next_head = (cj->head + 1) % COOKIE_JAR_SIZE;
    if (next_head == cj->tail) {
        LOG_WARN("CookieJar full, dropping cookie %u", sequence);
        return false;
    }

    cookie_slot_t* slot = &cj->slots[cj->head];
    slot->sequence = sequence;
    slot->type = type;
    slot->client = client;
    slot->data = data;
    slot->timestamp_ns = monotonic_time_ns();

    cj->head = next_head;
    return true;
}

bool cookie_jar_poll(cookie_jar_t* cj, xcb_connection_t* conn, cookie_slot_t* out_slot, void** out_reply) {
    if (cj->head == cj->tail) return false;

    cookie_slot_t* slot = &cj->slots[cj->tail];
    xcb_generic_error_t* err = NULL;
    void* reply = NULL;

    // xcb_poll_for_reply returns 1 if reply is available (or error), 0 if pending
    if (xcb_poll_for_reply(conn, slot->sequence, &reply, &err)) {
        *out_slot = *slot;
        *out_reply = reply;
        if (err) {
            LOG_DEBUG("Cookie %u returned error code %d", slot->sequence, err->error_code);
            free(err);
        }
        cj->tail = (cj->tail + 1) % COOKIE_JAR_SIZE;
        return true;
    }

    // Check for timeout
    uint64_t now = monotonic_time_ns();
    if (now - slot->timestamp_ns > 5000000000ULL) {  // 5 seconds timeout
        LOG_WARN("Cookie %u timed out, dropping", slot->sequence);
        *out_slot = *slot;
        *out_reply = NULL;
        cj->tail = (cj->tail + 1) % COOKIE_JAR_SIZE;
        return true;
    }

    return false;
}
