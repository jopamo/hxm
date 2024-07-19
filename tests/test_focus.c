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

void test_should_focus_on_map() {
    client_hot_t hot = {0};
    render_init(&hot.render_ctx);
    hot.icon_surface = NULL;
    hot.focus_override = -1;

    // Default window type is NORMAL (0), transient_for HANDLE_INVALID
    hot.type = WINDOW_TYPE_NORMAL;
    hot.transient_for = HANDLE_INVALID;
    printf("debug: type=%u, transient_for=%lu, HANDLE_INVALID=%lu\n", hot.type, hot.transient_for,
           (uint64_t)HANDLE_INVALID);
    bool result = should_focus_on_map(&hot);
    printf("result=%d\n", result);
    assert(result == false);

    // Dialog window should focus
    hot.type = WINDOW_TYPE_DIALOG;
    assert(should_focus_on_map(&hot) == true);

    // Transient window should focus even if type is NORMAL
    hot.type = WINDOW_TYPE_NORMAL;
    hot.transient_for = handle_make(1, 0);
    assert(should_focus_on_map(&hot) == true);

    // Dock window should not focus (even if transient)
    hot.type = WINDOW_TYPE_DOCK;
    assert(should_focus_on_map(&hot) == false);

    // Notification window should not focus
    hot.type = WINDOW_TYPE_NOTIFICATION;
    hot.transient_for = HANDLE_INVALID;
    assert(should_focus_on_map(&hot) == false);

    // Desktop window should not focus
    hot.type = WINDOW_TYPE_DESKTOP;
    assert(should_focus_on_map(&hot) == false);

    printf("test_should_focus_on_map passed\n");
    render_free(&hot.render_ctx);
    if (hot.icon_surface) cairo_surface_destroy(hot.icon_surface);
}

void test_focus_on_map_integration() {
    // Integration test: simulate client_finish_manage with a normal window
    // and verify focus not set.
    // This is more complex and requires full server setup.
    // TODO: implement if needed.
}

int main() {
    test_should_focus_on_map();
    return 0;
}