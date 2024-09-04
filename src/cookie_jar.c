#include "cookie_jar.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcbext.h>

#include "bbox.h"

#define COOKIE_JAR_TIMEOUT_NS 5000000000ULL
#define COOKIE_JAR_MAX_LOAD_NUM 7
#define COOKIE_JAR_MAX_LOAD_DEN 10

static size_t cookie_jar_next_pow2(size_t x) {
    size_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

static inline size_t cookie_home(uint32_t seq, size_t mask) { return ((size_t)seq) & mask; }
static inline size_t cookie_next(size_t i, size_t mask) { return (i + 1) & mask; }

static size_t cookie_jar_probe(const cookie_jar_t* cj, uint32_t seq) {
    size_t mask = cj->cap - 1;
    size_t i = cookie_home(seq, mask);
    while (cj->slots[i].live && cj->slots[i].sequence != seq) {
        i = cookie_next(i, mask);
    }
    return i;
}

static void cookie_jar_grow(cookie_jar_t* cj, size_t new_cap) {
    new_cap = cookie_jar_next_pow2(new_cap);
    if (new_cap < 2) new_cap = 2;

    cookie_slot_t* new_slots = calloc(new_cap, sizeof(*new_slots));
    if (!new_slots) {
        LOG_ERROR("cookie_jar_grow failed");
        exit(1);
    }

    size_t old_cap = cj->cap;
    cookie_slot_t* old_slots = cj->slots;

    cj->slots = new_slots;
    cj->cap = new_cap;
    cj->live_count = 0;
    cj->scan_cursor = 0;

    if (old_slots && old_cap) {
        for (size_t i = 0; i < old_cap; i++) {
            if (!old_slots[i].live) continue;

            cookie_slot_t slot = old_slots[i];
            size_t idx = cookie_jar_probe(cj, slot.sequence);
            cj->slots[idx] = slot;
            cj->slots[idx].live = true;
            cj->live_count++;
        }
        free(old_slots);
    }
}

static void cookie_jar_remove(cookie_jar_t* cj, size_t idx) {
    size_t mask = cj->cap - 1;

    cj->slots[idx].live = false;
    cj->slots[idx].handler = NULL;
    cj->live_count--;

    // Backshift deletion to preserve linear-probe invariants
    size_t hole = idx;
    size_t i = cookie_next(hole, mask);

    while (cj->slots[i].live) {
        size_t home = cookie_home(cj->slots[i].sequence, mask);

        bool should_move;
        if (home <= i) {
            should_move = (home <= hole && hole < i);
        } else {
            should_move = (hole < i) || (home <= hole);
        }

        if (should_move) {
            cj->slots[hole] = cj->slots[i];
            cj->slots[i].live = false;
            cj->slots[i].handler = NULL;
            hole = i;
        }

        i = cookie_next(i, mask);
    }
}

void cookie_jar_init(cookie_jar_t* cj) {
    size_t cap = COOKIE_JAR_CAP;
    if (cap < 16) cap = 16;
    cap = cookie_jar_next_pow2(cap);

    cj->slots = calloc(cap, sizeof(*cj->slots));
    if (!cj->slots) {
        LOG_ERROR("cookie_jar_init failed");
        exit(1);
    }

    cj->cap = cap;
    cj->live_count = 0;
    cj->scan_cursor = 0;
}

void cookie_jar_destroy(cookie_jar_t* cj) {
    free(cj->slots);
    memset(cj, 0, sizeof(*cj));
}

bool cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data,
                     cookie_handler_fn handler) {
    // Grow before we hit the load factor
    if ((cj->live_count + 1) * COOKIE_JAR_MAX_LOAD_DEN >= cj->cap * COOKIE_JAR_MAX_LOAD_NUM) {
        cookie_jar_grow(cj, cj->cap ? (cj->cap * 2) : 16);
    }

    size_t idx = cookie_jar_probe(cj, sequence);
    if (cj->slots[idx].live) {
        LOG_WARN("CookieJar seq collision %u", sequence);
        return false;
    }

    cookie_slot_t* slot = &cj->slots[idx];
    slot->sequence = sequence;
    slot->type = type;
    slot->client = client;
    slot->data = data;
    slot->timestamp_ns = monotonic_time_ns();
    slot->handler = handler;
    slot->live = true;

    cj->live_count++;
    return true;
}

void cookie_jar_drain(cookie_jar_t* cj, xcb_connection_t* conn, struct server* s, size_t max_replies) {
    if (!cj || cj->live_count == 0 || max_replies == 0) return;

    size_t processed = 0;
    size_t scanned = 0;
    size_t idx = cj->scan_cursor;
    size_t mask = cj->cap - 1;
    uint64_t now = monotonic_time_ns();

    while (scanned < cj->cap && processed < max_replies && cj->live_count > 0) {
        cookie_slot_t* slot = &cj->slots[idx];

        if (slot->live) {
            void* reply = NULL;
            xcb_generic_error_t* err = NULL;

            // Returns 1 if a reply or error is ready, 0 otherwise
            int ready = xcb_poll_for_reply(conn, slot->sequence, &reply, &err);

            if (ready) {
                cookie_slot_t local = *slot;
                cookie_jar_remove(cj, idx);

                if (local.handler) local.handler(s, &local, reply, err);

                if (reply) free(reply);
                if (err) free(err);

                processed++;
                continue;
            }

            // Not ready, check timeout
            if (now > slot->timestamp_ns && now - slot->timestamp_ns > COOKIE_JAR_TIMEOUT_NS) {
                cookie_slot_t local = *slot;
                cookie_jar_remove(cj, idx);

                LOG_WARN("Cookie %u timed out, dropping", local.sequence);
                if (local.handler) local.handler(s, &local, NULL, NULL);

                processed++;
                continue;
            }
        }

        idx = cookie_next(idx, mask);
        scanned++;
    }

    cj->scan_cursor = idx;
}
