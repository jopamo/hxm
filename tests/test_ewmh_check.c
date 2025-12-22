#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bbox.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

// From tests/xcb_stubs.c
extern void xcb_stubs_reset(void);

extern int stub_map_window_count;
extern xcb_window_t stub_mapped_windows[256];
extern int stub_mapped_windows_len;

static bool was_mapped(xcb_window_t w) {
    for (int i = 0; i < stub_mapped_windows_len; i++) {
        if (stub_mapped_windows[i] == w) return true;
    }
    return false;
}

static void dump_mapped(void) {
    fprintf(stderr, "Mapped windows (%d):", stub_mapped_windows_len);
    for (int i = 0; i < stub_mapped_windows_len; i++) {
        fprintf(stderr, " %u", stub_mapped_windows[i]);
    }
    fprintf(stderr, "\n");
}

static void test_supporting_wm_check_mapped(void) {
    server_t s;
    memset(&s, 0, sizeof(s));

    s.conn = xcb_connect(NULL, NULL);
    if (!s.conn || xcb_connection_has_error(s.conn)) {
        fprintf(stderr, "FAIL: xcb_connect failed\n");
        exit(1);
    }

    // Keep root distinct from stub xid range
    s.root = 99;

    // Ensure atoms table exists
    atoms_init(s.conn);

    // Keep server state sane for future logic
    slotmap_init(&s.clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    s.desktop_count = 4;

    xcb_stubs_reset();

    wm_become(&s);

    if (s.supporting_wm_check == XCB_WINDOW_NONE) {
        fprintf(stderr, "FAIL: supporting_wm_check not set\n");
        exit(1);
    }

    if (stub_map_window_count == 0) {
        fprintf(stderr, "FAIL: no windows mapped\n");
        exit(1);
    }

    if (!was_mapped(s.supporting_wm_check)) {
        fprintf(stderr, "FAIL: supporting_wm_check %u was not mapped\n", s.supporting_wm_check);
        dump_mapped();
        exit(1);
    }

    printf("PASS: supporting_wm_check %u was mapped\n", s.supporting_wm_check);

    slotmap_destroy(&s.clients);
    xcb_disconnect(s.conn);
}

// From tests/xcb_stubs.c
extern xcb_atom_t stub_last_prop_atom;
extern uint32_t stub_last_prop_len;
extern uint8_t stub_last_prop_data[4096];

static void test_net_client_list_published(void) {
    server_t s;
    memset(&s, 0, sizeof(s));

    s.conn = xcb_connect(NULL, NULL);
    s.root = 1;
    atoms_init(s.conn);
    slotmap_init(&s.clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    s.desktop_count = 1;
    xcb_stubs_reset();

    // Add a client
    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    (void)h;
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    hot->xid = 12345;
    hot->state = STATE_MAPPED;

    // Trigger update
    s.root_dirty = ROOT_DIRTY_CLIENT_LIST;
    wm_flush_dirty(&s);

    assert(stub_last_prop_atom == atoms._NET_CLIENT_LIST);
    assert(stub_last_prop_len == 1);
    uint32_t* wins = (uint32_t*)stub_last_prop_data;
    assert(wins[0] == 12345);

    printf("PASS: _NET_CLIENT_LIST published\n");

    arena_destroy(&s.tick_arena);
    slotmap_destroy(&s.clients);
    xcb_disconnect(s.conn);
}

int main(void) {
    test_supporting_wm_check_mapped();
    test_net_client_list_published();
    return 0;
}
