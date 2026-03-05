#include <X11/keysym.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xproto.h>

#include "client.h"
#include "config.h"
#include "cookie_jar.h"
#include "event.h"
#include "hxm.h"
#include "src/wm_internal.h"
#include "wm.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern int stub_grab_pointer_count;
extern int stub_ungrab_pointer_count;
extern int stub_grab_key_count;
extern int stub_ungrab_key_count;
extern uint16_t stub_last_grab_key_mods;
extern xcb_keycode_t stub_last_grab_keycode;
extern int stub_sync_await_count;
extern int stub_config_calls_len;
extern void xcb_stubs_set_query_pointer_sequence(uint32_t sequence);

void __real_cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data, uint64_t txn_id, cookie_handler_fn handler);

static int g_cookie_push_query_pointer_calls = 0;

void __wrap_cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data, uint64_t txn_id, cookie_handler_fn handler) {
  if (type == COOKIE_QUERY_POINTER)
    g_cookie_push_query_pointer_calls++;
  __real_cookie_jar_push(cj, sequence, type, client, data, txn_id, handler);
}

static void reset_cookie_push_spy(void) {
  g_cookie_push_query_pointer_calls = 0;
}

static void setup_server(server_t* s) {
  memset(s, 0, sizeof(*s));
  s->is_test = true;
  s->conn = xcb_connect(NULL, NULL);
  atoms_init(s->conn);
  s->root = 1;
  s->root_visual = 1;
  s->root_depth = 24;
  s->root_visual_type = xcb_get_visualtype(s->conn, 0);

  config_init_defaults(&s->config);

  slotmap_init(&s->clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
  cookie_jar_init(&s->cookie_jar);
  hash_map_init(&s->window_to_client);
  hash_map_init(&s->frame_to_client);
  list_init(&s->focus_history);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_init(&s->layers[i]);

  s->desktop_count = 2;
  s->current_desktop = 0;
}

static void cleanup_server(server_t* s) {
  for (uint32_t i = 1; i < s->clients.cap; i++) {
    if (s->clients.hdr[i].live) {
      handle_t h = handle_make(i, s->clients.hdr[i].gen);
      client_hot_t* hot = server_chot(s, h);
      client_cold_t* cold = server_ccold(s, h);
      if (cold) {
        arena_destroy(&cold->string_arena);
      }
      if (hot) {
        render_free(&hot->render_ctx);
        if (hot->icon_surface)
          cairo_surface_destroy(hot->icon_surface);
      }
    }
  }
  config_destroy(&s->config);
  cookie_jar_destroy(&s->cookie_jar);
  slotmap_destroy(&s->clients);
  hash_map_destroy(&s->window_to_client);
  hash_map_destroy(&s->frame_to_client);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_destroy(&s->layers[i]);
  xcb_disconnect(s->conn);
}

static void reset_keybindings(config_t* config) {
  for (size_t i = 0; i < config->key_bindings.length; i++) {
    key_binding_t* b = config->key_bindings.items[i];
    if (b->exec_cmd)
      free(b->exec_cmd);
    free(b);
  }
  small_vec_destroy(&config->key_bindings);
  small_vec_init(&config->key_bindings);
}

static handle_t add_mapped_client(server_t* s, xcb_window_t win, xcb_window_t frame) {
  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  client_cold_t* cold = (client_cold_t*)cold_ptr;
  memset(hot, 0, sizeof(*hot));
  memset(cold, 0, sizeof(*cold));

  render_init(&hot->render_ctx);
  arena_init(&cold->string_arena, 128);

  hot->self = h;
  hot->xid = win;
  hot->frame = frame;
  hot->state = STATE_MAPPED;
  hot->type = WINDOW_TYPE_NORMAL;
  hot->focus_override = -1;
  hot->layer = LAYER_NORMAL;
  hot->base_layer = LAYER_NORMAL;
  hot->server = (rect_t){10, 10, 200, 150};
  hot->desired = hot->server;

  list_init(&hot->focus_node);
  list_init(&hot->transients_head);
  list_init(&hot->transient_sibling);

  hash_map_insert(&s->window_to_client, win, handle_to_ptr(h));
  hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));

  return h;
}

static void test_click_to_focus(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  handle_t h1 = add_mapped_client(&s, 1001, 1101);
  handle_t h2 = add_mapped_client(&s, 1002, 1102);

  wm_set_focus(&s, h1);
  assert(s.focused_client == h1);

  xcb_button_press_event_t ev = {0};
  ev.event = 1002;
  ev.detail = 1;
  ev.state = 0;

  wm_handle_button_press(&s, &ev);

  assert(s.focused_client == h2);
  assert(s.interaction_mode == INTERACTION_NONE);

  printf("test_click_to_focus passed\n");
  cleanup_server(&s);
}

static void test_click_ignores_dock_and_desktop(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  handle_t h1 = add_mapped_client(&s, 1201, 1301);
  handle_t h2 = add_mapped_client(&s, 1202, 1302);
  handle_t h3 = add_mapped_client(&s, 1203, 1303);

  client_hot_t* dock = server_chot(&s, h2);
  client_hot_t* desktop = server_chot(&s, h3);
  assert(dock != NULL);
  assert(desktop != NULL);
  dock->type = WINDOW_TYPE_DOCK;
  desktop->type = WINDOW_TYPE_DESKTOP;

  wm_set_focus(&s, h1);
  assert(s.focused_client == h1);

  xcb_button_press_event_t ev = {0};
  ev.detail = 1;
  ev.state = 0;

  ev.event = dock->xid;
  wm_handle_button_press(&s, &ev);
  assert(s.focused_client == h1);
  assert(s.interaction_mode == INTERACTION_NONE);

  ev.event = desktop->xid;
  wm_handle_button_press(&s, &ev);
  assert(s.focused_client == h1);
  assert(s.interaction_mode == INTERACTION_NONE);

  printf("test_click_ignores_dock_and_desktop passed\n");
  cleanup_server(&s);
}

static void test_move_interaction(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  handle_t h = add_mapped_client(&s, 2001, 2101);
  client_hot_t* hot = server_chot(&s, h);

  xcb_button_press_event_t press = {0};
  press.event = hot->xid;
  press.detail = 1;
  press.state = XCB_MOD_MASK_1;
  press.root_x = 50;
  press.root_y = 60;

  wm_handle_button_press(&s, &press);
  assert(s.interaction_mode == INTERACTION_MOVE);
  assert(stub_grab_pointer_count == 1);

  xcb_motion_notify_event_t motion = {0};
  motion.root_x = 70;
  motion.root_y = 90;
  motion.event = hot->frame;
  motion.state = XCB_KEY_BUT_MASK_BUTTON_1;

  wm_handle_motion_notify(&s, &motion);
  assert(hot->desired.x == hot->server.x + 20);
  assert(hot->desired.y == hot->server.y + 30);
  assert(hot->dirty & DIRTY_GEOM);

  xcb_button_release_event_t release = {0};
  wm_handle_button_release(&s, &release);
  assert(s.interaction_mode == INTERACTION_NONE);
  assert(stub_ungrab_pointer_count == 1);

  printf("test_move_interaction passed\n");
  cleanup_server(&s);
}

static void test_resize_interaction(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  handle_t h = add_mapped_client(&s, 3001, 3101);
  client_hot_t* hot = server_chot(&s, h);

  xcb_button_press_event_t press = {0};
  press.event = hot->xid;
  press.detail = 3;
  press.state = XCB_MOD_MASK_1;
  press.root_x = 100;
  press.root_y = 100;

  wm_handle_button_press(&s, &press);
  assert(s.interaction_mode == INTERACTION_RESIZE);
  assert(stub_grab_pointer_count == 1);

  xcb_motion_notify_event_t motion = {0};
  motion.root_x = 140;
  motion.root_y = 120;
  motion.event = hot->frame;
  motion.state = XCB_KEY_BUT_MASK_BUTTON_3;

  wm_handle_motion_notify(&s, &motion);
  assert(hot->desired.w == (uint16_t)(hot->server.w + 40));
  assert(hot->desired.h == (uint16_t)(hot->server.h + 20));
  assert(hot->desired.x == hot->server.x);
  assert(hot->desired.y == hot->server.y);

  xcb_button_release_event_t release = {0};
  wm_handle_button_release(&s, &release);
  assert(stub_ungrab_pointer_count == 1);

  printf("test_resize_interaction passed\n");
  cleanup_server(&s);
}

static void test_resize_interaction_clamps_frame_overflow(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  handle_t h = add_mapped_client(&s, 3201, 3301);
  client_hot_t* hot = server_chot(&s, h);

  uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : (uint16_t)s.config.theme.border_width;
  uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : (uint16_t)s.config.theme.title_height;
  uint16_t max_client_w = MAX_FRAME_SIZE;
  uint16_t max_client_h = MAX_FRAME_SIZE;
  wm_compute_max_client_size(bw, th, hot->gtk_frame_extents_set, &max_client_w, &max_client_h);

  hot->server.w = (max_client_w > 2) ? (uint16_t)(max_client_w - 2) : max_client_w;
  hot->server.h = (max_client_h > 2) ? (uint16_t)(max_client_h - 2) : max_client_h;
  hot->desired.w = hot->server.w;
  hot->desired.h = hot->server.h;

  wm_start_interaction(&s, h, hot, false, RESIZE_BOTTOM | RESIZE_RIGHT, 100, 100, XCB_CURRENT_TIME, false);
  assert(s.interaction_mode == INTERACTION_RESIZE);

  xcb_motion_notify_event_t motion = {0};
  motion.root_x = 130;
  motion.root_y = 130;
  motion.event = hot->frame;
  motion.state = XCB_KEY_BUT_MASK_BUTTON_1;

  wm_handle_motion_notify(&s, &motion);

  assert(hot->desired.w == max_client_w);
  assert(hot->desired.h == max_client_h);
  assert((uint32_t)hot->desired.w + (uint32_t)(2u * bw) <= MAX_FRAME_SIZE);
  assert((uint32_t)hot->desired.h + (uint32_t)th + (uint32_t)bw <= MAX_FRAME_SIZE);

  xcb_button_release_event_t release = {0};
  wm_handle_button_release(&s, &release);
  assert(stub_ungrab_pointer_count == 1);

  printf("test_resize_interaction_clamps_frame_overflow passed\n");
  cleanup_server(&s);
}

static void test_resize_corner_top_left(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  handle_t h = add_mapped_client(&s, 4001, 4101);
  client_hot_t* hot = server_chot(&s, h);

  wm_start_interaction(&s, h, hot, false, RESIZE_TOP | RESIZE_LEFT, 100, 100, XCB_CURRENT_TIME, false);
  assert(stub_grab_pointer_count == 1);

  xcb_motion_notify_event_t motion = {0};
  motion.root_x = 110;
  motion.root_y = 105;
  motion.event = hot->frame;
  motion.state = XCB_KEY_BUT_MASK_BUTTON_1;

  wm_handle_motion_notify(&s, &motion);

  assert(hot->desired.w == (uint16_t)(hot->server.w - 10));
  assert(hot->desired.h == (uint16_t)(hot->server.h - 5));
  assert(hot->desired.x == hot->server.x + 10);
  assert(hot->desired.y == hot->server.y + 5);

  printf("test_resize_corner_top_left passed\n");
  cleanup_server(&s);
}

static void test_cancel_interaction_resets_cursor(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  handle_t h = add_mapped_client(&s, 5001, 5101);
  client_hot_t* hot = server_chot(&s, h);

  hot->last_cursor_dir = RESIZE_RIGHT;
  s.interaction_mode = INTERACTION_RESIZE;
  s.interaction_window = hot->frame;
  s.interaction_handle = h;
  s.interaction_resize_dir = RESIZE_RIGHT;

  wm_cancel_interaction(&s);

  assert(s.interaction_mode == INTERACTION_NONE);
  assert(hot->last_cursor_dir == RESIZE_NONE);
  assert(stub_ungrab_pointer_count == 1);

  printf("test_cancel_interaction_resets_cursor passed\n");
  cleanup_server(&s);
}

static void test_resize_no_sync_await(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  handle_t h = add_mapped_client(&s, 6001, 6101);
  client_hot_t* hot = server_chot(&s, h);

  hot->sync_enabled = true;
  hot->sync_counter = 1;
  hot->dirty |= DIRTY_GEOM;

  s.interaction_mode = INTERACTION_RESIZE;
  s.interaction_window = hot->frame;
  s.interaction_handle = h;

  wm_flush_dirty(&s, monotonic_time_ns());

  assert(stub_sync_await_count == 0);

  printf("test_resize_no_sync_await passed\n");
  cleanup_server(&s);
}

static void test_button_release_flushes_pending_resize(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  s.is_test = false;

  handle_t h = add_mapped_client(&s, 6101, 6201);
  client_hot_t* hot = server_chot(&s, h);
  small_vec_init(&s.active_clients);
  small_vec_push(&s.active_clients, handle_to_ptr(h));

  s.interaction_mode = INTERACTION_RESIZE;
  s.interaction_window = hot->frame;
  s.interaction_handle = h;
  s.interaction_resize_dir = RESIZE_BOTTOM | RESIZE_RIGHT;

  hot->desired.w = (uint16_t)(hot->server.w + 32);
  hot->desired.h = (uint16_t)(hot->server.h + 24);
  hot->dirty |= DIRTY_GEOM;

  stub_config_calls_len = 0;

  xcb_button_release_event_t ev = {0};
  ev.root_x = 200;
  ev.root_y = 180;

  wm_handle_button_release(&s, &ev);

  assert(s.interaction_mode == INTERACTION_NONE);
  assert((hot->dirty & DIRTY_GEOM) == 0);
  assert(hot->server.w == hot->desired.w);
  assert(hot->server.h == hot->desired.h);
  assert(stub_config_calls_len >= 2);

  printf("test_button_release_flushes_pending_resize passed\n");
  small_vec_destroy(&s.active_clients);
  cleanup_server(&s);
}

static void test_keybinding_clean_mods(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  key_binding_t* b = calloc(1, sizeof(*b));
  b->keysym = XK_Escape;
  b->modifiers = XCB_MOD_MASK_1;
  b->action = ACTION_RESTART;
  reset_keybindings(&s.config);
  small_vec_push(&s.config.key_bindings, b);

  g_restart_pending = 0;

  xcb_key_press_event_t ev = {0};
  ev.detail = 9;
  ev.state = XCB_MOD_MASK_1 | XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2;

  s.keysyms = xcb_key_symbols_alloc(s.conn);
  wm_handle_key_press(&s, &ev);

  assert(g_restart_pending == 1);

  printf("test_keybinding_clean_mods passed\n");
  xcb_key_symbols_free(s.keysyms);
  cleanup_server(&s);
}

static void test_keybinding_conflict_deterministic(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  key_binding_t* first = calloc(1, sizeof(*first));
  first->keysym = XK_Escape;
  first->modifiers = 0;
  first->action = ACTION_RESTART;

  key_binding_t* second = calloc(1, sizeof(*second));
  second->keysym = XK_Escape;
  second->modifiers = 0;
  second->action = ACTION_WORKSPACE_NEXT;

  reset_keybindings(&s.config);
  small_vec_push(&s.config.key_bindings, first);
  small_vec_push(&s.config.key_bindings, second);

  g_restart_pending = 0;
  s.current_desktop = 0;

  xcb_key_press_event_t ev = {0};
  ev.detail = 9;
  ev.state = 0;

  s.keysyms = xcb_key_symbols_alloc(s.conn);
  wm_handle_key_press(&s, &ev);

  assert(g_restart_pending == 1);
  assert(s.current_desktop == 0);

  printf("test_keybinding_conflict_deterministic passed\n");
  xcb_key_symbols_free(s.keysyms);
  cleanup_server(&s);
}

static void test_key_grabs_from_config(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  key_binding_t* b = calloc(1, sizeof(*b));
  b->keysym = XK_Escape;
  b->modifiers = XCB_MOD_MASK_1;
  b->action = ACTION_RESTART;
  reset_keybindings(&s.config);
  small_vec_push(&s.config.key_bindings, b);

  wm_setup_keys(&s);

  assert(stub_ungrab_key_count >= 1);
  assert(stub_grab_key_count >= 1);
  assert(wm_clean_mods(stub_last_grab_key_mods) == XCB_MOD_MASK_1);
  assert(stub_last_grab_keycode != 0);

  printf("test_key_grabs_from_config passed\n");
  if (s.keysyms) {
    xcb_key_symbols_free(s.keysyms);
    s.keysyms = NULL;
  }
  cleanup_server(&s);
}

static void test_moveresize_keyboard_zero_query_sequence_skips_enqueue(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_cookie_push_spy();

  handle_t h = add_mapped_client(&s, 7001, 7101);
  client_hot_t* hot = server_chot(&s, h);

  xcb_stubs_set_query_pointer_sequence(0);

  xcb_client_message_event_t ev = {0};
  ev.format = 32;
  ev.window = hot->xid;
  ev.type = atoms._NET_WM_MOVERESIZE;
  ev.data.data32[0] = (uint32_t)-1;
  ev.data.data32[1] = (uint32_t)-1;
  ev.data.data32[2] = 10;  // NET_WM_MOVERESIZE_MOVE_KEYBOARD
  ev.data.data32[3] = 0;
  ev.data.data32[4] = 1;

  wm_handle_client_message(&s, &ev);

  assert(g_cookie_push_query_pointer_calls == 0);
  assert(!cookie_jar_has_pending(&s.cookie_jar));
  assert(s.interaction_mode == INTERACTION_NONE);

  printf("test_moveresize_keyboard_zero_query_sequence_skips_enqueue passed\n");
  cleanup_server(&s);
}

int main(void) {
  test_click_to_focus();
  test_click_ignores_dock_and_desktop();
  test_move_interaction();
  test_resize_interaction();
  test_resize_interaction_clamps_frame_overflow();
  test_resize_corner_top_left();
  test_cancel_interaction_resets_cursor();
  test_resize_no_sync_await();
  test_button_release_flushes_pending_resize();
  test_keybinding_clean_mods();
  test_keybinding_conflict_deterministic();
  test_key_grabs_from_config();
  test_moveresize_keyboard_zero_query_sequence_skips_enqueue();
  return 0;
}
