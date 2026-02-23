#include <assert.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "event.h"
#include "hxm.h"
#include "menu.h"
#include "wm.h"

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

  // Load default menu config from source tree (tests run from build dir)
  const char* candidates[] = {"data/menu.conf", "../data/menu.conf"};
  bool loaded = false;
  for (size_t i = 0; i < HXM_ARRAY_LEN(candidates); i++) {
    if (access(candidates[i], R_OK) == 0 && menu_load_config(s, candidates[i])) {
      loaded = true;
      break;
    }
  }
  assert(loaded && "menu.conf not found for test");
}

static void teardown_server(server_t* s) {
  menu_destroy(s);
  config_destroy(&s->config);
  for (uint32_t i = 1; i < s->clients.cap; i++) {
    if (s->clients.hdr[i].live) {
      handle_t h = handle_make(i, s->clients.hdr[i].gen);
      client_hot_t* hot = server_chot(s, h);
      if (hot) {
        render_free(&hot->render_ctx);
        if (hot->icon_surface)
          cairo_surface_destroy(hot->icon_surface);
      }
    }
  }
  slotmap_destroy(&s->clients);
  xcb_key_symbols_free(s->keysyms);
  xcb_disconnect(s->conn);
}

void test_menu_basics(void) {
  server_t s;
  setup_server(&s);

  // 1. Initial state
  assert(s.menu.visible == false);
  assert(s.menu.items.length == 0);

  // 2. Show menu
  menu_show(&s, 100, 100);
  assert(s.menu.items.length == 25);  // Matches data/menu.conf (15 apps, 3
                                      // separators, 2 prefs, 4 monitors, 1 exit)
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
  teardown_server(&s);
}

void test_menu_esc(void) {
  server_t s;
  setup_server(&s);

  menu_show(&s, 100, 100);
  assert(s.menu.visible == true);

  // Simulate Escape key press
  xcb_key_press_event_t ev = {0};
  ev.detail = 9;  // Usually Escape keycode

  // We need to mock xcb_key_symbols_get_keysym to return XK_Escape
  // But since we are using stubs, let's just call wm_handle_key_press with a
  // mock server that has keysyms set up? Actually, xcb_stubs.c has a mock for
  // xcb_key_symbols_get_keysym.

  // Let's check xcb_stubs.c's xcb_key_symbols_get_keysym.

  wm_handle_key_press(&s, &ev);
  assert(s.menu.visible == false);

  printf("test_menu_esc passed\n");
  teardown_server(&s);
}

void test_menu_right_click_keeps_menu_visible(void) {
  server_t s;
  setup_server(&s);

  menu_show(&s, 100, 100);
  assert(s.menu.visible == true);

  // Right-click outside should not dismiss the menu.
  xcb_button_press_event_t press_outside = {0};
  press_outside.detail = 3;
  press_outside.root_x = 0;
  press_outside.root_y = 0;
  menu_handle_button_press(&s, &press_outside);
  assert(s.menu.visible == true);

  xcb_button_release_event_t release_outside = {0};
  release_outside.detail = 3;
  release_outside.root_x = 0;
  release_outside.root_y = 0;
  menu_handle_button_release(&s, &release_outside);
  assert(s.menu.visible == true);

  // Right-click release over an item should not activate/dismiss it.
  xcb_button_release_event_t release_inside_item = {0};
  release_inside_item.detail = 3;
  release_inside_item.root_x = 110;
  release_inside_item.root_y = 110;
  menu_handle_button_release(&s, &release_inside_item);
  assert(s.menu.visible == true);

  // Left-click outside still dismisses.
  xcb_button_press_event_t left_press_outside = {0};
  left_press_outside.detail = 1;
  left_press_outside.root_x = 0;
  left_press_outside.root_y = 0;
  menu_handle_button_press(&s, &left_press_outside);
  assert(s.menu.visible == false);

  printf("test_menu_right_click_keeps_menu_visible passed\n");
  teardown_server(&s);
}

int main(void) {
  test_menu_basics();
  test_menu_esc();
  test_menu_right_click_keeps_menu_visible();

  /*
   * Release shared font-map/fontconfig globals once after all menu tests.
   * This keeps sanitizer leak checks stable across libc/fontconfig variants.
   */
  pango_cairo_font_map_set_default(NULL);
  FcFini();

  return 0;
}
