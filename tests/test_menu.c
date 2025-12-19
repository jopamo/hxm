#include <assert.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bbox.h"
#include "client.h"
#include "event.h"
#include "menu.h"
#include "wm.h"

volatile sig_atomic_t g_reload_pending = 0;

void setup_server(server_t* s) {
    memset(s, 0, sizeof(server_t));
    s->is_test = true;
    s->root_depth = 24;
    s->root_visual_type = xcb_get_visualtype(NULL, 0);
    s->conn = xcb_connect(NULL, NULL);
    s->keysyms = xcb_key_symbols_alloc(s->conn);
    slotmap_init(&s->clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    s->desktop_count = 4;
    s->current_desktop = 0;

    config_init_defaults(&s->config);

    // Menu init happens in server_init usually, but we call it manually
    menu_init(s);
}

void test_menu_basics() {
    server_t s;
    setup_server(&s);

    // 1. Initial state
    assert(s.menu.visible == false);
    assert(s.menu.items.length == 0);

    // 2. Show menu
    menu_show(&s, 100, 100);
    assert(s.menu.items.length ==
           24);  // Corrected for actual number of items (14 apps, 3 separators, 2 prefs, 4 monitors, 1 exit)
    assert(s.menu.visible == true);
    assert(s.menu.x == 100);
    assert(s.menu.y == 100);
    assert(s.menu.selected_index == -1);

    // 3. Motion (Hover over first item)
    // Item 0 is at y = MENU_PADDING (4) to 24.
    // Window relative coordinates: 10, 10
    // Global coordinates: 110, 110
    menu_handle_pointer_motion(&s, 110, 110);

    // (110 - 100) = 10. (110 - 100) = 10.
    // 10 - 4 = 6. 6 / 20 = 0.
    assert(s.menu.selected_index == 0);  // Should be selected

    // 4. Motion (Hover outside)
    menu_handle_pointer_motion(&s, 0, 0);
    assert(s.menu.selected_index == -1);

    // 5. Hide menu
    menu_hide(&s);
    assert(s.menu.visible == false);

    printf("test_menu_basics passed\n");

    menu_destroy(&s);
    config_destroy(&s.config);
    for (uint32_t i = 1; i < s.clients.cap; i++) {
        if (s.clients.hdr[i].live) {
            handle_t h = handle_make(i, s.clients.hdr[i].gen);
            client_hot_t* hot = server_chot(&s, h);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    slotmap_destroy(&s.clients);
    xcb_key_symbols_free(s.keysyms);
    xcb_disconnect(s.conn);

    pango_cairo_font_map_set_default(NULL);
    FcFini();
}

void test_menu_esc() {
    server_t s;
    setup_server(&s);

    menu_show(&s, 100, 100);
    assert(s.menu.visible == true);

    // Simulate Escape key press
    xcb_key_press_event_t ev = {0};
    ev.detail = 9;  // Usually Escape keycode

    // We need to mock xcb_key_symbols_get_keysym to return XK_Escape
    // But since we are using stubs, let's just call wm_handle_key_press with a mock server that has keysyms set up?
    // Actually, xcb_stubs.c has a mock for xcb_key_symbols_get_keysym.

    // Let's check xcb_stubs.c's xcb_key_symbols_get_keysym.

    wm_handle_key_press(&s, &ev);
    assert(s.menu.visible == false);

    printf("test_menu_esc passed\n");
    menu_destroy(&s);
    config_destroy(&s.config);
    for (uint32_t i = 1; i < s.clients.cap; i++) {
        if (s.clients.hdr[i].live) {
            handle_t h = handle_make(i, s.clients.hdr[i].gen);
            client_hot_t* hot = server_chot(&s, h);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    slotmap_destroy(&s.clients);
    xcb_key_symbols_free(s.keysyms);
    xcb_disconnect(s.conn);

    pango_cairo_font_map_set_default(NULL);
    FcFini();
}

int main() {
    test_menu_basics();
    test_menu_esc();
    return 0;
}
