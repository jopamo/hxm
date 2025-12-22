#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xproto.h>

static xcb_connection_t* c;
static xcb_screen_t* screen;
static xcb_window_t root;

static void fail(const char* msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static void failf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static uint64_t now_us(void) {
    // coarse but fine for tests
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000ull);
}

static void xflush(void) {
    int rc = xcb_flush(c);
    if (rc <= 0) fail("xcb_flush failed");
}

static xcb_atom_t get_atom(const char* name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(c, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(c, cookie, NULL);
    if (!reply) fail("Failed to intern atom");
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

static bool atom_in_list(const xcb_atom_t* atoms, size_t n, xcb_atom_t a) {
    for (size_t i = 0; i < n; i++) {
        if (atoms[i] == a) return true;
    }
    return false;
}

static void* get_property_any(xcb_window_t win, xcb_atom_t prop, xcb_atom_t type, uint32_t* nbytes_out) {
    xcb_get_property_cookie_t ck = xcb_get_property(c, 0, win, prop, type, 0, 0x7fffffff);
    xcb_get_property_reply_t* r = xcb_get_property_reply(c, ck, NULL);
    if (!r) return NULL;
    int len = xcb_get_property_value_length(r);
    if (len <= 0) {
        free(r);
        if (nbytes_out) *nbytes_out = 0;
        return NULL;
    }
    void* val = malloc((size_t)len);
    if (!val) fail("oom");
    memcpy(val, xcb_get_property_value(r), (size_t)len);
    free(r);
    if (nbytes_out) *nbytes_out = (uint32_t)len;
    return val;
}

static bool wait_until_us(uint64_t deadline_us, bool (*pred)(void*), void* ctx) {
    while (now_us() < deadline_us) {
        if (pred(ctx)) return true;

        // drain some events so the connection doesn't starve
        for (int i = 0; i < 32; i++) {
            xcb_generic_event_t* ev = xcb_poll_for_event(c);
            if (!ev) break;
            free(ev);
        }

        usleep(2000);
    }
    return false;
}

static xcb_window_t create_window(uint16_t w, uint16_t h) {
    xcb_window_t win = xcb_generate_id(c);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE |
                                                  XCB_EVENT_MASK_FOCUS_CHANGE};

    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, root, 0, 0, w, h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, values);

    // give the WM something to chew on
    xcb_icccm_set_wm_class(c, win, (uint16_t)strlen("bbox-test\0bbox-test"), "bbox-test\0bbox-test");
    xcb_icccm_set_wm_name(c, win, XCB_ATOM_STRING, 8, (uint32_t)strlen("bbox-test-window"), "bbox-test-window");

    return win;
}

static void map_window(xcb_window_t win) {
    xcb_map_window(c, win);
    xflush();
}

static void destroy_window(xcb_window_t win) {
    xcb_destroy_window(c, win);
    xflush();
}

static bool is_mapped(xcb_window_t win) {
    xcb_get_window_attributes_cookie_t ck = xcb_get_window_attributes(c, win);
    xcb_get_window_attributes_reply_t* r = xcb_get_window_attributes_reply(c, ck, NULL);
    if (!r) return false;
    bool mapped = (r->map_state == XCB_MAP_STATE_VIEWABLE);
    free(r);
    return mapped;
}

static xcb_window_t get_parent(xcb_window_t win) {
    xcb_query_tree_cookie_t ck = xcb_query_tree(c, win);
    xcb_query_tree_reply_t* r = xcb_query_tree_reply(c, ck, NULL);
    if (!r) return XCB_WINDOW_NONE;
    xcb_window_t parent = r->parent;
    free(r);
    return parent;
}

static xcb_window_t get_net_active_window(void) {
    xcb_atom_t net_active = get_atom("_NET_ACTIVE_WINDOW");
    uint32_t n = 0;
    xcb_window_t* v = (xcb_window_t*)get_property_any(root, net_active, XCB_ATOM_WINDOW, &n);
    if (!v || n < sizeof(xcb_window_t)) {
        free(v);
        return XCB_WINDOW_NONE;
    }
    xcb_window_t w = v[0];
    free(v);
    return w;
}

static bool client_list_contains(xcb_atom_t prop, xcb_window_t win) {
    uint32_t nbytes = 0;
    xcb_window_t* list = (xcb_window_t*)get_property_any(root, prop, XCB_ATOM_WINDOW, &nbytes);
    if (!list || nbytes < sizeof(xcb_window_t)) {
        free(list);
        return false;
    }
    size_t n = (size_t)nbytes / sizeof(xcb_window_t);
    for (size_t i = 0; i < n; i++) {
        if (list[i] == win) {
            free(list);
            return true;
        }
    }
    free(list);
    return false;
}

static void require_eventual_reparent(xcb_window_t win, uint32_t timeout_ms, xcb_window_t* out_parent) {
    uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;

    while (now_us() < deadline) {
        // Prefer query_tree (works even if we missed the event)
        xcb_window_t p = get_parent(win);
        if (p != XCB_WINDOW_NONE && p != root) {
            if (out_parent) *out_parent = p;
            return;
        }

        // Also watch for ReparentNotify
        xcb_generic_event_t* ev = xcb_poll_for_event(c);
        if (ev) {
            if ((ev->response_type & ~0x80) == XCB_REPARENT_NOTIFY) {
                xcb_reparent_notify_event_t* re = (xcb_reparent_notify_event_t*)ev;
                if (re->window == win && re->parent != root) {
                    if (out_parent) *out_parent = re->parent;
                    free(ev);
                    return;
                }
            }
            free(ev);
        }

        usleep(2000);
    }

    fail("Window was not reparented (not managed?)");
}

static void require_eventual_mapped(xcb_window_t win, uint32_t timeout_ms) {
    uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;

    while (now_us() < deadline) {
        if (is_mapped(win)) return;

        xcb_generic_event_t* ev = xcb_poll_for_event(c);
        if (ev) free(ev);

        usleep(2000);
    }

    fail("Window did not become viewable (map_state != VIEWABLE)");
}

static void require_eventual_client_list_membership(xcb_window_t win, uint32_t timeout_ms) {
    xcb_atom_t net_client_list = get_atom("_NET_CLIENT_LIST");
    xcb_atom_t net_client_list_stacking = get_atom("_NET_CLIENT_LIST_STACKING");

    uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;

    while (now_us() < deadline) {
        bool in_list = client_list_contains(net_client_list, win);
        bool in_stack = client_list_contains(net_client_list_stacking, win);
        if (in_list && in_stack) return;

        // drain events
        for (int i = 0; i < 16; i++) {
            xcb_generic_event_t* ev = xcb_poll_for_event(c);
            if (!ev) break;
            free(ev);
        }
        usleep(2000);
    }

    fail("Window not present in _NET_CLIENT_LIST and/or _NET_CLIENT_LIST_STACKING");
}

static void require_eventual_active_window(xcb_window_t win, uint32_t timeout_ms) {
    uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;

    while (now_us() < deadline) {
        xcb_window_t aw = get_net_active_window();
        if (aw == win) return;

        // drain events
        for (int i = 0; i < 16; i++) {
            xcb_generic_event_t* ev = xcb_poll_for_event(c);
            if (!ev) break;
            free(ev);
        }
        usleep(2000);
    }

    xcb_window_t aw = get_net_active_window();
    failf("Expected _NET_ACTIVE_WINDOW=%u, got %u", win, aw);
}

static void require_best_effort_input_focus(xcb_window_t expected, uint32_t timeout_ms) {
    // Not all WMs guarantee this (focus-follows-mouse etc)
    // so we only warn unless it is completely nonsensical
    uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;
    xcb_window_t got = XCB_WINDOW_NONE;

    while (now_us() < deadline) {
        xcb_get_input_focus_cookie_t ck = xcb_get_input_focus(c);
        xcb_get_input_focus_reply_t* r = xcb_get_input_focus_reply(c, ck, NULL);
        if (r) {
            got = r->focus;
            free(r);
            if (got == expected) return;
        }

        usleep(2000);
    }

    if (got == XCB_WINDOW_NONE || got == root) {
        fprintf(stderr, "WARN: input focus did not become %u (got %u)\n", expected, got);
    } else {
        fprintf(stderr, "WARN: input focus expected %u, got %u\n", expected, got);
    }
}

static void set_wm_delete_window(xcb_window_t win) {
    xcb_atom_t wm_protocols = get_atom("WM_PROTOCOLS");
    xcb_atom_t wm_delete = get_atom("WM_DELETE_WINDOW");
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, wm_protocols, XCB_ATOM_ATOM, 32, 1, &wm_delete);
    xflush();
}

static void send_wm_delete_window(xcb_window_t win) {
    xcb_atom_t wm_protocols = get_atom("WM_PROTOCOLS");
    xcb_atom_t wm_delete = get_atom("WM_DELETE_WINDOW");

    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = win;
    ev.type = wm_protocols;
    ev.data.data32[0] = wm_delete;
    ev.data.data32[1] = XCB_CURRENT_TIME;

    xcb_send_event(c, 0, win, XCB_EVENT_MASK_NO_EVENT, (const char*)&ev);
    xflush();
}

static void request_net_wm_state_toggle(xcb_window_t win, xcb_atom_t state_atom, uint32_t action) {
    // EWMH _NET_WM_STATE client message to root
    // action: 0 remove, 1 add, 2 toggle
    xcb_atom_t net_wm_state = get_atom("_NET_WM_STATE");

    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = win;
    ev.type = net_wm_state;
    ev.data.data32[0] = action;
    ev.data.data32[1] = state_atom;
    ev.data.data32[2] = XCB_ATOM_NONE;
    ev.data.data32[3] = 1;  // source indication: application
    ev.data.data32[4] = 0;

    xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   (const char*)&ev);
    xflush();
}

static bool window_has_state(xcb_window_t win, xcb_atom_t state_atom) {
    xcb_atom_t net_wm_state = get_atom("_NET_WM_STATE");
    uint32_t nbytes = 0;
    xcb_atom_t* states = (xcb_atom_t*)get_property_any(win, net_wm_state, XCB_ATOM_ATOM, &nbytes);
    if (!states || nbytes < sizeof(xcb_atom_t)) {
        free(states);
        return false;
    }
    size_t n = (size_t)nbytes / sizeof(xcb_atom_t);
    bool ok = atom_in_list(states, n, state_atom);
    free(states);
    return ok;
}

static void test_wm_presence_ewmh(void) {
    printf("Test: WM presence / EWMH sanity\n");

    xcb_atom_t net_supporting = get_atom("_NET_SUPPORTING_WM_CHECK");
    uint32_t nbytes = 0;
    xcb_window_t* v = (xcb_window_t*)get_property_any(root, net_supporting, XCB_ATOM_WINDOW, &nbytes);
    if (!v || nbytes < sizeof(xcb_window_t)) fail("_NET_SUPPORTING_WM_CHECK missing on root");
    xcb_window_t sup = v[0];
    free(v);

    // supporting window should have property pointing to itself
    v = (xcb_window_t*)get_property_any(sup, net_supporting, XCB_ATOM_WINDOW, &nbytes);
    if (!v || nbytes < sizeof(xcb_window_t)) fail("_NET_SUPPORTING_WM_CHECK missing on supporting window");
    if (v[0] != sup) fail("_NET_SUPPORTING_WM_CHECK on supporting window is not self-referential");
    free(v);

    // supporting window should have a name (EWMH suggests _NET_WM_NAME, but allow WM_NAME too)
    xcb_atom_t net_wm_name = get_atom("_NET_WM_NAME");
    xcb_atom_t utf8 = get_atom("UTF8_STRING");
    uint32_t name_bytes = 0;
    char* name = (char*)get_property_any(sup, net_wm_name, utf8, &name_bytes);
    if (!name || name_bytes == 0) {
        // fallback WM_NAME
        xcb_atom_t wm_name = get_atom("WM_NAME");
        name = (char*)get_property_any(sup, wm_name, XCB_ATOM_STRING, &name_bytes);
    }
    if (!name || name_bytes == 0) fail("Supporting window name missing (_NET_WM_NAME/WM_NAME)");
    free(name);

    // _NET_SUPPORTED should exist, but contents depend on your implementation
    xcb_atom_t net_supported = get_atom("_NET_SUPPORTED");
    uint32_t supbytes = 0;
    xcb_atom_t* supported = (xcb_atom_t*)get_property_any(root, net_supported, XCB_ATOM_ATOM, &supbytes);
    if (!supported || supbytes == 0) {
        free(supported);
        fail("_NET_SUPPORTED missing or empty");
    }
    free(supported);

    printf("PASS: WM presence / EWMH sanity\n");
}

static void test_management_and_lists(void) {
    printf("Test: Management + client lists\n");

    xcb_window_t w = create_window(200, 120);
    map_window(w);

    xcb_window_t frame = XCB_WINDOW_NONE;
    require_eventual_reparent(w, 1000, &frame);
    printf("  Window %u reparented under %u\n", w, frame);

    require_eventual_mapped(w, 1000);
    require_eventual_client_list_membership(w, 1000);

    destroy_window(w);

    printf("PASS: Management + client lists\n");
}

static void test_focus_policy(void) {
    printf("Test: Focus (_NET_ACTIVE_WINDOW)\n");

    xcb_window_t w1 = create_window(100, 100);
    map_window(w1);

    require_eventual_reparent(w1, 1000, NULL);
    require_eventual_active_window(w1, 1000);
    require_best_effort_input_focus(w1, 250);

    xcb_window_t w2 = create_window(100, 100);
    map_window(w2);

    require_eventual_reparent(w2, 1000, NULL);
    require_eventual_active_window(w2, 1000);
    require_best_effort_input_focus(w2, 250);

    destroy_window(w2);
    destroy_window(w1);

    printf("PASS: Focus (_NET_ACTIVE_WINDOW)\n");
}

static void test_wm_delete_window(void) {
    printf("Test: WM_DELETE_WINDOW close protocol\n");

    xcb_window_t w = create_window(120, 80);
    set_wm_delete_window(w);
    map_window(w);

    require_eventual_reparent(w, 1000, NULL);
    require_eventual_mapped(w, 1000);

    send_wm_delete_window(w);

    // Expect the window to disappear: either DestroyNotify observed or removed from client list
    xcb_atom_t net_client_list = get_atom("_NET_CLIENT_LIST");
    uint64_t deadline = now_us() + 1500ull * 1000ull;
    bool gone = false;

    while (now_us() < deadline) {
        // event path
        for (;;) {
            xcb_generic_event_t* ev = xcb_poll_for_event(c);
            if (!ev) break;
            uint8_t rt = (ev->response_type & ~0x80);
            if (rt == XCB_DESTROY_NOTIFY) {
                xcb_destroy_notify_event_t* de = (xcb_destroy_notify_event_t*)ev;
                if (de->window == w) gone = true;
            }
            free(ev);
        }

        if (gone) break;

        // property path (more robust across some WMs)
        if (!client_list_contains(net_client_list, w)) {
            gone = true;
            break;
        }

        usleep(2000);
    }

    if (!gone) fail("WM_DELETE_WINDOW did not cause window to close / disappear");
    printf("PASS: WM_DELETE_WINDOW close protocol\n");
}

static void test_fullscreen_state(void) {
    printf("Test: _NET_WM_STATE_FULLSCREEN toggle\n");

    xcb_window_t w = create_window(180, 120);
    map_window(w);
    require_eventual_reparent(w, 1000, NULL);
    require_eventual_mapped(w, 1000);

    xcb_atom_t fs = get_atom("_NET_WM_STATE_FULLSCREEN");

    // request add fullscreen
    request_net_wm_state_toggle(w, fs, 1);

    uint64_t deadline = now_us() + 1500ull * 1000ull;
    bool set = false;
    while (now_us() < deadline) {
        if (window_has_state(w, fs)) {
            set = true;
            break;
        }
        // drain events
        for (int i = 0; i < 16; i++) {
            xcb_generic_event_t* ev = xcb_poll_for_event(c);
            if (!ev) break;
            free(ev);
        }
        usleep(2000);
    }
    if (!set) fail("_NET_WM_STATE_FULLSCREEN was not applied");

    // request remove fullscreen
    request_net_wm_state_toggle(w, fs, 0);

    deadline = now_us() + 1500ull * 1000ull;
    bool cleared = false;
    while (now_us() < deadline) {
        if (!window_has_state(w, fs)) {
            cleared = true;
            break;
        }
        for (int i = 0; i < 16; i++) {
            xcb_generic_event_t* ev = xcb_poll_for_event(c);
            if (!ev) break;
            free(ev);
        }
        usleep(2000);
    }
    if (!cleared) fail("_NET_WM_STATE_FULLSCREEN was not cleared");

    destroy_window(w);

    printf("PASS: _NET_WM_STATE_FULLSCREEN toggle\n");
}

static void test_desktop_props_best_effort(void) {
    printf("Test: Desktop props (best-effort)\n");

    // Don’t hard-fail if you haven’t implemented desktops yet
    // But if present, validate basic shape
    xcb_atom_t ndesks = get_atom("_NET_NUMBER_OF_DESKTOPS");
    xcb_atom_t curdesk = get_atom("_NET_CURRENT_DESKTOP");
    xcb_atom_t names = get_atom("_NET_DESKTOP_NAMES");
    xcb_atom_t utf8 = get_atom("UTF8_STRING");

    uint32_t nb = 0;
    uint32_t* v = (uint32_t*)get_property_any(root, ndesks, XCB_ATOM_CARDINAL, &nb);
    if (v && nb >= 4) {
        if (v[0] == 0) fail("_NET_NUMBER_OF_DESKTOPS present but zero");
    } else {
        fprintf(stderr, "  WARN: _NET_NUMBER_OF_DESKTOPS missing\n");
    }
    free(v);

    nb = 0;
    v = (uint32_t*)get_property_any(root, curdesk, XCB_ATOM_CARDINAL, &nb);
    if (v && nb >= 4) {
        // ok
    } else {
        fprintf(stderr, "  WARN: _NET_CURRENT_DESKTOP missing\n");
    }
    free(v);

    nb = 0;
    char* s = (char*)get_property_any(root, names, utf8, &nb);
    if (s && nb > 0) {
        // names is NUL-separated UTF-8 strings
    } else {
        fprintf(stderr, "  WARN: _NET_DESKTOP_NAMES missing\n");
    }
    free(s);

    printf("PASS: Desktop props (best-effort)\n");
}

int main(void) {
    c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) fail("Cannot connect to X");
    screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    root = screen->root;

    // Make sure we can receive root property changes if needed
    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY};
    xcb_change_window_attributes(c, root, mask, values);
    xflush();

    test_wm_presence_ewmh();
    test_management_and_lists();
    test_focus_policy();
    test_wm_delete_window();
    test_fullscreen_state();
    test_desktop_props_best_effort();

    xcb_disconnect(c);
    return 0;
}
