#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"
#include "xcb_utils.h"

// Mock server state
static server_t s;
static client_hot_t* hot;
static client_cold_t* cold;
static handle_t h;

// Setup
static void setup(void) {
    memset(&s, 0, sizeof(s));
    small_vec_init(&s.active_clients);
    cookie_jar_init(&s.cookie_jar);

    // Initialize slotmap
    slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));

    // Create a dummy client
    void* cold_ptr = NULL;
    h = slotmap_alloc(&s.clients, (void**)&hot, &cold_ptr);
    cold = (client_cold_t*)cold_ptr;

    hot->self = h;
    hot->xid = 100;

    // Initialize cold data
    arena_init(&cold->string_arena, 4096);

    // Mock atoms
    atoms._NET_WM_NAME = 1;
    atoms.WM_NAME = 2;
    atoms.UTF8_STRING = 3;
    // STRING is 31 (XCB_ATOM_STRING)
}

static void teardown(void) {
    // Clean up cold data arenas for all clients
    // (In a real run we'd iterate, but here we only have one)
    if (cold) {
        arena_destroy(&cold->string_arena);
    }

    small_vec_destroy(&s.active_clients);
    cookie_jar_destroy(&s.cookie_jar);
    slotmap_destroy(&s.clients);
}

// Helper to simulate property reply
static xcb_get_property_reply_t* make_string_reply(xcb_atom_t type, const char* str, int len) {
    xcb_get_property_reply_t* rep = calloc(1, sizeof(*rep) + len + 1);
    rep->type = type;
    rep->format = 8;
    rep->value_len = len;
    memcpy(xcb_get_property_value(rep), str, len);
    return rep;
}

static void test_net_wm_name_update(void) {
    printf("Testing _NET_WM_NAME update...\n");
    setup();

    // 1. Mark title dirty
    hot->dirty |= DIRTY_TITLE;

    // 2. Simulate reply for _NET_WM_NAME
    cookie_slot_t slot = {0};
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;

    xcb_get_property_reply_t* rep = make_string_reply(atoms.UTF8_STRING, "NetTitle", 8);
    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);

    assert(cold->title != NULL);
    assert(strcmp(cold->title, "NetTitle") == 0);
    assert(cold->has_net_wm_name);

    teardown();
    printf("PASS: _NET_WM_NAME update\n");
}

static void test_wm_name_fallback(void) {
    printf("Testing WM_NAME fallback...\n");
    setup();

    // 1. Mark title dirty
    hot->dirty |= DIRTY_TITLE;

    // 2. Simulate empty _NET_WM_NAME (or missing)
    cookie_slot_t slot_net = {0};
    slot_net.type = COOKIE_GET_PROPERTY;
    slot_net.client = h;
    slot_net.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;

    xcb_get_property_reply_t* rep_net = make_string_reply(0, "", 0);  // Empty
    wm_handle_reply(&s, &slot_net, rep_net, NULL);
    free(rep_net);

    assert(cold->has_net_wm_name == false);

    // 3. Simulate WM_NAME present
    cookie_slot_t slot_legacy = {0};
    slot_legacy.type = COOKIE_GET_PROPERTY;
    slot_legacy.client = h;
    slot_legacy.data = ((uint64_t)hot->xid << 32) | atoms.WM_NAME;

    xcb_get_property_reply_t* rep_legacy = make_string_reply(XCB_ATOM_STRING, "LegacyTitle", 11);
    wm_handle_reply(&s, &slot_legacy, rep_legacy, NULL);
    free(rep_legacy);

    assert(cold->title != NULL);
    assert(strcmp(cold->title, "LegacyTitle") == 0);

    teardown();
    printf("PASS: WM_NAME fallback\n");
}

static void test_title_truncation(void) {
    printf("Testing title truncation...\n");
    setup();

    char long_title[5000];
    memset(long_title, 'A', 4999);
    long_title[4999] = '\0';

    cookie_slot_t slot = {0};
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;

    xcb_get_property_reply_t* rep = make_string_reply(atoms.UTF8_STRING, long_title, 4999);
    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);

    assert(cold->title != NULL);
    // MAX_TITLE_BYTES is 4096 in wm_reply.c, but since it's internal we can't check the constant directly easily
    // unless it's in a header. We check the length is 4096.
    assert(strlen(cold->title) == 4096);

    teardown();
    printf("PASS: Title truncation\n");
}

int main(void) {
    test_net_wm_name_update();
    test_wm_name_fallback();
    test_title_truncation();
    return 0;
}
