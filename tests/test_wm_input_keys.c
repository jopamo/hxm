// tests/test_wm_input_keys.c
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// -----------------------------
// Minimal types + constants used by src/wm_input_keys.c
// -----------------------------

typedef uint32_t handle_t;

#define HANDLE_INVALID 0

// Modifier masks (match what the module expects to mask out)
#define XCB_MOD_MASK_LOCK (1u << 1)
#define XCB_MOD_MASK_2 (1u << 4)
#define XCB_MOD_MASK_5 (1u << 7)

// XCB "current time" used in wm_start_interaction call
#define XCB_CURRENT_TIME 0

// XCB "none" / grab constants used in wm_setup_keys or resize path
#define XCB_NONE 0
#define XCB_GRAB_ANY 0
#define XCB_MOD_MASK_ANY 0
#define XCB_GRAB_MODE_ASYNC 0

// Window types from your WM (subset used by is_focusable)
typedef enum {
  WINDOW_TYPE_NORMAL = 0,
  WINDOW_TYPE_DOCK,
  WINDOW_TYPE_NOTIFICATION,
  WINDOW_TYPE_DESKTOP,
  WINDOW_TYPE_MENU,
  WINDOW_TYPE_DROPDOWN_MENU,
  WINDOW_TYPE_POPUP_MENU,
  WINDOW_TYPE_TOOLTIP,
  WINDOW_TYPE_COMBO,
  WINDOW_TYPE_DND
} window_type_t;

// Client mapping state
typedef enum { STATE_UNMAPPED = 0, STATE_MAPPED = 1 } client_state_t;

// Actions from config
typedef enum {
  ACTION_NONE = 0,
  ACTION_CLOSE,
  ACTION_FOCUS_NEXT,
  ACTION_FOCUS_PREV,
  ACTION_TERMINAL,
  ACTION_EXEC,
  ACTION_RESTART,
  ACTION_EXIT,
  ACTION_WORKSPACE,
  ACTION_WORKSPACE_PREV,
  ACTION_WORKSPACE_NEXT,
  ACTION_MOVE_TO_WORKSPACE,
  ACTION_MOVE_TO_WORKSPACE_FOLLOW,
  ACTION_TOGGLE_STICKY,
  ACTION_MOVE,
  ACTION_RESIZE
} action_t;

// Resize flags used by RESIZE path (values don't matter for logic tests)
#define RESIZE_BOTTOM (1u << 1)
#define RESIZE_RIGHT (1u << 2)

// Simple intrusive list node, sentinel-based
typedef struct list_node {
  struct list_node *prev;
  struct list_node *next;
} list_node_t;

static inline void list_init(list_node_t *head) {
  head->prev = head;
  head->next = head;
}

static inline bool list_empty(list_node_t *head) { return head->next == head; }

static inline void list_push_back(list_node_t *head, list_node_t *node) {
  node->prev = head->prev;
  node->next = head;
  head->prev->next = node;
  head->prev = node;
}

// Key binding structure used by module
typedef struct key_binding {
  uint32_t keysym;
  uint32_t modifiers;
  int action;
  const char *exec_cmd;
} key_binding_t;

typedef struct {
  size_t length;
  key_binding_t **items;
} key_bindings_vec_t;

typedef struct {
  key_bindings_vec_t key_bindings;
} config_t;

typedef struct {
  size_t length;
} small_vec_t;

// Minimal xcb structs used by wm_handle_key_press
typedef uint32_t xcb_keysym_t;
typedef uint8_t xcb_keycode_t;

typedef struct {
  uint8_t detail;
  uint16_t state;
} xcb_key_press_event_t;

// "Menu" state referenced by wm_handle_key_press
typedef struct {
  bool visible;
} menu_state_t;

// Client geometry subset used by RESIZE warp math
typedef struct {
  int16_t x, y;
  int16_t w, h;
} geom_t;

// client_hot_t contains focus_node and fields touched by is_focusable + resize
// path
typedef struct client_hot {
  list_node_t focus_node;

  client_state_t state;
  int32_t desktop;
  bool sticky;
  window_type_t type;
  handle_t self;
  bool show_desktop_hidden;

  geom_t server;
} client_hot_t;

// Cookie jar / reply plumbing is referenced only in MOVE branch
// For these unit tests, we avoid ACTION_MOVE and ACTION_RESIZE,
// but the file references these types anyway
typedef struct {
  int dummy;
} cookie_jar_t;

#define COOKIE_QUERY_POINTER 0

// server_t contains what this module touches
typedef struct server {
  void *conn;
  uint32_t root;
  void *keysyms;

  config_t config;

  list_node_t focus_history;
  handle_t focused_client;
  uint32_t current_desktop;
  bool showing_desktop;

  small_vec_t active_clients;

  cookie_jar_t cookie_jar;
  uint64_t txn_id;

  menu_state_t menu;
} server_t;

// Global referenced by ACTION_RESTART
int g_restart_pending = 0;

// Logging macros used in module
#define LOG_DEBUG(...)                                                         \
  do {                                                                         \
    (void)0;                                                                   \
  } while (0)
#define LOG_INFO(...)                                                          \
  do {                                                                         \
    (void)0;                                                                   \
  } while (0)

// -----------------------------
// Stubs/spies for external WM functions called by module
// -----------------------------

static int spy_menu_hide_calls;
static int spy_menu_handle_key_press_calls;
static int spy_client_close_calls;
static handle_t spy_client_close_last;

static int spy_wm_set_focus_calls;
static handle_t spy_wm_set_focus_last;

static int spy_stack_raise_calls;
static handle_t spy_stack_raise_last;

static int spy_cycle_focus_calls;
static bool spy_cycle_focus_last_forward;

static int spy_switch_workspace_calls;
static uint32_t spy_switch_workspace_last;

static int spy_switch_ws_rel_calls;
static int spy_switch_ws_rel_last_delta;

static int spy_move_to_ws_calls;
static handle_t spy_move_to_ws_last_client;
static uint32_t spy_move_to_ws_last_ws;
static bool spy_move_to_ws_last_follow;

static int spy_toggle_sticky_calls;
static handle_t spy_toggle_sticky_last;

static int spy_spawn_calls;
static char spy_spawn_last_cmd[256];

static int spy_exit_calls;
static int spy_exit_last_code;

// spies reset
static void reset_spies(void) {
  spy_menu_hide_calls = 0;
  spy_menu_handle_key_press_calls = 0;
  spy_client_close_calls = 0;
  spy_client_close_last = HANDLE_INVALID;

  spy_wm_set_focus_calls = 0;
  spy_wm_set_focus_last = HANDLE_INVALID;

  spy_stack_raise_calls = 0;
  spy_stack_raise_last = HANDLE_INVALID;

  spy_cycle_focus_calls = 0;
  spy_cycle_focus_last_forward = false;

  spy_switch_workspace_calls = 0;
  spy_switch_workspace_last = 0;

  spy_switch_ws_rel_calls = 0;
  spy_switch_ws_rel_last_delta = 0;

  spy_move_to_ws_calls = 0;
  spy_move_to_ws_last_client = HANDLE_INVALID;
  spy_move_to_ws_last_ws = 0;
  spy_move_to_ws_last_follow = false;

  spy_toggle_sticky_calls = 0;
  spy_toggle_sticky_last = HANDLE_INVALID;

  spy_spawn_calls = 0;
  spy_spawn_last_cmd[0] = 0;

  spy_exit_calls = 0;
  spy_exit_last_code = 0;

  g_restart_pending = 0;
}

// externs used by module
void menu_hide(server_t *s) {
  (void)s;
  spy_menu_hide_calls++;
}

void menu_handle_key_press(server_t *s, xcb_key_press_event_t *ev) {
  (void)s;
  (void)ev;
  spy_menu_handle_key_press_calls++;
}

void client_close(server_t *s, handle_t h) {
  (void)s;
  spy_client_close_calls++;
  spy_client_close_last = h;
}

void wm_set_focus(server_t *s, handle_t h) {
  (void)s;
  spy_wm_set_focus_calls++;
  spy_wm_set_focus_last = h;
}

void stack_raise(server_t *s, handle_t h) {
  (void)s;
  spy_stack_raise_calls++;
  spy_stack_raise_last = h;
}

void wm_switch_workspace(server_t *s, uint32_t ws) {
  (void)s;
  spy_switch_workspace_calls++;
  spy_switch_workspace_last = ws;
}

void wm_switch_workspace_relative(server_t *s, int delta) {
  (void)s;
  spy_switch_ws_rel_calls++;
  spy_switch_ws_rel_last_delta = delta;
}

void wm_client_move_to_workspace(server_t *s, handle_t h, uint32_t ws,
                                 bool follow) {
  (void)s;
  spy_move_to_ws_calls++;
  spy_move_to_ws_last_client = h;
  spy_move_to_ws_last_ws = ws;
  spy_move_to_ws_last_follow = follow;
}

void wm_client_toggle_sticky(server_t *s, handle_t h) {
  (void)s;
  spy_toggle_sticky_calls++;
  spy_toggle_sticky_last = h;
}

client_hot_t *server_chot(server_t *s, handle_t h) {
  // Very small lookup for tests: we stash pointers in (void*)conn for
  // convenience conn points to a NULL-terminated array of client_hot_t*
  client_hot_t **arr = (client_hot_t **)s->conn;
  if (!arr)
    return NULL;
  for (size_t i = 0; arr[i]; i++) {
    if (arr[i]->self == h)
      return arr[i];
  }
  return NULL;
}

// These are referenced by MOVE/RESIZE branches; tests avoid those actions
bool client_can_move(client_hot_t *hot) {
  (void)hot;
  return true;
}
bool client_can_resize(client_hot_t *hot) {
  (void)hot;
  return true;
}

typedef struct {
  uint32_t sequence;
} xcb_query_pointer_cookie_t;
xcb_query_pointer_cookie_t xcb_query_pointer(void *conn, uint32_t root) {
  (void)conn;
  (void)root;
  xcb_query_pointer_cookie_t ck = {.sequence = 1};
  return ck;
}

void cookie_jar_push(cookie_jar_t *jar, uint32_t seq, int kind, handle_t h,
                     uint32_t flags, uint64_t txn_id, void *cb) {
  (void)jar;
  (void)seq;
  (void)kind;
  (void)h;
  (void)flags;
  (void)txn_id;
  (void)cb;
}

void *wm_handle_reply = NULL;

void xcb_warp_pointer(void *conn, uint32_t src, uint32_t dst, int16_t src_x,
                      int16_t src_y, uint16_t src_w, uint16_t src_h,
                      int16_t dst_x, int16_t dst_y) {
  (void)conn;
  (void)src;
  (void)dst;
  (void)src_x;
  (void)src_y;
  (void)src_w;
  (void)src_h;
  (void)dst_x;
  (void)dst_y;
}

void wm_start_interaction(server_t *s, handle_t h, client_hot_t *hot,
                          bool start_move, int resize_dir, int16_t root_x,
                          int16_t root_y, uint32_t time, bool is_keyboard) {
  (void)s;
  (void)h;
  (void)hot;
  (void)start_move;
  (void)resize_dir;
  (void)root_x;
  (void)root_y;
  (void)time;
  (void)is_keyboard;
}

// -----------------------------
// XCB keysyms stub
// -----------------------------

static xcb_keysym_t g_fake_keysym;

xcb_keysym_t xcb_key_symbols_get_keysym(void *keysyms, xcb_keycode_t detail,
                                        int col) {
  (void)keysyms;
  (void)detail;
  (void)col;
  return g_fake_keysym;
}

// X11 KeySym constants used by module logic
// (Avoid pulling X11 headers into the test)
#define XK_Escape 0xff1b

// -----------------------------
// Override spawn() + exit() inside included module
// -----------------------------

static jmp_buf exit_jmp_buf;

static void test_spawn(const char *cmd) {
  spy_spawn_calls++;
  if (!cmd) {
    spy_spawn_last_cmd[0] = 0;
    return;
  }
  snprintf(spy_spawn_last_cmd, sizeof(spy_spawn_last_cmd), "%s", cmd);
}

static void test_exit(int code) {
  spy_exit_calls++;
  spy_exit_last_code = code;
  longjmp(exit_jmp_buf, 1);
}

// Make "static" helpers visible for direct unit testing
#define static /* expose statics */

// Redirect spawn/exit to spies
#define spawn test_spawn
#define exit test_exit

// Include the module under test
#include "../src/wm_input_keys.c"

// Undo macros to avoid leaking to other includes
#undef static
#undef spawn
#undef exit

// -----------------------------
// Helpers for building test servers/clients
// -----------------------------

static server_t make_server(key_binding_t **bindings, size_t nbindings,
                            client_hot_t **clients) {
  server_t s;
  memset(&s, 0, sizeof(s));

  s.keysyms = (void *)0x1; // non-null to allow wm_handle_key_press
  s.current_desktop = 0;
  s.focused_client = HANDLE_INVALID;

  // abuse conn as our client registry pointer for server_chot()
  s.conn = clients;

  // Count clients for active_clients.length
  size_t count = 0;
  if (clients) {
    while (clients[count])
      count++;
  }
  s.active_clients.length = count;

  list_init(&s.focus_history);

  s.config.key_bindings.length = nbindings;
  s.config.key_bindings.items = bindings;

  s.menu.visible = false;
  return s;
}

static client_hot_t make_client(handle_t id, int32_t desktop, bool sticky,
                                client_state_t st, window_type_t ty) {
  client_hot_t c;
  memset(&c, 0, sizeof(c));
  c.self = id;
  c.desktop = desktop;
  c.sticky = sticky;
  c.state = st;
  c.type = ty;
  list_init(&c.focus_node);
  c.server.x = 10;
  c.server.y = 10;
  c.server.w = 100;
  c.server.h = 50;
  return c;
}

// -----------------------------
// Tests
// -----------------------------

static void test_wm_clean_mods_masks_lock_num_scroll(void) {
  uint16_t in = 0;
  in |= XCB_MOD_MASK_LOCK;
  in |= XCB_MOD_MASK_2;
  in |= XCB_MOD_MASK_5;
  in |= (1u << 0); // some other mod bit

  uint32_t out = wm_clean_mods(in);
  assert((out & XCB_MOD_MASK_LOCK) == 0);
  assert((out & XCB_MOD_MASK_2) == 0);
  assert((out & XCB_MOD_MASK_5) == 0);
  assert((out & (1u << 0)) != 0);
}

static void test_safe_atoi_cases(void) {
  assert(safe_atoi(NULL) == 0);
  assert(safe_atoi("") == 0);
  assert(safe_atoi("abc") == 0);
  assert(safe_atoi("12") == 12);
  assert(safe_atoi("12x") == 12); // strtol stops at first non-digit
  assert(safe_atoi("-3") == 0);   // clamped
  assert(safe_atoi("-0") == 0);
}

static void test_is_focusable_rules(void) {
  client_hot_t c = make_client(10, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL);
  server_t s = make_server(NULL, 0, NULL);
  s.current_desktop = 0;

  // mapped, same desktop, normal -> true
  assert(is_focusable(&c, &s) == true);

  // unmapped -> false
  c.state = STATE_UNMAPPED;
  assert(is_focusable(&c, &s) == false);
  c.state = STATE_MAPPED;

  // other desktop, not sticky -> false
  c.desktop = 1;
  c.sticky = false;
  assert(is_focusable(&c, &s) == false);

  // other desktop but sticky -> true
  c.sticky = true;
  assert(is_focusable(&c, &s) == true);

  // rejected types -> false
  c.desktop = 0;
  c.sticky = false;
  c.type = WINDOW_TYPE_DOCK;
  assert(is_focusable(&c, &s) == false);

  c.type = WINDOW_TYPE_TOOLTIP;
  assert(is_focusable(&c, &s) == false);

  // show_desktop_hidden -> false if showing_desktop
  c.type = WINDOW_TYPE_NORMAL;
  c.show_desktop_hidden = true;
  s.showing_desktop = true;
  assert(is_focusable(&c, &s) == false);

  s.showing_desktop = false;
  assert(is_focusable(&c, &s) == true);
}

static void test_wm_cycle_focus_selects_next_focusable(void) {
  reset_spies();

  client_hot_t a = make_client(100, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL);
  client_hot_t b = make_client(200, 0, false, STATE_MAPPED,
                               WINDOW_TYPE_DOCK); // not focusable
  client_hot_t c =
      make_client(300, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL); // focusable

  // focus_history order: a, b, c
  client_hot_t *registry[] = {&a, &b, &c, NULL};
  server_t s = make_server(NULL, 0, registry);
  list_init(&s.focus_history);
  list_push_back(&s.focus_history, &a.focus_node);
  list_push_back(&s.focus_history, &b.focus_node);
  list_push_back(&s.focus_history, &c.focus_node);

  // start from a (focused)
  s.focused_client = a.self;

  wm_cycle_focus(&s, true);

  assert(spy_wm_set_focus_calls == 1);
  assert(spy_wm_set_focus_last == c.self);
  assert(spy_stack_raise_calls == 1);
  assert(spy_stack_raise_last == c.self);
}

static void test_wm_cycle_focus_no_focusable_no_calls(void) {
  reset_spies();

  client_hot_t a = make_client(100, 0, false, STATE_MAPPED, WINDOW_TYPE_DOCK);
  client_hot_t b =
      make_client(200, 0, false, STATE_MAPPED, WINDOW_TYPE_TOOLTIP);

  client_hot_t *registry[] = {&a, &b, NULL};
  server_t s = make_server(NULL, 0, registry);
  list_init(&s.focus_history);
  list_push_back(&s.focus_history, &a.focus_node);
  list_push_back(&s.focus_history, &b.focus_node);

  s.focused_client = a.self;

  wm_cycle_focus(&s, true);

  assert(spy_wm_set_focus_calls == 0);
  assert(spy_stack_raise_calls == 0);
}

static void test_key_press_menu_delegates_to_menu(void) {
  reset_spies();

  server_t s = make_server(NULL, 0, NULL);
  s.menu.visible = true;

  xcb_key_press_event_t ev;
  ev.detail = 9;
  ev.state = 0;

  g_fake_keysym = XK_Escape;

  wm_handle_key_press(&s, &ev);

  assert(spy_menu_handle_key_press_calls == 1);
  // Should NOT hide menu here (menu handle does it)
  assert(spy_menu_hide_calls == 0);
  assert(spy_client_close_calls == 0);
}

static void test_key_press_matches_binding_with_ignored_mods(void) {
  reset_spies();

  // Binding expects mods without lock/num/scroll
  key_binding_t bind = {.keysym = 0x1234,
                        .modifiers = (1u << 0), // some mod bit
                        .action = ACTION_RESTART,
                        .exec_cmd = NULL};
  key_binding_t *binds[] = {&bind};
  key_binding_t **binds_ptr = binds;

  server_t s = make_server(binds_ptr, 1, NULL);

  xcb_key_press_event_t ev;
  ev.detail = 10;
  ev.state = (uint16_t)((1u << 0) | XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2 |
                        XCB_MOD_MASK_5);

  g_fake_keysym = 0x1234;

  wm_handle_key_press(&s, &ev);

  assert(g_restart_pending == 1);
}

static void test_key_press_action_close_calls_client_close(void) {
  reset_spies();

  key_binding_t bind = {.keysym = 0x2222,
                        .modifiers = 0,
                        .action = ACTION_CLOSE,
                        .exec_cmd = NULL};
  key_binding_t *binds[] = {&bind};
  server_t s = make_server(binds, 1, NULL);

  s.focused_client = 0xBEEF;

  xcb_key_press_event_t ev;
  ev.detail = 11;
  ev.state = 0;

  g_fake_keysym = 0x2222;

  wm_handle_key_press(&s, &ev);

  assert(spy_client_close_calls == 1);
  assert(spy_client_close_last == 0xBEEF);
}

static void test_key_press_action_focus_next_prev_dispatch(void) {
  reset_spies();

  client_hot_t a = make_client(10, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL);
  client_hot_t b = make_client(20, 0, false, STATE_MAPPED, WINDOW_TYPE_NORMAL);
  client_hot_t *registry[] = {&a, &b, NULL};

  key_binding_t bind_next = {.keysym = 0x3333,
                             .modifiers = 0,
                             .action = ACTION_FOCUS_NEXT,
                             .exec_cmd = NULL};
  key_binding_t *binds[] = {&bind_next};

  server_t s = make_server(binds, 1, registry);
  list_push_back(&s.focus_history, &a.focus_node);
  list_push_back(&s.focus_history, &b.focus_node);
  s.focused_client = a.self;

  xcb_key_press_event_t ev = {.detail = 12, .state = 0};
  g_fake_keysym = 0x3333;

  wm_handle_key_press(&s, &ev);

  assert(spy_wm_set_focus_calls == 1);
  assert(spy_wm_set_focus_last == b.self);
}

static void test_key_press_action_workspace_uses_safe_atoi(void) {
  reset_spies();

  key_binding_t bind = {.keysym = 0x4444,
                        .modifiers = 0,
                        .action = ACTION_WORKSPACE,
                        .exec_cmd = "2"};
  key_binding_t *binds[] = {&bind};

  server_t s = make_server(binds, 1, NULL);

  xcb_key_press_event_t ev = {.detail = 13, .state = 0};
  g_fake_keysym = 0x4444;

  wm_handle_key_press(&s, &ev);

  assert(spy_switch_workspace_calls == 1);
  assert(spy_switch_workspace_last == 2u);
}

static void test_key_press_action_move_to_workspace_follow(void) {
  reset_spies();

  key_binding_t bind = {.keysym = 0x5555,
                        .modifiers = 0,
                        .action = ACTION_MOVE_TO_WORKSPACE_FOLLOW,
                        .exec_cmd = "7"};
  key_binding_t *binds[] = {&bind};

  server_t s = make_server(binds, 1, NULL);
  s.focused_client = 0xCAFE;

  xcb_key_press_event_t ev = {.detail = 14, .state = 0};
  g_fake_keysym = 0x5555;

  wm_handle_key_press(&s, &ev);

  assert(spy_move_to_ws_calls == 1);
  assert(spy_move_to_ws_last_client == 0xCAFE);
  assert(spy_move_to_ws_last_ws == 7u);
  assert(spy_move_to_ws_last_follow == true);
}

static void test_key_press_action_toggle_sticky(void) {
  reset_spies();

  key_binding_t bind = {.keysym = 0x6666,
                        .modifiers = 0,
                        .action = ACTION_TOGGLE_STICKY,
                        .exec_cmd = NULL};
  key_binding_t *binds[] = {&bind};

  server_t s = make_server(binds, 1, NULL);
  s.focused_client = 0x123;

  xcb_key_press_event_t ev = {.detail = 15, .state = 0};
  g_fake_keysym = 0x6666;

  wm_handle_key_press(&s, &ev);

  assert(spy_toggle_sticky_calls == 1);
  assert(spy_toggle_sticky_last == 0x123);
}

static void test_key_press_action_exec_and_terminal_spawn(void) {
  reset_spies();

  key_binding_t bind_exec = {.keysym = 0x7777,
                             .modifiers = 0,
                             .action = ACTION_EXEC,
                             .exec_cmd = "echo hi"};
  key_binding_t bind_term = {.keysym = 0x8888,
                             .modifiers = 0,
                             .action = ACTION_TERMINAL,
                             .exec_cmd = NULL};

  key_binding_t *binds1[] = {&bind_exec};
  server_t s1 = make_server(binds1, 1, NULL);

  xcb_key_press_event_t ev1 = {.detail = 16, .state = 0};
  g_fake_keysym = 0x7777;
  wm_handle_key_press(&s1, &ev1);

  assert(spy_spawn_calls == 1);
  assert(strcmp(spy_spawn_last_cmd, "echo hi") == 0);

  reset_spies();

  key_binding_t *binds2[] = {&bind_term};
  server_t s2 = make_server(binds2, 1, NULL);

  xcb_key_press_event_t ev2 = {.detail = 17, .state = 0};
  g_fake_keysym = 0x8888;
  wm_handle_key_press(&s2, &ev2);

  assert(spy_spawn_calls == 1);
  assert(strstr(spy_spawn_last_cmd, "st") != NULL);
}

static void test_key_press_action_exit_intercepted(void) {
  reset_spies();

  key_binding_t bind = {.keysym = 0x9999,
                        .modifiers = 0,
                        .action = ACTION_EXIT,
                        .exec_cmd = NULL};
  key_binding_t *binds[] = {&bind};
  server_t s = make_server(binds, 1, NULL);

  xcb_key_press_event_t ev = {.detail = 18, .state = 0};
  g_fake_keysym = 0x9999;

  if (setjmp(exit_jmp_buf) == 0) {
    wm_handle_key_press(&s, &ev);
  }

  assert(spy_exit_calls == 1);
  assert(spy_exit_last_code == 0);
}

int main(void) {
  test_wm_clean_mods_masks_lock_num_scroll();
  test_safe_atoi_cases();
  test_is_focusable_rules();
  test_wm_cycle_focus_selects_next_focusable();
  test_wm_cycle_focus_no_focusable_no_calls();
  test_key_press_menu_delegates_to_menu();
  test_key_press_matches_binding_with_ignored_mods();
  test_key_press_action_close_calls_client_close();
  test_key_press_action_focus_next_prev_dispatch();
  test_key_press_action_workspace_uses_safe_atoi();
  test_key_press_action_move_to_workspace_follow();
  test_key_press_action_toggle_sticky();
  test_key_press_action_exec_and_terminal_spawn();
  test_key_press_action_exit_intercepted();

  puts("test_wm_input_keys: OK");
  return 0;
}
