#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "event.h"
#include "hxm.h"
#include "wm.h"
#include "xcb_utils.h"

// From tests/xcb_stubs.c
extern void xcb_stubs_reset(void);
extern int (*stub_poll_for_reply_hook)(xcb_connection_t* c, unsigned int request, void** reply,
                                       xcb_generic_error_t** error);

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
    wm_flush_dirty(&s);

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
extern int stub_prop_calls_len;
extern struct stub_prop_call {
    xcb_window_t window;
    xcb_atom_t atom;
    xcb_atom_t type;
    uint8_t format;
    uint32_t len;
    uint8_t data[4096];
    bool deleted;
} stub_prop_calls[128];

extern void xcb_stubs_set_selection_owner(xcb_window_t owner);
extern xcb_window_t xcb_stubs_get_selection_owner(void);

static bool g_force_badaccess_once = false;

static int poll_badaccess_once(xcb_connection_t* c, unsigned int request, void** reply, xcb_generic_error_t** error) {
    (void)c;
    (void)request;

    if (!g_force_badaccess_once) return 0;

    g_force_badaccess_once = false;
    *reply = NULL;
    *error = malloc(sizeof(xcb_generic_error_t));
    if (*error) {
        memset(*error, 0, sizeof(xcb_generic_error_t));
        (*error)->error_code = XCB_ACCESS;
    }
    return 1;
}

static void test_net_client_list_published(void) {
    server_t s;
    memset(&s, 0, sizeof(s));

    s.conn = xcb_connect(NULL, NULL);
    s.root = 1;
    atoms_init(s.conn);
    slotmap_init(&s.clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    small_vec_init(&s.active_clients);
    s.desktop_count = 1;
    xcb_stubs_reset();

    // Add a client
    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    small_vec_push(&s.active_clients, handle_to_ptr(h));
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

static const struct stub_prop_call* find_prop_call(xcb_window_t win, xcb_atom_t atom) {
    for (int i = stub_prop_calls_len - 1; i >= 0; i--) {
        if (stub_prop_calls[i].window == win && stub_prop_calls[i].atom == atom && !stub_prop_calls[i].deleted) {
            return &stub_prop_calls[i];
        }
    }
    return NULL;
}

static void test_refuse_when_substructure_redirect_fails(void) {
    server_t s;
    memset(&s, 0, sizeof(s));

    s.conn = xcb_connect(NULL, NULL);
    s.root = 42;
    atoms_init(s.conn);
    slotmap_init(&s.clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    s.desktop_count = 1;

    xcb_stubs_reset();
    g_force_badaccess_once = true;
    stub_poll_for_reply_hook = poll_badaccess_once;

    wm_become(&s);
    wm_flush_dirty(&s);

    assert(s.supporting_wm_check == XCB_WINDOW_NONE);
    assert(stub_map_window_count == 0);
    assert(xcb_stubs_get_selection_owner() == XCB_NONE);

    stub_poll_for_reply_hook = NULL;
    printf("PASS: WM refuses when SubstructureRedirect is unavailable\n");

    slotmap_destroy(&s.clients);
    xcb_disconnect(s.conn);
}

static void test_existing_wm_keeps_selection_owner(void) {
    server_t s1;
    memset(&s1, 0, sizeof(s1));

    s1.conn = xcb_connect(NULL, NULL);
    s1.root = 7;
    atoms_init(s1.conn);
    slotmap_init(&s1.clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    s1.desktop_count = 1;

    xcb_stubs_reset();
    wm_become(&s1);
    wm_flush_dirty(&s1);

    xcb_window_t owner = xcb_stubs_get_selection_owner();
    assert(owner == s1.supporting_wm_check);
    int map_count = stub_map_window_count;

    const struct stub_prop_call* root_call = find_prop_call(s1.root, atoms._NET_SUPPORTING_WM_CHECK);
    assert(root_call != NULL);
    const uint32_t* root_val = (const uint32_t*)root_call->data;
    assert(root_val[0] == owner);

    server_t s2;
    memset(&s2, 0, sizeof(s2));
    s2.conn = xcb_connect(NULL, NULL);
    s2.root = s1.root;
    slotmap_init(&s2.clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    s2.desktop_count = 1;

    wm_become(&s2);
    wm_flush_dirty(&s2);

    assert(s2.supporting_wm_check == XCB_WINDOW_NONE);
    assert(xcb_stubs_get_selection_owner() == owner);
    assert(stub_map_window_count == map_count);

    printf("PASS: existing WM keeps WM_S0 ownership after second start\n");

    slotmap_destroy(&s2.clients);
    xcb_disconnect(s2.conn);
    slotmap_destroy(&s1.clients);
    xcb_disconnect(s1.conn);
}

static void test_wm_s0_selection_and_supporting_check(void) {
    server_t s;
    memset(&s, 0, sizeof(s));

    s.conn = xcb_connect(NULL, NULL);
    s.root = 77;
    atoms_init(s.conn);
    slotmap_init(&s.clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    s.desktop_count = 1;
    xcb_stubs_reset();

    wm_become(&s);
    wm_flush_dirty(&s);

    assert(xcb_stubs_get_selection_owner() == s.supporting_wm_check);

    const struct stub_prop_call* root_call = find_prop_call(s.root, atoms._NET_SUPPORTING_WM_CHECK);
    assert(root_call != NULL);
    assert(root_call->format == 32);
    assert(root_call->len == 1);
    const uint32_t* root_val = (const uint32_t*)root_call->data;
    assert(root_val[0] == s.supporting_wm_check);

    const struct stub_prop_call* support_call = find_prop_call(s.supporting_wm_check, atoms._NET_SUPPORTING_WM_CHECK);
    assert(support_call != NULL);
    assert(support_call->format == 32);
    assert(support_call->len == 1);
    const uint32_t* support_val = (const uint32_t*)support_call->data;
    assert(support_val[0] == s.supporting_wm_check);

    printf("PASS: WM_S0 and _NET_SUPPORTING_WM_CHECK round-trip\n");

    slotmap_destroy(&s.clients);
    xcb_disconnect(s.conn);
}

static void test_refuse_when_selection_owned(void) {
    server_t s;
    memset(&s, 0, sizeof(s));

    s.conn = xcb_connect(NULL, NULL);
    s.root = 55;
    atoms_init(s.conn);
    slotmap_init(&s.clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    s.desktop_count = 1;

    xcb_stubs_reset();
    xcb_stubs_set_selection_owner(999);

    wm_become(&s);
    wm_flush_dirty(&s);

    assert(s.supporting_wm_check == XCB_WINDOW_NONE);
    assert(stub_map_window_count == 0);
    assert(xcb_stubs_get_selection_owner() == 999);

    printf("PASS: WM refuses when WM_S0 owned\n");

    slotmap_destroy(&s.clients);
    xcb_disconnect(s.conn);
}

int main(void) {
    test_supporting_wm_check_mapped();
    test_net_client_list_published();
    test_refuse_when_substructure_redirect_fails();
    test_wm_s0_selection_and_supporting_check();
    test_existing_wm_keeps_selection_owner();
    test_refuse_when_selection_owned();
    return 0;
}
