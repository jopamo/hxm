#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

// External stubs counters/variables
extern xcb_window_t stub_last_prop_window;
extern xcb_atom_t stub_last_prop_atom;
extern xcb_atom_t stub_last_prop_type;
extern uint32_t stub_last_prop_len;
extern uint8_t stub_last_prop_data[1024];

void test_frame_extents(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t *)malloc(1);
  arena_init(&s.tick_arena, 4096);

  // Init config
  config_init_defaults(&s.config);
  s.config.theme.border_width = 5;
  s.config.theme.title_height = 20;

  // Init atoms
  atoms._NET_FRAME_EXTENTS = 200;

  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t),
                    sizeof(client_cold_t)))
    return;
  small_vec_init(&s.active_clients);

  // Manually setup a client to simulate what client_finish_manage does,
  // but calling wm_flush_dirty is hard because it relies on the loop and hash
  // maps. Instead, I'll invoke client_finish_manage logic partially or simulate
  // the property set. Actually, I modified client_finish_manage to call
  // xcb_change_property. So I can just call client_finish_manage if I setup
  // enough context (hash maps).

  hash_map_init(&s.window_to_client);
  hash_map_init(&s.frame_to_client);
  list_init(&s.focus_history);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_init(&s.layers[i]);

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  small_vec_push(&s.active_clients, handle_to_ptr(h));
  client_hot_t *hot = (client_hot_t *)hot_ptr;
  hot->self = h;
  hot->xid = 123;
  hot->state = STATE_NEW;
  hot->desired.x = 0;
  hot->desired.y = 0;
  hot->desired.w = 100;
  hot->desired.h = 100;
  hot->stacking_index = -1;
  hot->stacking_layer = -1;
  list_init(&hot->transient_sibling);
  list_init(&hot->transients_head);
  list_init(&hot->focus_node);
  hot->visual_id = 0;

  // We want to capture the property change for _NET_FRAME_EXTENTS
  // Reset stub variables
  stub_last_prop_atom = 0;

  // Call client_finish_manage
  // Note: this will do many things including setting properties.
  // We hope _NET_FRAME_EXTENTS is set.
  // The order matters. It sets _NET_FRAME_EXTENTS then _NET_WM_ALLOWED_ACTIONS
  // then _NET_WM_DESKTOP... But since xcb_change_property overwrites the stub
  // variables, we might miss it if subsequent calls happen. Wait,
  // client_finish_manage sets many properties. The stub only stores the LAST
  // one. _NET_WM_DESKTOP is set near the end.

  // To verify _NET_FRAME_EXTENTS, I should probably call wm_flush_dirty with
  // DIRTY_GEOM, because that sets _NET_FRAME_EXTENTS and returns. But
  // wm_flush_dirty checks for dirty flags.

  hot->state = STATE_MAPPED;
  hot->frame = 456;
  hot->dirty = DIRTY_GEOM;

  // Clear last prop
  stub_last_prop_atom = 0;

  wm_flush_dirty(&s, monotonic_time_ns());

  // Now check if _NET_FRAME_EXTENTS was set
  // It is set inside the DIRTY_GEOM block.
  // But wait, xcb_configure_window is also called.
  // And DIRTY_GEOM does NOT trigger other property changes in the same block
  // usually. So if I only set DIRTY_GEOM, it should be the last property change
  // in that block? Actually xcb_configure_window is not a property change. So
  // _NET_FRAME_EXTENTS should be the last property change if I only set
  // DIRTY_GEOM.

  if (stub_last_prop_atom == atoms._NET_FRAME_EXTENTS) {
    assert(stub_last_prop_window == 123);
    assert(stub_last_prop_type == XCB_ATOM_CARDINAL);
    assert(stub_last_prop_len == 4);
    uint32_t *extents = (uint32_t *)stub_last_prop_data;
    uint16_t bw = s.config.theme.border_width;
    uint16_t hh = s.config.theme.handle_height;
    uint16_t bottom = (hh > bw) ? hh : bw;
    // bw=5, th=20 -> {5, 5, 25, bottom}
    assert(extents[0] == 5);      // left
    assert(extents[1] == 5);      // right
    assert(extents[2] == 25);     // top
    assert(extents[3] == bottom); // bottom
    printf("test_frame_extents passed\n");
  } else {
    printf("test_frame_extents failed: Expected atom %u, got %u\n",
           atoms._NET_FRAME_EXTENTS, stub_last_prop_atom);
    assert(0);
  }

  // Cleanup
  render_free(&hot->render_ctx);
  if (hot->icon_surface)
    cairo_surface_destroy(hot->icon_surface);
  slotmap_destroy(&s.clients);
  small_vec_destroy(&s.active_clients);
  hash_map_destroy(&s.window_to_client);
  hash_map_destroy(&s.frame_to_client);
  config_destroy(&s.config);
  arena_destroy(&s.tick_arena);
  free(s.conn);
}

void test_allowed_actions(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t *)malloc(1);
  arena_init(&s.tick_arena, 4096);

  atoms._NET_WM_ALLOWED_ACTIONS = 300;
  atoms._NET_WM_ACTION_MOVE = 301;
  atoms._NET_WM_ACTION_RESIZE = 302;
  atoms._NET_WM_STATE = 400;

  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t),
                    sizeof(client_cold_t)))
    return;
  small_vec_init(&s.active_clients);

  hash_map_init(&s.window_to_client);
  hash_map_init(&s.frame_to_client);

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  small_vec_push(&s.active_clients, handle_to_ptr(h));
  client_hot_t *hot = (client_hot_t *)hot_ptr;
  hot->self = h;
  hot->xid = 123;
  hot->state = STATE_MAPPED;
  hot->frame = 456;

  // Case 1: Resizable window
  hot->hints.min_w = 0;
  hot->hints.max_w = 0; // unlimited
  hot->dirty = DIRTY_STATE;

  // wm_flush_dirty will set WM_STATE and ALLOWED_ACTIONS.
  // ALLOWED_ACTIONS is set LAST in the block.

  stub_last_prop_atom = 0;
  wm_flush_dirty(&s, monotonic_time_ns());

  assert(stub_last_prop_atom == atoms._NET_WM_ALLOWED_ACTIONS);
  assert(stub_last_prop_window == 123);

  // Verify atoms presence
  // We can't easily iterate the raw data without casting, but we know it's
  // atoms (uint32_t)
  uint32_t *acts = (uint32_t *)stub_last_prop_data;
  bool has_move = false;
  bool has_resize = false;
  for (uint32_t i = 0; i < stub_last_prop_len; i++) {
    if (acts[i] == atoms._NET_WM_ACTION_MOVE)
      has_move = true;
    if (acts[i] == atoms._NET_WM_ACTION_RESIZE)
      has_resize = true;
  }
  assert(has_move);
  assert(has_resize);

  // Case 2: Fixed size window
  hot->hints.min_w = 100;
  hot->hints.max_w = 100;
  hot->hints.min_h = 100;
  hot->hints.max_h = 100;
  hot->dirty = DIRTY_STATE;

  stub_last_prop_atom = 0;
  wm_flush_dirty(&s, monotonic_time_ns());

  assert(stub_last_prop_atom == atoms._NET_WM_ALLOWED_ACTIONS);

  acts = (uint32_t *)stub_last_prop_data;
  has_move = false;
  has_resize = false;
  for (uint32_t i = 0; i < stub_last_prop_len; i++) {
    if (acts[i] == atoms._NET_WM_ACTION_MOVE)
      has_move = true;
    if (acts[i] == atoms._NET_WM_ACTION_RESIZE)
      has_resize = true;
  }
  assert(has_move);
  assert(!has_resize); // Should NOT have resize

  printf("test_allowed_actions passed\n");

  render_free(&hot->render_ctx);
  if (hot->icon_surface)
    cairo_surface_destroy(hot->icon_surface);
  slotmap_destroy(&s.clients);
  small_vec_destroy(&s.active_clients);
  hash_map_destroy(&s.window_to_client);
  hash_map_destroy(&s.frame_to_client);
  arena_destroy(&s.tick_arena);
  free(s.conn);
}

void test_desktop_clamp_single(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t *)malloc(1);
  s.desktop_count = 1;
  s.current_desktop = 0;
  arena_init(&s.tick_arena, 4096);

  atoms._NET_WM_DESKTOP = 500;
  atoms.WM_STATE = 501;

  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t),
                    sizeof(client_cold_t)))
    return;
  small_vec_init(&s.active_clients);
  list_init(&s.focus_history);

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  small_vec_push(&s.active_clients, handle_to_ptr(h));
  client_hot_t *hot = (client_hot_t *)hot_ptr;
  hot->self = h;
  hot->xid = 123;
  hot->frame = 456;
  hot->state = STATE_MAPPED;
  hot->desktop = 0;
  hot->sticky = false;
  list_init(&hot->focus_node);

  hash_map_init(&s.window_to_client);
  hash_map_init(&s.frame_to_client);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_init(&s.layers[i]);

  stub_last_prop_atom = 0;
  wm_client_move_to_workspace(&s, h, 2, false);
  wm_flush_dirty(&s, monotonic_time_ns());

  // We need to see if _NET_WM_DESKTOP was set.
  // Since wm_flush_dirty sets multiple props, we check the history if
  // available, or just assume if it was called, it's fine. In our stubs, we
  // have stub_prop_calls.

  bool found_desktop = false;
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

  for (int i = 0; i < stub_prop_calls_len; i++) {
    if (stub_prop_calls[i].window == 123 &&
        stub_prop_calls[i].atom == atoms._NET_WM_DESKTOP) {
      found_desktop = true;
      uint32_t *val = (uint32_t *)stub_prop_calls[i].data;
      assert(val[0] == 0);
    }
  }
  assert(found_desktop);

  printf("test_desktop_clamp_single passed\n");

  hash_map_destroy(&s.window_to_client);
  hash_map_destroy(&s.frame_to_client);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_destroy(&s.layers[i]);
  render_free(&hot->render_ctx);
  if (hot->icon_surface)
    cairo_surface_destroy(hot->icon_surface);
  slotmap_destroy(&s.clients);
  small_vec_destroy(&s.active_clients);
  arena_destroy(&s.tick_arena);
  free(s.conn);
}

void test_dirty_stack_relayer(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t *)malloc(1);
  s.root = 1;
  arena_init(&s.tick_arena, 4096);

  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_init(&s.layers[i]);

  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t),
                    sizeof(client_cold_t)))
    return;
  small_vec_init(&s.active_clients);

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  small_vec_push(&s.active_clients, handle_to_ptr(h));
  client_hot_t *hot = (client_hot_t *)hot_ptr;
  hot->self = h;
  hot->xid = 123;
  hot->frame = 456;
  hot->state = STATE_MAPPED;
  hot->layer = LAYER_NORMAL;
  hot->stacking_index = -1;
  hot->stacking_layer = -1;
  list_init(&hot->transient_sibling);
  list_init(&hot->transients_head);

  stack_raise(&s, h);

  hot->layer = LAYER_ABOVE;
  hot->dirty = DIRTY_STACK;

  wm_flush_dirty(&s, monotonic_time_ns());

  assert(s.layers[LAYER_NORMAL].length == 0);
  assert(s.layers[LAYER_ABOVE].length == 1);
  assert((handle_t)(uintptr_t)s.layers[LAYER_ABOVE].items[0] == h);

  printf("test_dirty_stack_relayer passed\n");

  render_free(&hot->render_ctx);
  if (hot->icon_surface)
    cairo_surface_destroy(hot->icon_surface);
  slotmap_destroy(&s.clients);
  arena_destroy(&s.tick_arena);
  free(s.conn);
}

int main(void) {
  test_frame_extents();
  test_allowed_actions();
  test_desktop_clamp_single();
  test_dirty_stack_relayer();
  return 0;
}
