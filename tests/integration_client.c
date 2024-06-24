#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

xcb_connection_t* c;
xcb_screen_t* screen;
xcb_window_t root;

void fail(const char* msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

xcb_atom_t get_atom(const char* name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(c, 0, strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(c, cookie, NULL);
    if (!reply) fail("Failed to intern atom");
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

xcb_window_t create_window() {
    xcb_window_t w = xcb_generate_id(c);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, root, 0, 0, 100, 100, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, values);
    return w;
}

void test_management() {
    printf("Test: Management\n");
    xcb_window_t w = create_window();
    xcb_map_window(c, w);
    xcb_flush(c);

    // Wait for ReparentNotify
    bool reparented = false;
    for (int i = 0; i < 50; i++) {
        xcb_generic_event_t* ev = xcb_poll_for_event(c);
        if (ev) {
            if ((ev->response_type & ~0x80) == XCB_REPARENT_NOTIFY) {
                xcb_reparent_notify_event_t* re = (xcb_reparent_notify_event_t*)ev;
                if (re->window == w && re->parent != root) {
                    reparented = true;
                    printf("  Window %u reparented to %u\n", w, re->parent);
                }
            }
            free(ev);
        }
        if (reparented) break;
        usleep(10000);  // 10ms
    }

    if (!reparented) fail("Window was not reparented (not managed?)");
    printf("PASS: Management\n");
}

void test_focus() {
    printf("Test: Focus\n");
    xcb_window_t w1 = create_window();
    xcb_map_window(c, w1);
    xcb_flush(c);

    // Wait for management of w1
    usleep(100000);

    xcb_atom_t net_active = get_atom("_NET_ACTIVE_WINDOW");

    // Check if w1 is active (new windows get focus)
    xcb_get_property_cookie_t cookie = xcb_get_property(c, 0, root, net_active, XCB_ATOM_WINDOW, 0, 1);
    xcb_get_property_reply_t* reply = xcb_get_property_reply(c, cookie, NULL);
    if (!reply) fail("Failed to get _NET_ACTIVE_WINDOW");

    if (xcb_get_property_value_length(reply) == 0) fail("_NET_ACTIVE_WINDOW not set");
    xcb_window_t active = *(xcb_window_t*)xcb_get_property_value(reply);
    free(reply);

    if (active != w1) {
        printf("Expected active %u, got %u\n", w1, active);
        fail("New window w1 did not get focus");
    }

    xcb_window_t w2 = create_window();
    xcb_map_window(c, w2);
    xcb_flush(c);
    usleep(100000);

    cookie = xcb_get_property(c, 0, root, net_active, XCB_ATOM_WINDOW, 0, 1);
    reply = xcb_get_property_reply(c, cookie, NULL);
    active = *(xcb_window_t*)xcb_get_property_value(reply);
    free(reply);

    if (active != w2) {
        printf("Expected active %u, got %u\n", w2, active);
        fail("New window w2 did not get focus");
    }
    printf("PASS: Focus\n");
}

int main() {
    c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) fail("Cannot connect to X");
    screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    root = screen->root;

    test_management();
    test_focus();
    // test_stacking(); // Harder to test without query tree logic logic, add later

    xcb_disconnect(c);
    return 0;
}
