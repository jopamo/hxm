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
