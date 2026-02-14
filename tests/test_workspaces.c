#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "ds.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"

// Externs from xcb_stubs.c
extern void xcb_stubs_reset(void);
extern int stub_map_window_count;
extern int stub_unmap_window_count;
extern xcb_window_t stub_last_mapped_window;
extern xcb_window_t stub_last_unmapped_window;

void setup_server(server_t* s) {
  memset(s, 0, sizeof(server_t));
  s->is_test = true;

  xcb_stubs_reset();
  s->conn = xcb_connect(NULL, NULL);
  atoms_init(s->conn);

  s->root_depth = 24;
  s->root_visual_type = xcb_get_visualtype(s->conn, 0);
  slotmap_init(&s->clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
  small_vec_init(&s->active_clients);

  // Initialize workspace defaults
  s->desktop_count = 4;
  s->current_desktop = 0;

  list_init(&s->focus_history);
}

void test_workspace_switch_basics(void) {
  server_t s;
  setup_server(&s);

  // Create 3 clients
  // Client 1: Desktop 0 (Current)
  // Client 2: Desktop 1
  // Client 3: Desktop 0, but minimized (UNMAPPED state) - wait, if unmapped, we
  // shouldn't map it. Client 4: Sticky (Desktop -1)

  handle_t h1 = slotmap_alloc(&s.clients, NULL, NULL);
  small_vec_push(&s.active_clients, handle_to_ptr(h1));
  client_hot_t* c1 = server_chot(&s, h1);
  c1->state = STATE_MAPPED;
  c1->desktop = 0;
  c1->frame = 1001;

  handle_t h2 = slotmap_alloc(&s.clients, NULL, NULL);
  small_vec_push(&s.active_clients, handle_to_ptr(h2));
  client_hot_t* c2 = server_chot(&s, h2);
  c2->state = STATE_MAPPED;
  c2->desktop = 1;
  c2->frame = 1002;

  handle_t h3 = slotmap_alloc(&s.clients, NULL, NULL);
  small_vec_push(&s.active_clients, handle_to_ptr(h3));
  client_hot_t* c3 = server_chot(&s, h3);
  c3->state = STATE_UNMAPPED;  // Minimized
  c3->desktop = 0;
  c3->frame = 1003;

  handle_t h4 = slotmap_alloc(&s.clients, NULL, NULL);
  small_vec_push(&s.active_clients, handle_to_ptr(h4));
  client_hot_t* c4 = server_chot(&s, h4);
  c4->state = STATE_MAPPED;
  c4->desktop = 0;  // Will be sticky
  c4->sticky = true;
  c4->frame = 1004;

  // Reset counters
  stub_map_window_count = 0;
  stub_unmap_window_count = 0;

  // Switch to desktop 1
  printf("Switching to desktop 1...\n");
  wm_switch_workspace(&s, 1);
  wm_flush_dirty(&s, monotonic_time_ns());

  // Expectations:
  // c1 (Desk 0): Should be unmapped
  // c2 (Desk 1): Should be mapped
  // c3 (Desk 0, Unmapped): Should stay unmapped (no action)
  // c4 (Sticky): Should stay mapped (no action, or unmap/map?) -> Ideally no
  // action.

  // Check server state
  assert(s.current_desktop == 1);

  // We can't easily check exact sequence without a log, but we can check last
  // calls or counts. Ideally we check if unmap was called for c1 and map for
  // c2.

  // Let's assume the implementation optimizes and doesn't map already mapped
  // windows? But here c2 is "STATE_MAPPED" in our model, but physically it
  // might be unmapped if we were on desktop 0. Wait, in this test setup, we
  // just created them. We didn't simulate previous state. If we assume we
  // started at Desktop 0: c1 was visible. c2 was hidden. So c1 should be
  // unmapped. c2 should be mapped.

  // But `wm_switch_workspace` logic usually is:
  // For all clients:
  //   If should_be_visible(client, new_desktop):
  //      xcb_map_window(client.frame)
  //   Else:
  //      xcb_unmap_window(client.frame)

  // If we do this blindly, we map/unmap everything.
  // Optimizations come later (checking if already mapped).

  // So for now, expect:
  // c1: Unmap
  // c2: Map
  // c3: Stay Unmapped (it is STATE_UNMAPPED)
  // c4: Map (Sticky) - or Stay Mapped.

  // Let's implement wm_switch_workspace to be smart enough not to map
  // STATE_UNMAPPED clients.

  // Also need to link wm_switch_workspace.

  // Assertions
  // C1 (Desk 0) -> Unmap
  // C2 (Desk 1) -> Map
  // C3 (Desk 0, Unmapped) -> Ignore
  // C4 (Sticky) -> Map (redundant but correct for visibility)

  printf("Map count: %d, Unmap count: %d\n", stub_map_window_count, stub_unmap_window_count);

  assert(stub_unmap_window_count == 1);  // C1
  assert(stub_map_window_count == 2);    // C2, C4

  assert(stub_last_unmapped_window == 1001);  // C1's frame
  // Last mapped could be C2 or C4 depending on iteration order (likely index
  // order). C2 is index 2, C4 is index 4. So C4 is last.
  assert(stub_last_mapped_window == 1004);

  printf("Test finished successfully.\n");

  for (uint32_t i = 1; i < s.clients.cap; i++) {
    if (s.clients.hdr[i].live) {
      handle_t h = handle_make(i, s.clients.hdr[i].gen);
      client_hot_t* hot = server_chot(&s, h);
      if (hot) {
        render_free(&hot->render_ctx);
        if (hot->icon_surface)
          cairo_surface_destroy(hot->icon_surface);
      }
    }
  }
  slotmap_destroy(&s.clients);
  small_vec_destroy(&s.active_clients);
  xcb_disconnect(s.conn);
}

void test_client_move_to_workspace(void) {
  server_t s;
  setup_server(&s);

  handle_t h1 = slotmap_alloc(&s.clients, NULL, NULL);
  small_vec_push(&s.active_clients, handle_to_ptr(h1));
  client_hot_t* c1 = server_chot(&s, h1);
  c1->state = STATE_MAPPED;
  c1->desktop = 0;
  c1->frame = 1001;
  c1->xid = 2001;
  c1->self = h1;

  stub_map_window_count = 0;
  stub_unmap_window_count = 0;

  // Move c1 to desktop 1 (currently on desktop 0)
  printf("Moving c1 to desktop 1 (no follow)...\n");
  wm_client_move_to_workspace(&s, h1, 1, false);
  wm_flush_dirty(&s, monotonic_time_ns());

  assert(c1->desktop == 1);
  assert(c1->sticky == false);
  assert(stub_unmap_window_count == 1);
  assert(stub_last_unmapped_window == 1001);

  // Focus handling: if h1 was focused, it should lose focus if it moved to a
  // hidden desktop
  s.focused_client = h1;
  // Move it back to 0
  printf("Moving c1 back to desktop 0 while focused (no follow)...\n");
  wm_client_move_to_workspace(&s, h1, 0, false);
  wm_flush_dirty(&s, monotonic_time_ns());
  assert(c1->desktop == 0);
  assert(stub_map_window_count == 1);
  assert(stub_last_mapped_window == 1001);
  assert(s.focused_client == h1);  // Should stay focused because it's on current desktop

  // Move it to 2
  printf("Moving c1 to desktop 2 while focused (no follow)...\n");
  wm_client_move_to_workspace(&s, h1, 2, false);
  wm_flush_dirty(&s, monotonic_time_ns());
  assert(c1->desktop == 2);
  assert(s.focused_client == HANDLE_INVALID);  // Should lose focus

  // Test follow
  printf("Moving c1 back to desktop 0 (follow)...\n");
  wm_client_move_to_workspace(&s, h1, 0, true);
  wm_flush_dirty(&s, monotonic_time_ns());
  assert(c1->desktop == 0);
  assert(s.current_desktop == 0);
  assert(s.focused_client == h1);

  printf("Moving c1 to desktop 1 (follow)...\n");
  wm_client_move_to_workspace(&s, h1, 1, true);
  wm_flush_dirty(&s, monotonic_time_ns());
  assert(c1->desktop == 1);
  assert(s.current_desktop == 1);
  assert(s.focused_client == h1);

  printf("test_client_move_to_workspace passed.\n");
  for (uint32_t i = 1; i < s.clients.cap; i++) {
    if (s.clients.hdr[i].live) {
      handle_t h = handle_make(i, s.clients.hdr[i].gen);
      client_hot_t* hot = server_chot(&s, h);
      if (hot) {
        render_free(&hot->render_ctx);
        if (hot->icon_surface)
          cairo_surface_destroy(hot->icon_surface);
      }
    }
  }
  arena_destroy(&s.tick_arena);
  slotmap_destroy(&s.clients);
  small_vec_destroy(&s.active_clients);
  xcb_disconnect(s.conn);
}

void test_client_toggle_sticky(void) {
  server_t s;
  setup_server(&s);

  handle_t h1 = slotmap_alloc(&s.clients, NULL, NULL);
  small_vec_push(&s.active_clients, handle_to_ptr(h1));
  client_hot_t* c1 = server_chot(&s, h1);
  c1->state = STATE_MAPPED;
  c1->desktop = 1;  // On hidden desktop
  c1->frame = 1001;
  c1->self = h1;

  stub_map_window_count = 0;
  stub_unmap_window_count = 0;

  // Toggle sticky (should become visible because sticky)
  printf("Toggling sticky for c1 (on desktop 1, currently at 0)...\n");
  wm_client_toggle_sticky(&s, h1);
  wm_flush_dirty(&s, monotonic_time_ns());
  assert(c1->sticky == true);
  assert(stub_map_window_count == 1);
  assert(stub_last_mapped_window == 1001);

  // Toggle back
  printf("Toggling sticky off for c1...\n");
  wm_client_toggle_sticky(&s, h1);
  wm_flush_dirty(&s, monotonic_time_ns());
  assert(c1->sticky == false);
  assert(stub_unmap_window_count == 1);
  assert(stub_last_unmapped_window == 1001);

  printf("test_client_toggle_sticky passed.\n");
  for (uint32_t i = 1; i < s.clients.cap; i++) {
    if (s.clients.hdr[i].live) {
      handle_t h = handle_make(i, s.clients.hdr[i].gen);
      client_hot_t* hot = server_chot(&s, h);
      if (hot) {
        render_free(&hot->render_ctx);
        if (hot->icon_surface)
          cairo_surface_destroy(hot->icon_surface);
      }
    }
  }
  slotmap_destroy(&s.clients);
  small_vec_destroy(&s.active_clients);
  xcb_disconnect(s.conn);
}

void test_workspace_relative(void) {
  server_t s;
  setup_server(&s);

  s.current_desktop = 1;
  s.desktop_count = 4;

  printf("Switching relative +1 from 1...\n");
  wm_switch_workspace_relative(&s, 1);
  assert(s.current_desktop == 2);

  printf("Switching relative +1 from 2...\n");
  wm_switch_workspace_relative(&s, 1);
  assert(s.current_desktop == 3);

  printf("Switching relative +1 from 3 (wrap)...\n");
  wm_switch_workspace_relative(&s, 1);
  assert(s.current_desktop == 0);

  printf("Switching relative -1 from 0 (wrap)...\n");
  wm_switch_workspace_relative(&s, -1);
  assert(s.current_desktop == 3);

  printf("test_workspace_relative passed.\n");
  for (uint32_t i = 1; i < s.clients.cap; i++) {
    if (s.clients.hdr[i].live) {
      handle_t h = handle_make(i, s.clients.hdr[i].gen);
      client_hot_t* hot = server_chot(&s, h);
      if (hot) {
        render_free(&hot->render_ctx);
        if (hot->icon_surface)
          cairo_surface_destroy(hot->icon_surface);
      }
    }
  }
  slotmap_destroy(&s.clients);
  small_vec_destroy(&s.active_clients);
  xcb_disconnect(s.conn);
}

void test_sticky_panel_ignores_workspace_move(void) {
  server_t s;
  setup_server(&s);

  handle_t h = slotmap_alloc(&s.clients, NULL, NULL);
  small_vec_push(&s.active_clients, handle_to_ptr(h));
  client_hot_t* c = server_chot(&s, h);
  c->state = STATE_MAPPED;
  c->desktop = 0;
  c->sticky = true;
  c->type = WINDOW_TYPE_DOCK;
  c->frame = 2001;

  wm_client_move_to_workspace(&s, h, 2, false);

  assert(c->sticky == true);
  assert(c->desktop == -1);

  printf("test_sticky_panel_ignores_workspace_move passed\n");

  slotmap_destroy(&s.clients);
  small_vec_destroy(&s.active_clients);
  xcb_disconnect(s.conn);
}

int main(void) {
  test_workspace_switch_basics();
  test_client_move_to_workspace();
  test_client_toggle_sticky();
  test_sticky_panel_ignores_workspace_move();
  test_workspace_relative();
  printf("All tests passed!\n");
  return 0;
}
