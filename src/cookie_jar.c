#include "cookie_jar.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcbext.h>

#include "bbox.h"

#define COOKIE_JAR_TIMEOUT_NS 5000000000ULL

static size_t cookie_jar_next_pow2(size_t x) {
    size_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

static size_t cookie_jar_probe(const cookie_jar_t* cj, uint32_t seq) {
    size_t mask = cj->cap - 1;
    size_t i = (size_t)seq & mask;
    while (cj->slots[i].live && cj->slots[i].sequence != seq) {
        i = (i + 1) & mask;
    }
    return i;
}

static void cookie_jar_remove(cookie_jar_t* cj, size_t idx) {
    size_t mask = cj->cap - 1;
    cj->slots[idx].live = false;
    cj->live_count--;

    size_t i = (idx + 1) & mask;
    while (cj->slots[i].live) {
        cookie_slot_t tmp = cj->slots[i];
        cj->slots[i].live = false;
        cj->live_count--;

        size_t home = (size_t)tmp.sequence & mask;
        size_t k = home;
        while (cj->slots[k].live) {
            k = (k + 1) & mask;
        }

        cj->slots[k] = tmp;
        cj->slots[k].live = true;
        cj->live_count++;

        i = (i + 1) & mask;
    }
}

void cookie_jar_init(cookie_jar_t* cj) {
    size_t cap = COOKIE_JAR_CAP;
    if (cap < 2) cap = 2;
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
    if ((cj->live_count + 1) * 10 >= cj->cap * 7) {
        LOG_WARN("CookieJar full, dropping cookie %u", sequence);
        return false;
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
    if (cj->live_count == 0 || max_replies == 0) return;

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
            int ok = xcb_poll_for_reply(conn, slot->sequence, &reply, &err);

            if (ok) {
                cookie_slot_t local = *slot;
                cookie_jar_remove(cj, idx);
                if (local.handler) local.handler(s, &local, reply, err);
                if (reply) free(reply);
                if (err) free(err);
                processed++;
                continue;
            }

            if (err) {
                free(err);
            }

            if (now - slot->timestamp_ns > COOKIE_JAR_TIMEOUT_NS) {
                cookie_slot_t local = *slot;
                cookie_jar_remove(cj, idx);
                LOG_WARN("Cookie %u timed out, dropping", local.sequence);
                if (local.handler) local.handler(s, &local, NULL, NULL);
                processed++;
                continue;
            }
        }

        idx = (idx + 1) & mask;
        scanned++;
    }

    cj->scan_cursor = idx;
}
