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

// Externs from xcb_stubs.c
extern void xcb_stubs_reset(void);

static void setup_server(server_t* s) {
    memset(s, 0, sizeof(server_t));
    s->is_test = true;

    xcb_stubs_reset();
    s->conn = xcb_connect(NULL, NULL);
    
    // Minimal init for focus tests
    slotmap_init(&s->clients, 128, sizeof(client_hot_t), sizeof(client_cold_t));
    list_init(&s->focus_history);
    s->desktop_count = 4;
    s->current_desktop = 0;
}

static void teardown_server(server_t* s) {
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (s->clients.hdr[i].live) {
            handle_t h = handle_make(i, s->clients.hdr[i].gen);
            client_hot_t* hot = server_chot(s, h);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    slotmap_destroy(&s->clients);
    xcb_disconnect(s->conn);
}

void test_should_focus_on_map(void) {
    client_hot_t hot = {0};
    render_init(&hot.render_ctx);
    hot.icon_surface = NULL;
    hot.focus_override = -1;

    // Default window type is NORMAL (0), transient_for HANDLE_INVALID
    hot.type = WINDOW_TYPE_NORMAL;
    hot.transient_for = HANDLE_INVALID;
    bool result = should_focus_on_map(&hot);
    assert(result == false);

    // Dialog window should focus
    hot.type = WINDOW_TYPE_DIALOG;
    assert(should_focus_on_map(&hot) == true);

    // Transient window should focus even if type is NORMAL
    hot.type = WINDOW_TYPE_NORMAL;
    hot.transient_for = handle_make(1, 0);
    assert(should_focus_on_map(&hot) == true);

    // Types that should never focus on map, even if transient.
    const window_type_t no_focus_types[] = {
        WINDOW_TYPE_DOCK,    WINDOW_TYPE_NOTIFICATION,  WINDOW_TYPE_DESKTOP,
        WINDOW_TYPE_MENU,    WINDOW_TYPE_DROPDOWN_MENU, WINDOW_TYPE_POPUP_MENU,
        WINDOW_TYPE_TOOLTIP, WINDOW_TYPE_COMBO,         WINDOW_TYPE_DND,
    };
    for (size_t i = 0; i < sizeof(no_focus_types) / sizeof(no_focus_types[0]); i++) {
        hot.type = no_focus_types[i];
        hot.transient_for = handle_make(2, 0);
        assert(should_focus_on_map(&hot) == false);
    }

    // Focus override forces behavior.
    hot.type = WINDOW_TYPE_DIALOG;
    hot.transient_for = HANDLE_INVALID;
    hot.focus_override = 0;
    assert(should_focus_on_map(&hot) == false);
    hot.focus_override = 1;
    assert(should_focus_on_map(&hot) == true);
    hot.focus_override = -1;

    printf("test_should_focus_on_map passed\n");
    render_free(&hot.render_ctx);
    if (hot.icon_surface) cairo_surface_destroy(hot.icon_surface);
}

void test_debug_dump_focus_history_guard(void) {
    server_t s;
    setup_server(&s);

    // Create 70 clients to trigger the loop guard (64)
    const int CLIENT_COUNT = 70;
    handle_t handles[CLIENT_COUNT];

    for (int i = 0; i < CLIENT_COUNT; i++) {
        handles[i] = slotmap_alloc(&s.clients, NULL, NULL);
        client_hot_t* c = server_chot(&s, handles[i]);
        render_init(&c->render_ctx);
        c->state = STATE_MAPPED;
        c->desktop = 0;
        c->frame = 1000 + i;
        c->xid = 2000 + i;
        c->self = handles[i];
        // Initialize focus_node
        c->focus_node.prev = &c->focus_node;
        c->focus_node.next = &c->focus_node;
    }

    // Focus them one by one. This will insert them into s.focus_history.
    // wm_set_focus calls debug_dump_focus_history internally if debug logging is enabled.
    for (int i = 0; i < CLIENT_COUNT; i++) {
        wm_set_focus(&s, handles[i]);
    }

    // Verify list size roughly (sanity check)
    int count = 0;
    list_node_t* node = s.focus_history.next;
    while (node != &s.focus_history) {
        count++;
        node = node->next;
    }
    assert(count == CLIENT_COUNT);

    printf("test_debug_dump_focus_history_guard passed (check logs for WARN if needed)\n");
    teardown_server(&s);
}

void test_focus_on_map_integration(void) {
    // Integration test: simulate client_finish_manage with a normal window
    // and verify focus not set.
    // This is more complex and requires full server setup.
    // TODO: implement if needed.
}

int main(void) {
    test_should_focus_on_map();
    test_debug_dump_focus_history_guard();
    return 0;
}
