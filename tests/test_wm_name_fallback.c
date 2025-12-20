#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

volatile sig_atomic_t g_reload_pending = 0;

static xcb_get_property_reply_t* make_string_reply(xcb_atom_t type, const char* value, int len) {
    size_t total = sizeof(xcb_get_property_reply_t) + (size_t)len;
    xcb_get_property_reply_t* rep = calloc(1, total);
    if (!rep) return NULL;
    rep->format = 8;
    rep->type = type;
    rep->value_len = (uint32_t)len;
    if (len > 0 && value) {
        memcpy(xcb_get_property_value(rep), value, (size_t)len);
    }
    return rep;
}

void test_net_wm_name_fallback() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.conn = (xcb_connection_t*)malloc(1);

    atoms._NET_WM_NAME = 10;
    atoms.WM_NAME = 11;
    atoms.UTF8_STRING = 12;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 123;
    hot->state = STATE_MAPPED;
    hot->pending_replies = 1;
    arena_init(&cold->string_arena, 512);

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;

    xcb_get_property_reply_t* rep = make_string_reply(atoms.UTF8_STRING, "modern", 6);
    slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;
    wm_handle_reply(&s, &slot, rep);
    free(rep);

    assert(cold->has_net_wm_name);
    assert(strcmp(cold->title, "modern") == 0);

    rep = make_string_reply(XCB_ATOM_STRING, "legacy", 6);
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_NAME;
    wm_handle_reply(&s, &slot, rep);
    free(rep);
    assert(strcmp(cold->title, "modern") == 0);

    rep = make_string_reply(atoms.UTF8_STRING, "", 0);
    slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;
    wm_handle_reply(&s, &slot, rep);
    free(rep);
    assert(!cold->has_net_wm_name);

    rep = make_string_reply(XCB_ATOM_STRING, "legacy", 6);
    slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_NAME;
    wm_handle_reply(&s, &slot, rep);
    free(rep);
    assert(strcmp(cold->title, "legacy") == 0);

    printf("test_net_wm_name_fallback passed\n");

    arena_destroy(&cold->string_arena);
    slotmap_destroy(&s.clients);
    free(s.conn);
}

int main() {
    test_net_wm_name_fallback();
    return 0;
}
