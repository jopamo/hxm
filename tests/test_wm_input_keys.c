#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "client.h"
#include "config.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "../src/wm_internal.h"

volatile sig_atomic_t g_reload_pending = 0;
volatile sig_atomic_t g_shutdown_pending = 0;
volatile sig_atomic_t g_restart_pending = 0;

void hxm_log(enum log_level level, const char* fmt, ...) {
  (void)level;
  (void)fmt;

  va_list args;
  va_start(args, fmt);
  va_end(args);
}

static jmp_buf g_exit_jmp_buf;

static xcb_keysym_t g_fake_keysym = 0;
static uint32_t g_query_pointer_sequence = 1;
static bool g_cookie_jar_push_should_fail = false;
static bool g_client_can_move = true;
static bool g_client_can_resize = true;
static handle_t g_menu_selected_client = HANDLE_INVALID;

static int spy_menu_hide_calls = 0;
static int spy_menu_handle_key_press_calls = 0;
static int spy_menu_show_switcher_calls = 0;
static handle_t spy_menu_show_switcher_last_origin = HANDLE_INVALID;
static int spy_menu_switcher_step_calls = 0;
static int spy_menu_switcher_step_last_dir = 0;

static int spy_client_close_calls = 0;
static handle_t spy_client_close_last = HANDLE_INVALID;

static int spy_wm_set_focus_calls = 0;
static handle_t spy_wm_set_focus_last = HANDLE_INVALID;

static int spy_stack_raise_calls = 0;
static handle_t spy_stack_raise_last = HANDLE_INVALID;

static int spy_switch_workspace_calls = 0;
static uint32_t spy_switch_workspace_last = 0;

static int spy_switch_workspace_relative_calls = 0;
static int spy_switch_workspace_relative_last_delta = 0;

static int spy_move_to_workspace_calls = 0;
static handle_t spy_move_to_workspace_last_client = HANDLE_INVALID;
static uint32_t spy_move_to_workspace_last_workspace = 0;
static bool spy_move_to_workspace_last_follow = false;

static int spy_wm_client_restore_calls = 0;
static handle_t spy_wm_client_restore_last = HANDLE_INVALID;

static int spy_toggle_sticky_calls = 0;
static handle_t spy_toggle_sticky_last = HANDLE_INVALID;

static int spy_cookie_jar_push_calls = 0;
static uint32_t spy_cookie_jar_push_last_sequence = 0;
static cookie_type_t spy_cookie_jar_push_last_type = COOKIE_NONE;
static handle_t spy_cookie_jar_push_last_client = HANDLE_INVALID;

static int spy_wm_start_interaction_calls = 0;
static bool spy_wm_start_interaction_last_start_move = false;
static bool spy_wm_start_interaction_last_is_keyboard = false;

static int spy_warp_pointer_calls = 0;

static int spy_exit_calls = 0;
static int spy_exit_last_code = 0;

static void reset_spies(void) {
  g_fake_keysym = 0;
  g_query_pointer_sequence = 1;
  g_cookie_jar_push_should_fail = false;
  g_client_can_move = true;
  g_client_can_resize = true;
  g_menu_selected_client = HANDLE_INVALID;

  spy_menu_hide_calls = 0;
  spy_menu_handle_key_press_calls = 0;
  spy_menu_show_switcher_calls = 0;
  spy_menu_show_switcher_last_origin = HANDLE_INVALID;
  spy_menu_switcher_step_calls = 0;
  spy_menu_switcher_step_last_dir = 0;

  spy_client_close_calls = 0;
  spy_client_close_last = HANDLE_INVALID;

  spy_wm_set_focus_calls = 0;
  spy_wm_set_focus_last = HANDLE_INVALID;

  spy_stack_raise_calls = 0;
  spy_stack_raise_last = HANDLE_INVALID;

  spy_switch_workspace_calls = 0;
  spy_switch_workspace_last = 0;

  spy_switch_workspace_relative_calls = 0;
  spy_switch_workspace_relative_last_delta = 0;

  spy_move_to_workspace_calls = 0;
  spy_move_to_workspace_last_client = HANDLE_INVALID;
  spy_move_to_workspace_last_workspace = 0;
  spy_move_to_workspace_last_follow = false;

  spy_wm_client_restore_calls = 0;
  spy_wm_client_restore_last = HANDLE_INVALID;

  spy_toggle_sticky_calls = 0;
  spy_toggle_sticky_last = HANDLE_INVALID;

  spy_cookie_jar_push_calls = 0;
  spy_cookie_jar_push_last_sequence = 0;
  spy_cookie_jar_push_last_type = COOKIE_NONE;
  spy_cookie_jar_push_last_client = HANDLE_INVALID;

  spy_wm_start_interaction_calls = 0;
  spy_wm_start_interaction_last_start_move = false;
  spy_wm_start_interaction_last_is_keyboard = false;

  spy_warp_pointer_calls = 0;

  spy_exit_calls = 0;
  spy_exit_last_code = 0;

  g_restart_pending = 0;
}

static void setup_server(server_t* s) {
  memset(s, 0, sizeof(*s));

  s->keysyms = (xcb_key_symbols_t*)0x1;
  s->root = 1;
  s->current_desktop = 0;
  s->menu.selected_index = -1;

  list_init(&s->focus_history);

  bool ok = slotmap_init(&s->clients, 16u, sizeof(client_hot_t), sizeof(client_cold_t));
  assert(ok);

  s->active_clients.length = 0;
  s->active_clients.items = NULL;
}

static void teardown_server(server_t* s) {
  slotmap_destroy(&s->clients);
}

static client_hot_t* add_client(server_t* s, int32_t desktop, bool sticky, client_state_t state, window_type_t type, handle_t* out_handle) {
  void* hot_ptr = NULL;
  handle_t h = slotmap_alloc(&s->clients, &hot_ptr, NULL);
  assert(h != HANDLE_INVALID);

  client_hot_t* hot = (client_hot_t*)hot_ptr;
  memset(hot, 0, sizeof(*hot));

  hot->self = h;
  hot->desktop = desktop;
  hot->sticky = sticky;
  hot->state = (uint8_t)state;
  hot->type = (uint8_t)type;

  hot->server.x = 10;
  hot->server.y = 10;
  hot->server.w = 100;
  hot->server.h = 50;

  list_init(&hot->focus_node);

  s->active_clients.length++;

  if (out_handle)
    *out_handle = h;

  return hot;
}

static void set_bindings(server_t* s, key_binding_t** bindings, size_t nbindings) {
  s->config.key_bindings.items = (void**)bindings;
  s->config.key_bindings.length = nbindings;
}

void __wrap_menu_hide(server_t* s) {
  spy_menu_hide_calls++;
  if (!s)
    return;
  s->menu.visible = false;
  s->menu.is_switcher = false;
  s->menu.selected_index = -1;
}

void __wrap_menu_handle_key_press(server_t* s, xcb_key_press_event_t* ev) {
  (void)s;
  (void)ev;
  spy_menu_handle_key_press_calls++;
}

void __wrap_menu_show_switcher(server_t* s, handle_t origin) {
  spy_menu_show_switcher_calls++;
  spy_menu_show_switcher_last_origin = origin;
  g_menu_selected_client = origin;

  if (!s)
    return;

  s->menu.visible = true;
  s->menu.is_switcher = true;
  s->menu.selected_index = 0;
}

bool __wrap_menu_switcher_step(server_t* s, int dir) {
  (void)s;
  spy_menu_switcher_step_calls++;
  spy_menu_switcher_step_last_dir = dir;
  return true;
}

handle_t __wrap_menu_switcher_selected_client(const server_t* s) {
  (void)s;
  return g_menu_selected_client;
}

void __wrap_client_close(server_t* s, handle_t h) {
  (void)s;
  spy_client_close_calls++;
  spy_client_close_last = h;
}

void __wrap_wm_set_focus(server_t* s, handle_t h) {
  (void)s;
  spy_wm_set_focus_calls++;
  spy_wm_set_focus_last = h;
}

void __wrap_stack_raise(server_t* s, handle_t h) {
  (void)s;
  spy_stack_raise_calls++;
  spy_stack_raise_last = h;
}

void __wrap_wm_switch_workspace(server_t* s, uint32_t ws) {
  (void)s;
  spy_switch_workspace_calls++;
  spy_switch_workspace_last = ws;
}

void __wrap_wm_switch_workspace_relative(server_t* s, int delta) {
  (void)s;
  spy_switch_workspace_relative_calls++;
  spy_switch_workspace_relative_last_delta = delta;
}

void __wrap_wm_client_move_to_workspace(server_t* s, handle_t h, uint32_t ws, bool follow) {
  (void)s;
  spy_move_to_workspace_calls++;
  spy_move_to_workspace_last_client = h;
  spy_move_to_workspace_last_workspace = ws;
  spy_move_to_workspace_last_follow = follow;
}

void __wrap_wm_client_restore(server_t* s, handle_t h) {
  (void)s;
  spy_wm_client_restore_calls++;
  spy_wm_client_restore_last = h;
}

void __wrap_wm_client_toggle_sticky(server_t* s, handle_t h) {
  (void)s;
  spy_toggle_sticky_calls++;
  spy_toggle_sticky_last = h;
}

bool __wrap_client_can_move(const client_hot_t* hot) {
  (void)hot;
  return g_client_can_move;
}

bool __wrap_client_can_resize(const client_hot_t* hot) {
  (void)hot;
  return g_client_can_resize;
}

xcb_query_pointer_cookie_t __wrap_xcb_query_pointer(xcb_connection_t* c, xcb_window_t window) {
  (void)c;
  (void)window;

  xcb_query_pointer_cookie_t ck;
  ck.sequence = g_query_pointer_sequence;
  return ck;
}

bool __wrap_cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data, uint64_t txn_id, cookie_handler_fn handler) {
  (void)cj;
  (void)data;
  (void)txn_id;
  (void)handler;

  spy_cookie_jar_push_calls++;
  spy_cookie_jar_push_last_sequence = sequence;
  spy_cookie_jar_push_last_type = type;
  spy_cookie_jar_push_last_client = client;

  return !g_cookie_jar_push_should_fail;
}

xcb_void_cookie_t __wrap_xcb_warp_pointer(xcb_connection_t* c,
                                          xcb_window_t src_window,
                                          xcb_window_t dst_window,
                                          int16_t src_x,
                                          int16_t src_y,
                                          uint16_t src_width,
                                          uint16_t src_height,
                                          int16_t dst_x,
                                          int16_t dst_y) {
  (void)c;
  (void)src_window;
  (void)dst_window;
  (void)src_x;
  (void)src_y;
  (void)src_width;
  (void)src_height;
  (void)dst_x;
  (void)dst_y;

  spy_warp_pointer_calls++;

  xcb_void_cookie_t ck;
  ck.sequence = 1;
  return ck;
}

void __wrap_wm_start_interaction(server_t* s,
                                 handle_t h,
                                 client_hot_t* hot,
                                 bool start_move,
                                 int resize_dir,
                                 int16_t root_x,
                                 int16_t root_y,
                                 uint32_t time,
                                 bool is_keyboard) {
  (void)s;
  (void)h;
  (void)hot;
  (void)resize_dir;
  (void)root_x;
  (void)root_y;
  (void)time;

  spy_wm_start_interaction_calls++;
  spy_wm_start_interaction_last_start_move = start_move;
  spy_wm_start_interaction_last_is_keyboard = is_keyboard;
}

xcb_keysym_t __wrap_xcb_key_symbols_get_keysym(xcb_key_symbols_t* syms, xcb_keycode_t keycode, int col) {
  (void)syms;
  (void)keycode;
  (void)col;
  return g_fake_keysym;
}

__attribute__((noreturn)) void __wrap_exit(int code) {
  spy_exit_calls++;
  spy_exit_last_code = code;
  longjmp(g_exit_jmp_buf, 1);
}

void wm_handle_reply(server_t* s, const cookie_slot_t* slot, void* reply, xcb_generic_error_t* err) {
  (void)s;
  (void)slot;
  (void)reply;
  (void)err;
}

static void test_wm_clean_mods_masks_lock_num_scroll(void) {
  uint16_t in = 0;
  in |= XCB_MOD_MASK_LOCK;
  in |= XCB_MOD_MASK_2;
  in |= XCB_MOD_MASK_5;
  in |= (1u << 0);

  uint32_t out = wm_clean_mods(in);
  assert((out & XCB_MOD_MASK_LOCK) == 0);
  assert((out & XCB_MOD_MASK_2) == 0);
  assert((out & XCB_MOD_MASK_5) == 0);
  assert((out & (1u << 0)) != 0);
}

static void test_wm_cycle_focus_selects_next_focusable(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  handle_t a_handle = HANDLE_INVALID;
  handle_t b_handle = HANDLE_INVALID;
  handle_t c_handle = HANDLE_INVALID;

  client_hot_t* a = add_client(&s, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL, &a_handle);
  client_hot_t* b = add_client(&s, 0, false, STATE_MAPPED, WINDOW_TYPE_DOCK, &b_handle);
  client_hot_t* c = add_client(&s, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL, &c_handle);

  list_push_back(&s.focus_history, &a->focus_node);
  list_push_back(&s.focus_history, &b->focus_node);
  list_push_back(&s.focus_history, &c->focus_node);

  s.focused_client = a_handle;

  wm_cycle_focus(&s, true);

  assert(spy_wm_set_focus_calls == 1);
  assert(spy_wm_set_focus_last == c_handle);
  assert(spy_stack_raise_calls == 1);
  assert(spy_stack_raise_last == c_handle);

  teardown_server(&s);
}

static void test_wm_cycle_focus_no_focusable_no_calls(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  client_hot_t* a = add_client(&s, 0, false, STATE_MAPPED, WINDOW_TYPE_DOCK, NULL);
  client_hot_t* b = add_client(&s, 0, false, STATE_MAPPED, WINDOW_TYPE_TOOLTIP, NULL);

  list_push_back(&s.focus_history, &a->focus_node);
  list_push_back(&s.focus_history, &b->focus_node);

  s.focused_client = a->self;

  wm_cycle_focus(&s, true);

  assert(spy_wm_set_focus_calls == 0);
  assert(spy_stack_raise_calls == 0);

  teardown_server(&s);
}

static void test_key_press_menu_delegates_to_menu(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  s.menu.visible = true;

  xcb_key_press_event_t ev = {.detail = 9, .state = 0};
  g_fake_keysym = XK_Escape;

  wm_handle_key_press(&s, &ev);

  assert(spy_menu_handle_key_press_calls == 1);
  assert(spy_menu_hide_calls == 0);
  assert(spy_client_close_calls == 0);

  teardown_server(&s);
}

static void test_key_press_matches_binding_with_ignored_mods(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  key_binding_t bind = {
    .keysym = 0x1234u,
    .modifiers = (1u << 0),
    .action = ACTION_RESTART,
    .exec_cmd = NULL,
  };
  key_binding_t* bindings[] = {&bind};
  set_bindings(&s, bindings, 1);

  xcb_key_press_event_t ev;
  ev.detail = 10;
  ev.state = (uint16_t)((1u << 0) | XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2 | XCB_MOD_MASK_5);

  g_fake_keysym = 0x1234u;

  wm_handle_key_press(&s, &ev);

  assert(g_restart_pending == 1);

  teardown_server(&s);
}

static void test_key_press_action_close_calls_client_close(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  key_binding_t bind = {
    .keysym = 0x2222u,
    .modifiers = 0,
    .action = ACTION_CLOSE,
    .exec_cmd = NULL,
  };
  key_binding_t* bindings[] = {&bind};
  set_bindings(&s, bindings, 1);

  s.focused_client = 0xBEEFu;

  xcb_key_press_event_t ev = {.detail = 11, .state = 0};
  g_fake_keysym = 0x2222u;

  wm_handle_key_press(&s, &ev);

  assert(spy_client_close_calls == 1);
  assert(spy_client_close_last == 0xBEEFu);

  teardown_server(&s);
}

static void test_key_press_action_focus_next_dispatch(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  handle_t a_handle = HANDLE_INVALID;
  client_hot_t* a = add_client(&s, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL, &a_handle);
  client_hot_t* b = add_client(&s, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL, NULL);

  list_push_back(&s.focus_history, &a->focus_node);
  list_push_back(&s.focus_history, &b->focus_node);
  s.focused_client = a_handle;

  key_binding_t bind = {
    .keysym = 0x3333u,
    .modifiers = 0,
    .action = ACTION_FOCUS_NEXT,
    .exec_cmd = NULL,
  };
  key_binding_t* bindings[] = {&bind};
  set_bindings(&s, bindings, 1);

  xcb_key_press_event_t ev = {.detail = 12, .state = 0};
  g_fake_keysym = 0x3333u;

  wm_handle_key_press(&s, &ev);

  assert(spy_menu_show_switcher_calls == 1);
  assert(spy_menu_show_switcher_last_origin == a_handle);
  assert(spy_menu_switcher_step_calls == 1);
  assert(spy_menu_switcher_step_last_dir == 1);
  assert(spy_wm_set_focus_calls == 0);

  teardown_server(&s);
}

static void test_switcher_commit_restores_and_focuses(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  handle_t h = HANDLE_INVALID;
  add_client(&s, 1, false, STATE_UNMAPPED, WINDOW_TYPE_NORMAL, &h);

  s.current_desktop = 0;
  s.switcher_active = true;
  s.menu.visible = true;
  s.menu.is_switcher = true;
  g_menu_selected_client = h;

  wm_switcher_commit(&s);

  assert(spy_menu_hide_calls == 1);
  assert(spy_switch_workspace_calls == 1);
  assert(spy_switch_workspace_last == 1u);
  assert(spy_wm_client_restore_calls == 1);
  assert(spy_wm_client_restore_last == h);
  assert(spy_wm_set_focus_calls == 1);
  assert(spy_wm_set_focus_last == h);
  assert(spy_stack_raise_calls == 1);
  assert(spy_stack_raise_last == h);

  teardown_server(&s);
}

static void test_key_press_action_workspace_uses_safe_atoi(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  char workspace_str[] = "2";
  key_binding_t bind = {
    .keysym = 0x4444u,
    .modifiers = 0,
    .action = ACTION_WORKSPACE,
    .exec_cmd = workspace_str,
  };
  key_binding_t* bindings[] = {&bind};
  set_bindings(&s, bindings, 1);

  xcb_key_press_event_t ev = {.detail = 13, .state = 0};
  g_fake_keysym = 0x4444u;

  wm_handle_key_press(&s, &ev);

  assert(spy_switch_workspace_calls == 1);
  assert(spy_switch_workspace_last == 2u);

  teardown_server(&s);
}

static void test_key_press_action_move_to_workspace_follow(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  char workspace_str[] = "7";
  key_binding_t bind = {
    .keysym = 0x5555u,
    .modifiers = 0,
    .action = ACTION_MOVE_TO_WORKSPACE_FOLLOW,
    .exec_cmd = workspace_str,
  };
  key_binding_t* bindings[] = {&bind};
  set_bindings(&s, bindings, 1);

  s.focused_client = 0xCAFEu;

  xcb_key_press_event_t ev = {.detail = 14, .state = 0};
  g_fake_keysym = 0x5555u;

  wm_handle_key_press(&s, &ev);

  assert(spy_move_to_workspace_calls == 1);
  assert(spy_move_to_workspace_last_client == 0xCAFEu);
  assert(spy_move_to_workspace_last_workspace == 7u);
  assert(spy_move_to_workspace_last_follow == true);

  teardown_server(&s);
}

static void test_key_press_action_toggle_sticky(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  key_binding_t bind = {
    .keysym = 0x6666u,
    .modifiers = 0,
    .action = ACTION_TOGGLE_STICKY,
    .exec_cmd = NULL,
  };
  key_binding_t* bindings[] = {&bind};
  set_bindings(&s, bindings, 1);

  s.focused_client = 0x123u;

  xcb_key_press_event_t ev = {.detail = 15, .state = 0};
  g_fake_keysym = 0x6666u;

  wm_handle_key_press(&s, &ev);

  assert(spy_toggle_sticky_calls == 1);
  assert(spy_toggle_sticky_last == 0x123u);

  teardown_server(&s);
}

static void test_key_press_action_move_zero_query_sequence_skips_cookie_enqueue(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  handle_t focused_handle = HANDLE_INVALID;
  add_client(&s, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL, &focused_handle);
  s.focused_client = focused_handle;

  key_binding_t bind = {
    .keysym = 0x6A6Au,
    .modifiers = 0,
    .action = ACTION_MOVE,
    .exec_cmd = NULL,
  };
  key_binding_t* bindings[] = {&bind};
  set_bindings(&s, bindings, 1);

  g_query_pointer_sequence = 0;

  xcb_key_press_event_t ev = {.detail = 19, .state = 0};
  g_fake_keysym = 0x6A6Au;

  wm_handle_key_press(&s, &ev);

  assert(spy_cookie_jar_push_calls == 0);
  assert(spy_wm_start_interaction_calls == 0);

  teardown_server(&s);
}

static void test_key_press_action_resize_starts_interaction(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  handle_t focused_handle = HANDLE_INVALID;
  client_hot_t* focused = add_client(&s, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL, &focused_handle);
  focused->server.x = 20;
  focused->server.y = 30;
  focused->server.w = 120;
  focused->server.h = 80;
  s.focused_client = focused_handle;

  key_binding_t bind = {
    .keysym = 0x6B6Bu,
    .modifiers = 0,
    .action = ACTION_RESIZE,
    .exec_cmd = NULL,
  };
  key_binding_t* bindings[] = {&bind};
  set_bindings(&s, bindings, 1);

  xcb_key_press_event_t ev = {.detail = 20, .state = 0};
  g_fake_keysym = 0x6B6Bu;

  wm_handle_key_press(&s, &ev);

  assert(spy_warp_pointer_calls == 1);
  assert(spy_wm_start_interaction_calls == 1);
  assert(spy_wm_start_interaction_last_start_move == false);
  assert(spy_wm_start_interaction_last_is_keyboard == true);

  teardown_server(&s);
}

static void test_key_press_action_exit_intercepted(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  key_binding_t bind = {
    .keysym = 0x9999u,
    .modifiers = 0,
    .action = ACTION_EXIT,
    .exec_cmd = NULL,
  };
  key_binding_t* bindings[] = {&bind};
  set_bindings(&s, bindings, 1);

  xcb_key_press_event_t ev = {.detail = 18, .state = 0};
  g_fake_keysym = 0x9999u;

  if (setjmp(g_exit_jmp_buf) == 0) {
    wm_handle_key_press(&s, &ev);
  }

  assert(spy_exit_calls == 1);
  assert(spy_exit_last_code == 0);

  teardown_server(&s);
}

static void test_key_release_alt_commits_switcher(void) {
  reset_spies();

  server_t s;
  setup_server(&s);

  s.switcher_active = true;
  s.switcher_origin = HANDLE_INVALID;
  s.menu.visible = true;
  s.menu.is_switcher = true;
  g_menu_selected_client = HANDLE_INVALID;

  xcb_key_release_event_t ev = {.detail = 21, .state = 0};
  g_fake_keysym = XK_Alt_L;

  wm_handle_key_release(&s, &ev);

  assert(spy_menu_hide_calls == 1);
  assert(s.switcher_active == false);

  teardown_server(&s);
}

int main(void) {
  test_wm_clean_mods_masks_lock_num_scroll();
  test_wm_cycle_focus_selects_next_focusable();
  test_wm_cycle_focus_no_focusable_no_calls();
  test_key_press_menu_delegates_to_menu();
  test_key_press_matches_binding_with_ignored_mods();
  test_key_press_action_close_calls_client_close();
  test_key_press_action_focus_next_dispatch();
  test_switcher_commit_restores_and_focuses();
  test_key_press_action_workspace_uses_safe_atoi();
  test_key_press_action_move_to_workspace_follow();
  test_key_press_action_toggle_sticky();
  test_key_press_action_move_zero_query_sequence_skips_cookie_enqueue();
  test_key_press_action_resize_starts_interaction();
  test_key_press_action_exit_intercepted();
  test_key_release_alt_commits_switcher();

  puts("test_wm_input_keys: OK");
  return 0;
}
