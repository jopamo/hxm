#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <xcb/xcb_icccm.h>
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
extern xcb_atom_t stub_last_prop_atom;
extern uint32_t stub_last_prop_len;
extern uint8_t stub_last_prop_data[4096];
extern int stub_map_window_count;
extern int stub_unmap_window_count;
extern xcb_window_t stub_last_mapped_window;
extern xcb_window_t stub_last_unmapped_window;
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

static bool atom_in_state_list(const xcb_atom_t* atoms_list, uint32_t count, xcb_atom_t needle);

static const struct stub_prop_call* find_prop_call(xcb_window_t win, xcb_atom_t atom, bool deleted) {
  for (int i = stub_prop_calls_len - 1; i >= 0; i--) {
    if (stub_prop_calls[i].window == win && stub_prop_calls[i].atom == atom && stub_prop_calls[i].deleted == deleted) {
      return &stub_prop_calls[i];
    }
  }
  return NULL;
}

static void assert_hidden_state_flag(const client_hot_t* hot, bool expected, xcb_atom_t net_wm_state, xcb_atom_t hidden_atom) {
  const struct stub_prop_call* state = find_prop_call(hot->xid, net_wm_state, false);
  assert(state != NULL);
  bool has_hidden = atom_in_state_list((const xcb_atom_t*)state->data, state->len, hidden_atom);
  assert(has_hidden == expected);
}

static void assert_wm_state_value(const client_hot_t* hot, xcb_atom_t wm_state_atom, uint32_t expected) {
  const struct stub_prop_call* wm_state = find_prop_call(hot->xid, wm_state_atom, false);
  assert(wm_state != NULL);
  assert(wm_state->len == 2);
  const uint32_t* vals = (const uint32_t*)wm_state->data;
  assert(vals[0] == expected);
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
  s->desktop_count = 3;
  s->current_desktop = 0;
  s->workarea = (rect_t){0, 0, 1920, 1080};

  arena_init(&s->tick_arena, 4096);
  cookie_jar_init(&s->cookie_jar);
  slotmap_init(&s->clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
  small_vec_init(&s->active_clients);
  hash_map_init(&s->window_to_client);
  hash_map_init(&s->frame_to_client);
  list_init(&s->focus_history);

  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_init(&s->layers[i]);
}

static void cleanup_server(server_t* s) {
  for (uint32_t i = 1; i < s->clients.cap; i++) {
    if (!s->clients.hdr[i].live)
      continue;
    handle_t h = handle_make(i, s->clients.hdr[i].gen);
    client_hot_t* hot = server_chot(s, h);
    client_cold_t* cold = server_ccold(s, h);
    if (cold)
      arena_destroy(&cold->string_arena);
    if (hot) {
      client_render_payload_destroy(cold);
    }
  }
  cookie_jar_destroy(&s->cookie_jar);
  slotmap_destroy(&s->clients);
  small_vec_destroy(&s->active_clients);
  hash_map_destroy(&s->window_to_client);
  hash_map_destroy(&s->frame_to_client);
  free(s->monitors);
  s->monitors = NULL;
  s->monitor_count = 0;
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_destroy(&s->layers[i]);
  arena_destroy(&s->tick_arena);
  config_destroy(&s->config);
  xcb_disconnect(s->conn);
  pango_cairo_font_map_set_default(NULL);
  FcFini();
}

static handle_t add_mapped_client(server_t* s, xcb_window_t win, xcb_window_t frame) {
  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  client_cold_t* cold = (client_cold_t*)cold_ptr;
  memset(hot, 0, sizeof(*hot));
  memset(cold, 0, sizeof(*cold));

  client_render_payload_init(cold);
  arena_init(&cold->string_arena, 128);

  hot->self = h;
  hot->xid = win;
  hot->frame = frame;
  hot->state = STATE_MAPPED;
  hot->type = WINDOW_TYPE_NORMAL;
  hot->layer = LAYER_NORMAL;
  hot->base_layer = LAYER_NORMAL;
  hot->stacking_index = -1;
  hot->stacking_layer = -1;
  hot->server = (rect_t){10, 10, 200, 150};
  hot->desired = hot->server;

  list_init(&hot->focus_node);
  list_init(&hot->transients_head);
  list_init(&hot->transient_sibling);

  hash_map_insert(&s->window_to_client, win, handle_to_ptr(h));
  hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));
  small_vec_push(&s->active_clients, handle_to_ptr(h));

  return h;
}

static void send_net_active_window(server_t* s, xcb_window_t win, uint32_t source, uint32_t timestamp) {
  xcb_client_message_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = win;
  ev.type = atoms._NET_ACTIVE_WINDOW;
  ev.data.data32[0] = source;
  ev.data.data32[1] = timestamp;
  ev.data.data32[2] = XCB_NONE;
  wm_handle_client_message(s, &ev);
}

static void test_active_window_updates(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_ACTIVE_WINDOW = 200;

  handle_t h = add_mapped_client(&s, 1001, 1101);
  client_hot_t* hot = server_chot(&s, h);

  wm_set_focus(&s, h);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* call = find_prop_call(s.root, atoms._NET_ACTIVE_WINDOW, false);
  assert(call != NULL);
  assert(call->len == 1);
  const uint32_t* val = (const uint32_t*)call->data;
  assert(val[0] == hot->xid);

  wm_set_focus(&s, HANDLE_INVALID);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* del = find_prop_call(s.root, atoms._NET_ACTIVE_WINDOW, true);
  assert(del != NULL);

  hot->state = STATE_UNMAPPED;
  s.focused_client = h;
  s.root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* del_unmapped = find_prop_call(s.root, atoms._NET_ACTIVE_WINDOW, true);
  assert(del_unmapped != NULL);

  printf("test_active_window_updates passed\n");
  cleanup_server(&s);
}

static void test_active_window_exits_show_desktop_mode(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  s.desktop_count = 2;
  s.current_desktop = 0;

  handle_t h1 = add_mapped_client(&s, 1201, 1301);
  handle_t h2 = add_mapped_client(&s, 1202, 1302);
  client_hot_t* a = server_chot(&s, h1);
  client_hot_t* b = server_chot(&s, h2);
  assert(a && b);

  a->manage_phase = MANAGE_DONE;
  b->manage_phase = MANAGE_DONE;
  a->desktop = 0;
  b->desktop = 0;
  a->user_time = 100;
  b->user_time = 100;

  wm_set_showing_desktop(&s, true);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* shown = find_prop_call(s.root, atoms._NET_SHOWING_DESKTOP, false);
  assert(shown != NULL);
  assert(shown->len == 1);
  assert(((const uint32_t*)shown->data)[0] == 1u);
  assert(s.showing_desktop == true);
  assert(a->show_desktop_hidden == true);
  assert(b->show_desktop_hidden == true);
  assert(a->state == STATE_UNMAPPED);
  assert(b->state == STATE_UNMAPPED);

  send_net_active_window(&s, b->xid, 2, 200);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* hidden = find_prop_call(s.root, atoms._NET_SHOWING_DESKTOP, false);
  assert(hidden != NULL);
  assert(hidden->len == 1);
  assert(((const uint32_t*)hidden->data)[0] == 0u);
  assert(s.showing_desktop == false);
  assert(a->show_desktop_hidden == false);
  assert(b->show_desktop_hidden == false);
  assert(a->state == STATE_MAPPED);
  assert(b->state == STATE_MAPPED);
  assert(s.focused_client == h2);

  printf("test_active_window_exits_show_desktop_mode passed\n");
  cleanup_server(&s);
}

static void test_restore_exits_show_desktop_mode(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  handle_t h = add_mapped_client(&s, 1401, 1501);
  client_hot_t* hot = server_chot(&s, h);
  assert(hot);
  hot->manage_phase = MANAGE_DONE;
  hot->desktop = 0;

  wm_set_showing_desktop(&s, true);
  wm_flush_dirty(&s, monotonic_time_ns());

  assert(s.showing_desktop == true);
  assert(hot->show_desktop_hidden == true);
  assert(hot->state == STATE_UNMAPPED);

  wm_client_restore(&s, h);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* hidden = find_prop_call(s.root, atoms._NET_SHOWING_DESKTOP, false);
  assert(hidden != NULL);
  assert(hidden->len == 1);
  assert(((const uint32_t*)hidden->data)[0] == 0u);
  assert(s.showing_desktop == false);
  assert(hot->show_desktop_hidden == false);
  assert(hot->state == STATE_MAPPED);

  printf("test_restore_exits_show_desktop_mode passed\n");
  cleanup_server(&s);
}

static void test_client_list_add_remove(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_CLIENT_LIST = 300;
  atoms._NET_CLIENT_LIST_STACKING = 301;

  handle_t h1 = add_mapped_client(&s, 2001, 2101);
  handle_t h2 = add_mapped_client(&s, 2002, 2102);
  stack_raise(&s, h1);
  stack_raise(&s, h2);

  s.root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING;
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* list = find_prop_call(s.root, atoms._NET_CLIENT_LIST, false);
  assert(list != NULL);
  assert(list->len == 2);
  const uint32_t* list_vals = (const uint32_t*)list->data;
  assert(list_vals[0] == 2001);
  assert(list_vals[1] == 2002);

  client_unmanage(&s, h1);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* list2 = find_prop_call(s.root, atoms._NET_CLIENT_LIST, false);
  assert(list2 != NULL);
  assert(list2->len == 1);
  list_vals = (const uint32_t*)list2->data;
  assert(list_vals[0] == 2002);

  printf("test_client_list_add_remove passed\n");
  cleanup_server(&s);
}

static void test_client_list_remove_keeps_order(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_CLIENT_LIST = 330;
  atoms._NET_CLIENT_LIST_STACKING = 331;

  handle_t h1 = add_mapped_client(&s, 3001, 3101);
  handle_t h2 = add_mapped_client(&s, 3002, 3102);
  handle_t h3 = add_mapped_client(&s, 3003, 3103);
  (void)h2;
  (void)h3;

  stack_raise(&s, h1);
  s.root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING;
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* list = find_prop_call(s.root, atoms._NET_CLIENT_LIST, false);
  assert(list != NULL);
  assert(list->len == 3);
  const uint32_t* vals = (const uint32_t*)list->data;
  assert(vals[0] == 3001);
  assert(vals[1] == 3002);
  assert(vals[2] == 3003);

  client_unmanage(&s, h1);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* list_after = find_prop_call(s.root, atoms._NET_CLIENT_LIST, false);
  assert(list_after != NULL);
  assert(list_after->len == 2);
  vals = (const uint32_t*)list_after->data;
  assert(vals[0] == 3002);
  assert(vals[1] == 3003);

  printf("test_client_list_remove_keeps_order passed\n");
  cleanup_server(&s);
}

static void test_client_list_includes_all_managed(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_CLIENT_LIST = 320;
  atoms._NET_CLIENT_LIST_STACKING = 321;
  atoms._NET_WM_STATE = 322;
  atoms._NET_WM_STATE_SKIP_TASKBAR = 323;
  atoms._NET_WM_STATE_SKIP_PAGER = 324;

  handle_t h1 = add_mapped_client(&s, 7001, 7101);
  handle_t h2 = add_mapped_client(&s, 7002, 7102);
  handle_t h3 = add_mapped_client(&s, 7003, 7103);
  handle_t h4 = add_mapped_client(&s, 7004, 7104);

  client_hot_t* dock = server_chot(&s, h4);
  dock->type = WINDOW_TYPE_DOCK;

  stack_raise(&s, h1);
  stack_raise(&s, h2);
  stack_raise(&s, h3);
  stack_raise(&s, h4);

  wm_client_update_state(&s, h2, 1, atoms._NET_WM_STATE_SKIP_TASKBAR);
  wm_client_update_state(&s, h3, 1, atoms._NET_WM_STATE_SKIP_PAGER);
  wm_flush_dirty(&s, monotonic_time_ns());

  s.root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING;
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* list = find_prop_call(s.root, atoms._NET_CLIENT_LIST, false);
  assert(list != NULL);
  assert(list->len == 4);
  const uint32_t* list_vals = (const uint32_t*)list->data;
  assert(list_vals[0] == 7001);
  assert(list_vals[1] == 7002);
  assert(list_vals[2] == 7003);
  assert(list_vals[3] == 7004);

  const struct stub_prop_call* list_stack = find_prop_call(s.root, atoms._NET_CLIENT_LIST_STACKING, false);
  assert(list_stack != NULL);
  assert(list_stack->len == 4);
  const uint32_t* stack_vals = (const uint32_t*)list_stack->data;
  assert(stack_vals[0] == 7001);
  assert(stack_vals[1] == 7002);
  assert(stack_vals[2] == 7003);
  assert(stack_vals[3] == 7004);

  printf("test_client_list_includes_all_managed passed\n");
  cleanup_server(&s);
}

static void test_desktop_props_publish_and_switch(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_NUMBER_OF_DESKTOPS = 400;
  atoms._NET_CURRENT_DESKTOP = 401;

  s.desktop_count = 3;
  s.current_desktop = 1;

  wm_publish_desktop_props(&s);

  const struct stub_prop_call* num = find_prop_call(s.root, atoms._NET_NUMBER_OF_DESKTOPS, false);
  assert(num != NULL);
  const uint32_t* num_vals = (const uint32_t*)num->data;
  assert(num_vals[0] == 3);

  const struct stub_prop_call* cur = find_prop_call(s.root, atoms._NET_CURRENT_DESKTOP, false);
  assert(cur != NULL);
  const uint32_t* cur_vals = (const uint32_t*)cur->data;
  assert(cur_vals[0] == 1);

  wm_switch_workspace(&s, 2);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* cur2 = find_prop_call(s.root, atoms._NET_CURRENT_DESKTOP, false);
  assert(cur2 != NULL);
  cur_vals = (const uint32_t*)cur2->data;
  assert(cur_vals[0] == 2);

  printf("test_desktop_props_publish_and_switch passed\n");
  cleanup_server(&s);
}

static void test_net_wm_desktop_reply_sets_sticky_and_desktop(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_DESKTOP = 410;

  handle_t h = add_mapped_client(&s, 8001, 8101);
  client_hot_t* hot = server_chot(&s, h);
  hot->manage_phase = MANAGE_PHASE1;
  hot->desktop = 0;
  hot->sticky = false;

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_DESKTOP;

  struct {
    xcb_get_property_reply_t r;
    uint32_t data[1];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.value_len = 1;
  reply.r.type = XCB_ATOM_CARDINAL;

  reply.data[0] = 0xFFFFFFFFu;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(hot->net_wm_desktop_seen);
  assert(hot->sticky == true);
  assert(hot->desktop == -1);

  reply.data[0] = 2;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(hot->sticky == false);
  assert(hot->desktop == 2);

  printf("test_net_wm_desktop_reply_sets_sticky_and_desktop passed\n");
  cleanup_server(&s);
}

static void test_net_wm_desktop_reply_clamps_out_of_range(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_DESKTOP = 411;
  s.desktop_count = 2;
  s.current_desktop = 1;

  handle_t h = add_mapped_client(&s, 8002, 8102);
  client_hot_t* hot = server_chot(&s, h);
  hot->manage_phase = MANAGE_PHASE1;

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_DESKTOP;

  struct {
    xcb_get_property_reply_t r;
    uint32_t data[1];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.value_len = 1;
  reply.r.type = XCB_ATOM_CARDINAL;
  reply.data[0] = 7;

  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(hot->net_wm_desktop_seen);
  assert(hot->sticky == false);
  assert(hot->desktop == (int32_t)s.current_desktop);

  printf("test_net_wm_desktop_reply_clamps_out_of_range passed\n");
  cleanup_server(&s);
}

static void test_net_wm_desktop_reply_moves_after_manage(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_DESKTOP = 412;
  s.desktop_count = 3;
  s.current_desktop = 0;

  handle_t h = add_mapped_client(&s, 8003, 8103);
  client_hot_t* hot = server_chot(&s, h);
  hot->manage_phase = MANAGE_DONE;
  hot->desktop = 0;
  hot->sticky = false;

  stub_map_window_count = 0;
  stub_unmap_window_count = 0;

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_DESKTOP;

  struct {
    xcb_get_property_reply_t r;
    uint32_t data[1];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.value_len = 1;
  reply.r.type = XCB_ATOM_CARDINAL;
  reply.data[0] = 2;

  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(hot->desktop == 2);
  assert(hot->sticky == false);
  assert(hot->net_wm_desktop_seen);

  wm_flush_dirty(&s, monotonic_time_ns());

  assert(stub_unmap_window_count == 1);
  assert(stub_last_unmapped_window == hot->frame);

  const struct stub_prop_call* desk = find_prop_call(hot->xid, atoms._NET_WM_DESKTOP, false);
  assert(desk != NULL);
  assert(((uint32_t*)desk->data)[0] == 2);

  printf("test_net_wm_desktop_reply_moves_after_manage passed\n");
  cleanup_server(&s);
}

static void test_window_type_desktop_defaults_sticky(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_WINDOW_TYPE = 420;
  atoms._NET_WM_WINDOW_TYPE_DESKTOP = 421;

  handle_t h = add_mapped_client(&s, 8010, 8110);
  client_hot_t* hot = server_chot(&s, h);
  hot->manage_phase = MANAGE_PHASE1;
  hot->sticky = false;
  hot->desktop = 0;

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_WINDOW_TYPE;

  struct {
    xcb_get_property_reply_t r;
    xcb_atom_t types[1];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.value_len = 1;
  reply.r.type = XCB_ATOM_ATOM;
  reply.types[0] = atoms._NET_WM_WINDOW_TYPE_DESKTOP;

  wm_handle_reply(&s, &slot, &reply.r, NULL);

  assert(hot->type == WINDOW_TYPE_DESKTOP);
  assert(hot->layer == LAYER_DESKTOP);
  assert(hot->base_layer == LAYER_DESKTOP);
  assert(hot->flags & CLIENT_FLAG_UNDECORATED);
  assert(hot->skip_taskbar == true);
  assert(hot->skip_pager == true);
  assert(hot->sticky == true);
  assert(hot->desktop == -1);

  printf("test_window_type_desktop_defaults_sticky passed\n");
  cleanup_server(&s);
}

static void test_window_type_desktop_respects_net_wm_desktop(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_DESKTOP = 430;
  atoms._NET_WM_WINDOW_TYPE = 431;
  atoms._NET_WM_WINDOW_TYPE_DESKTOP = 432;

  handle_t h = add_mapped_client(&s, 8020, 8120);
  client_hot_t* hot = server_chot(&s, h);
  hot->manage_phase = MANAGE_PHASE1;

  cookie_slot_t desk_slot = {0};
  desk_slot.client = h;
  desk_slot.type = COOKIE_GET_PROPERTY;
  desk_slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_DESKTOP;

  struct {
    xcb_get_property_reply_t r;
    uint32_t data[1];
  } desk_reply;
  memset(&desk_reply, 0, sizeof(desk_reply));
  desk_reply.r.format = 32;
  desk_reply.r.value_len = 1;
  desk_reply.r.type = XCB_ATOM_CARDINAL;
  desk_reply.data[0] = 1;

  wm_handle_reply(&s, &desk_slot, &desk_reply.r, NULL);
  assert(hot->net_wm_desktop_seen);
  assert(hot->sticky == false);
  assert(hot->desktop == 1);

  cookie_slot_t type_slot = {0};
  type_slot.client = h;
  type_slot.type = COOKIE_GET_PROPERTY;
  type_slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_WINDOW_TYPE;

  struct {
    xcb_get_property_reply_t r;
    xcb_atom_t types[1];
  } type_reply;
  memset(&type_reply, 0, sizeof(type_reply));
  type_reply.r.format = 32;
  type_reply.r.value_len = 1;
  type_reply.r.type = XCB_ATOM_ATOM;
  type_reply.types[0] = atoms._NET_WM_WINDOW_TYPE_DESKTOP;

  wm_handle_reply(&s, &type_slot, &type_reply.r, NULL);

  assert(hot->type == WINDOW_TYPE_DESKTOP);
  assert(hot->layer == LAYER_DESKTOP);
  assert(hot->base_layer == LAYER_DESKTOP);
  assert(hot->skip_taskbar == true);
  assert(hot->skip_pager == true);
  assert(hot->sticky == false);
  assert(hot->desktop == 1);

  printf("test_window_type_desktop_respects_net_wm_desktop passed\n");
  cleanup_server(&s);
}

static void test_desktop_type_then_net_wm_desktop_updates(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_DESKTOP = 440;
  atoms._NET_WM_WINDOW_TYPE = 441;
  atoms._NET_WM_WINDOW_TYPE_DESKTOP = 442;

  handle_t h = add_mapped_client(&s, 8030, 8130);
  client_hot_t* hot = server_chot(&s, h);
  hot->manage_phase = MANAGE_PHASE1;

  cookie_slot_t type_slot = {0};
  type_slot.client = h;
  type_slot.type = COOKIE_GET_PROPERTY;
  type_slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_WINDOW_TYPE;

  struct {
    xcb_get_property_reply_t r;
    xcb_atom_t types[1];
  } type_reply;
  memset(&type_reply, 0, sizeof(type_reply));
  type_reply.r.format = 32;
  type_reply.r.value_len = 1;
  type_reply.r.type = XCB_ATOM_ATOM;
  type_reply.types[0] = atoms._NET_WM_WINDOW_TYPE_DESKTOP;

  wm_handle_reply(&s, &type_slot, &type_reply.r, NULL);
  assert(hot->sticky == true);
  assert(hot->desktop == -1);

  cookie_slot_t desk_slot = {0};
  desk_slot.client = h;
  desk_slot.type = COOKIE_GET_PROPERTY;
  desk_slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_DESKTOP;

  struct {
    xcb_get_property_reply_t r;
    uint32_t data[1];
  } desk_reply;
  memset(&desk_reply, 0, sizeof(desk_reply));
  desk_reply.r.format = 32;
  desk_reply.r.value_len = 1;
  desk_reply.r.type = XCB_ATOM_CARDINAL;
  desk_reply.data[0] = 2;

  wm_handle_reply(&s, &desk_slot, &desk_reply.r, NULL);
  assert(hot->sticky == false);
  assert(hot->desktop == 2);
  assert(hot->net_wm_desktop_seen);

  printf("test_desktop_type_then_net_wm_desktop_updates passed\n");
  cleanup_server(&s);
}

static void test_strut_updates_workarea(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WORKAREA = 500;
  atoms._NET_WM_STRUT_PARTIAL = 501;
  s.desktop_count = 1;

  handle_t h = add_mapped_client(&s, 3001, 3101);
  client_cold_t* cold = server_ccold(&s, h);
  assert(cold != NULL);

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)3001 << 32) | atoms._NET_WM_STRUT_PARTIAL;

  struct {
    xcb_get_property_reply_t r;
    uint32_t data[12];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.value_len = 12;
  reply.r.type = XCB_ATOM_CARDINAL;
  reply.data[0] = 100;   // left
  reply.data[4] = 0;     // left_start_y
  reply.data[5] = 1080;  // left_end_y

  wm_handle_reply(&s, &slot, &reply.r, NULL);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* wa = find_prop_call(s.root, atoms._NET_WORKAREA, false);
  assert(wa != NULL);
  const uint32_t* vals = (const uint32_t*)wa->data;
  assert(vals[0] == 100);
  assert(vals[1] == 0);
  assert(vals[2] == 1820);
  assert(vals[3] == 1080);

  reply.r.format = 0;
  reply.r.value_len = 0;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* wa2 = find_prop_call(s.root, atoms._NET_WORKAREA, false);
  assert(wa2 != NULL);
  vals = (const uint32_t*)wa2->data;
  assert(vals[0] == 0);
  assert(vals[1] == 0);
  assert(vals[2] == 1920);
  assert(vals[3] == 1080);

  printf("test_strut_updates_workarea passed\n");
  cleanup_server(&s);
}

static void test_workarea_publishes_per_desktop_array(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WORKAREA = 550;
  s.desktop_count = 3;
  s.current_desktop = 0;

  handle_t h_top = add_mapped_client(&s, 3201, 3301);
  handle_t h_left = add_mapped_client(&s, 3202, 3302);
  client_hot_t* top_hot = server_chot(&s, h_top);
  client_hot_t* left_hot = server_chot(&s, h_left);
  client_cold_t* top_cold = server_ccold(&s, h_top);
  client_cold_t* left_cold = server_ccold(&s, h_left);
  assert(top_hot && left_hot && top_cold && left_cold);

  top_hot->type = WINDOW_TYPE_DOCK;
  top_hot->desktop = 0;
  top_hot->sticky = false;
  top_cold->strut.top = 30;
  top_cold->strut_full_active = true;

  left_hot->type = WINDOW_TYPE_DOCK;
  left_hot->desktop = 1;
  left_hot->sticky = false;
  left_cold->strut.left = 40;
  left_cold->strut_full_active = true;

  s.root_dirty |= ROOT_DIRTY_WORKAREA;
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* wa = find_prop_call(s.root, atoms._NET_WORKAREA, false);
  assert(wa != NULL);
  assert(wa->len == 12);
  const uint32_t* vals = (const uint32_t*)wa->data;
  assert(vals[0] == 0);
  assert(vals[1] == 30);
  assert(vals[2] == 1920);
  assert(vals[3] == 1050);
  assert(vals[4] == 40);
  assert(vals[5] == 0);
  assert(vals[6] == 1880);
  assert(vals[7] == 1080);
  assert(vals[8] == 0);
  assert(vals[9] == 0);
  assert(vals[10] == 1920);
  assert(vals[11] == 1080);

  printf("test_workarea_publishes_per_desktop_array passed\n");
  cleanup_server(&s);
}

static void test_maximize_and_fullscreen_use_client_desktop_monitor_workarea(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_STATE_FULLSCREEN = 560;
  s.desktop_count = 2;
  s.current_desktop = 0;
  s.config.theme.border_width = 0;
  s.config.theme.title_height = 0;
  s.config.fullscreen_use_workarea = true;

  s.monitors = calloc(2, sizeof(monitor_t));
  assert(s.monitors != NULL);
  s.monitor_count = 2;
  s.monitors[0].geom = (rect_t){0, 0, 960, 1080};
  s.monitors[1].geom = (rect_t){960, 0, 960, 1080};
  s.monitors[0].workarea = s.monitors[0].geom;
  s.monitors[1].workarea = s.monitors[1].geom;

  handle_t h_dock = add_mapped_client(&s, 3401, 3501);
  client_hot_t* dock_hot = server_chot(&s, h_dock);
  client_cold_t* dock_cold = server_ccold(&s, h_dock);
  assert(dock_hot && dock_cold);
  dock_hot->type = WINDOW_TYPE_DOCK;
  dock_hot->desktop = 1;
  dock_hot->sticky = false;
  dock_cold->strut.top = 50;
  dock_cold->strut_partial_active = true;
  dock_cold->strut.top_start_x = 960;
  dock_cold->strut.top_end_x = 1920;

  handle_t h = add_mapped_client(&s, 3402, 3502);
  client_hot_t* hot = server_chot(&s, h);
  assert(hot != NULL);
  hot->desktop = 1;
  hot->sticky = false;
  hot->server = (rect_t){1200, 200, 300, 200};
  hot->desired = hot->server;

  wm_client_set_maximize(&s, hot, true, true);
  assert(hot->desired.x == 960);
  assert(hot->desired.y == 50);
  assert(hot->desired.w == 960);
  assert(hot->desired.h == 1030);

  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_FULLSCREEN);
  assert(hot->layer == LAYER_FULLSCREEN);
  assert(hot->desired.x == 960);
  assert(hot->desired.y == 50);
  assert(hot->desired.w == 960);
  assert(hot->desired.h == 1030);

  printf("test_maximize_and_fullscreen_use_client_desktop_monitor_workarea passed\n");
  cleanup_server(&s);
}

static void test_window_type_dock_layer(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_WINDOW_TYPE = 600;
  atoms._NET_WM_WINDOW_TYPE_DOCK = 601;

  handle_t h = add_mapped_client(&s, 4001, 4101);
  client_hot_t* hot = server_chot(&s, h);

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_WINDOW_TYPE;

  struct {
    xcb_get_property_reply_t r;
    xcb_atom_t types[1];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.value_len = 1;
  reply.r.type = XCB_ATOM_ATOM;
  reply.types[0] = atoms._NET_WM_WINDOW_TYPE_DOCK;

  wm_handle_reply(&s, &slot, &reply.r, NULL);

  assert(hot->type == WINDOW_TYPE_DOCK);
  assert(hot->base_layer == LAYER_DOCK);
  assert(hot->flags & CLIENT_FLAG_UNDECORATED);
  hot->focus_override = -1;
  assert(should_focus_on_map(hot) == false);
  assert(client_can_move(hot) == false);

  printf("test_window_type_dock_layer passed\n");
  cleanup_server(&s);
}

static void test_state_below_sticky_skip_applies(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_STATE = 910;
  atoms._NET_WM_STATE_BELOW = 911;
  atoms._NET_WM_STATE_STICKY = 912;
  atoms._NET_WM_STATE_SKIP_TASKBAR = 913;
  atoms._NET_WM_STATE_SKIP_PAGER = 914;

  s.desktop_count = 2;
  s.current_desktop = 0;

  handle_t h = add_mapped_client(&s, 6001, 6101);
  client_hot_t* hot = server_chot(&s, h);
  hot->manage_phase = MANAGE_DONE;

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_STATE;

  struct {
    xcb_get_property_reply_t r;
    xcb_atom_t states[4];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.value_len = 4;
  reply.r.type = XCB_ATOM_ATOM;
  reply.states[0] = atoms._NET_WM_STATE_BELOW;
  reply.states[1] = atoms._NET_WM_STATE_STICKY;
  reply.states[2] = atoms._NET_WM_STATE_SKIP_TASKBAR;
  reply.states[3] = atoms._NET_WM_STATE_SKIP_PAGER;

  wm_handle_reply(&s, &slot, &reply.r, NULL);

  assert(hot->state_below == true);
  assert(hot->layer == LAYER_BELOW);
  assert(hot->sticky == true);
  assert(hot->skip_taskbar == true);
  assert(hot->skip_pager == true);

  wm_flush_dirty(&s, monotonic_time_ns());

  assert(s.layers[LAYER_BELOW].length == 1);
  assert(ptr_to_handle(s.layers[LAYER_BELOW].items[0]) == h);

  const struct stub_prop_call* state = find_prop_call(hot->xid, atoms._NET_WM_STATE, false);
  assert(state != NULL);
  assert(atom_in_state_list((xcb_atom_t*)state->data, state->len, atoms._NET_WM_STATE_BELOW));
  assert(atom_in_state_list((xcb_atom_t*)state->data, state->len, atoms._NET_WM_STATE_STICKY));
  assert(atom_in_state_list((xcb_atom_t*)state->data, state->len, atoms._NET_WM_STATE_SKIP_TASKBAR));
  assert(atom_in_state_list((xcb_atom_t*)state->data, state->len, atoms._NET_WM_STATE_SKIP_PAGER));

  stub_unmap_window_count = 0;
  wm_switch_workspace(&s, 1);
  wm_flush_dirty(&s, monotonic_time_ns());
  assert(stub_unmap_window_count == 0);

  printf("test_state_below_sticky_skip_applies passed\n");
  cleanup_server(&s);
}

static void test_manage_phase_ignores_startup_maximize_state(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_STATE = 915;
  atoms._NET_WM_STATE_MAXIMIZED_HORZ = 916;
  atoms._NET_WM_STATE_MAXIMIZED_VERT = 917;

  handle_t h = add_mapped_client(&s, 6011, 6111);
  client_hot_t* hot = server_chot(&s, h);
  assert(hot != NULL);

  hot->state = STATE_NEW;
  hot->manage_phase = MANAGE_PHASE1;
  hot->server = (rect_t){100, 80, 640, 360};
  hot->desired = hot->server;

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_STATE;

  struct {
    xcb_get_property_reply_t r;
    xcb_atom_t states[2];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.value_len = 2;
  reply.r.type = XCB_ATOM_ATOM;
  reply.states[0] = atoms._NET_WM_STATE_MAXIMIZED_HORZ;
  reply.states[1] = atoms._NET_WM_STATE_MAXIMIZED_VERT;

  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(hot->maximized_horz == false);
  assert(hot->maximized_vert == false);
  assert(hot->desired.x == 100);
  assert(hot->desired.y == 80);
  assert(hot->desired.w == 640);
  assert(hot->desired.h == 360);

  hot->state = STATE_MAPPED;
  hot->manage_phase = MANAGE_DONE;
  hot->desired = hot->server;

  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(hot->maximized_horz == true);
  assert(hot->maximized_vert == true);

  printf("test_manage_phase_ignores_startup_maximize_state passed\n");
  cleanup_server(&s);
}

static void test_hidden_state_not_toggled_by_off_desktop_visibility(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_STATE = 920;
  atoms._NET_WM_STATE_HIDDEN = 921;
  atoms._NET_WM_DESKTOP = 922;
  atoms.WM_STATE = 923;

  s.desktop_count = 2;
  s.current_desktop = 0;

  handle_t h = add_mapped_client(&s, 6201, 6301);
  client_hot_t* hot = server_chot(&s, h);
  assert(hot != NULL);
  hot->desktop = 0;
  hot->sticky = false;
  hot->dirty |= DIRTY_STATE;

  wm_flush_dirty(&s, monotonic_time_ns());
  assert_hidden_state_flag(hot, false, atoms._NET_WM_STATE, atoms._NET_WM_STATE_HIDDEN);

  wm_client_move_to_workspace(&s, h, 1, false);
  wm_flush_dirty(&s, monotonic_time_ns());

  assert(stub_unmap_window_count == 1);
  assert(stub_last_unmapped_window == hot->frame);
  assert_wm_state_value(hot, atoms.WM_STATE, XCB_ICCCM_WM_STATE_ICONIC);
  assert_hidden_state_flag(hot, false, atoms._NET_WM_STATE, atoms._NET_WM_STATE_HIDDEN);

  wm_switch_workspace(&s, 1);
  wm_flush_dirty(&s, monotonic_time_ns());
  hot->dirty |= DIRTY_STATE;
  wm_flush_dirty(&s, monotonic_time_ns());

  assert(stub_map_window_count >= 1);
  assert_wm_state_value(hot, atoms.WM_STATE, XCB_ICCCM_WM_STATE_NORMAL);
  assert_hidden_state_flag(hot, false, atoms._NET_WM_STATE, atoms._NET_WM_STATE_HIDDEN);

  printf("test_hidden_state_not_toggled_by_off_desktop_visibility passed\n");
  cleanup_server(&s);
}

static void test_hidden_state_tracks_iconify_restore_and_wm_state(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_STATE = 924;
  atoms._NET_WM_STATE_HIDDEN = 925;
  atoms.WM_STATE = 926;

  handle_t h = add_mapped_client(&s, 6202, 6302);
  client_hot_t* hot = server_chot(&s, h);
  assert(hot != NULL);

  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_HIDDEN);
  wm_flush_dirty(&s, monotonic_time_ns());

  assert(hot->state == STATE_UNMAPPED);
  assert_wm_state_value(hot, atoms.WM_STATE, XCB_ICCCM_WM_STATE_ICONIC);
  assert_hidden_state_flag(hot, true, atoms._NET_WM_STATE, atoms._NET_WM_STATE_HIDDEN);

  wm_client_update_state(&s, h, 0, atoms._NET_WM_STATE_HIDDEN);
  wm_flush_dirty(&s, monotonic_time_ns());

  assert(hot->state == STATE_MAPPED);
  assert_wm_state_value(hot, atoms.WM_STATE, XCB_ICCCM_WM_STATE_NORMAL);
  assert_hidden_state_flag(hot, false, atoms._NET_WM_STATE, atoms._NET_WM_STATE_HIDDEN);

  printf("test_hidden_state_tracks_iconify_restore_and_wm_state passed\n");
  cleanup_server(&s);
}

static void test_show_desktop_round_trip_clears_hidden_state(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_STATE = 927;
  atoms._NET_WM_STATE_HIDDEN = 928;
  atoms._NET_SHOWING_DESKTOP = 929;
  atoms.WM_STATE = 930;

  handle_t h = add_mapped_client(&s, 6203, 6303);
  client_hot_t* hot = server_chot(&s, h);
  assert(hot != NULL);
  hot->manage_phase = MANAGE_DONE;

  wm_set_showing_desktop(&s, true);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* showing = find_prop_call(s.root, atoms._NET_SHOWING_DESKTOP, false);
  assert(showing != NULL);
  assert(showing->len == 1);
  assert(((const uint32_t*)showing->data)[0] == 1u);
  assert(s.showing_desktop == true);
  assert(hot->show_desktop_hidden == true);
  assert(hot->state == STATE_UNMAPPED);
  assert_wm_state_value(hot, atoms.WM_STATE, XCB_ICCCM_WM_STATE_ICONIC);

  wm_set_showing_desktop(&s, false);
  wm_flush_dirty(&s, monotonic_time_ns());

  showing = find_prop_call(s.root, atoms._NET_SHOWING_DESKTOP, false);
  assert(showing != NULL);
  assert(showing->len == 1);
  assert(((const uint32_t*)showing->data)[0] == 0u);
  assert(s.showing_desktop == false);
  assert(hot->show_desktop_hidden == false);
  assert(hot->state == STATE_MAPPED);
  assert_wm_state_value(hot, atoms.WM_STATE, XCB_ICCCM_WM_STATE_NORMAL);
  assert_hidden_state_flag(hot, false, atoms._NET_WM_STATE, atoms._NET_WM_STATE_HIDDEN);

  printf("test_show_desktop_round_trip_clears_hidden_state passed\n");
  cleanup_server(&s);
}

static void test_state_idempotent_and_unknown(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_STATE_ABOVE = 700;
  atoms._NET_WM_STATE_BELOW = 701;

  handle_t h = add_mapped_client(&s, 5001, 5101);
  client_hot_t* hot = server_chot(&s, h);

  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_ABOVE);
  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_ABOVE);
  assert(hot->state_above == true);
  assert(hot->layer == LAYER_ABOVE);

  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_BELOW);
  assert(hot->state_above == false);
  assert(hot->state_below == true);
  assert(hot->layer == LAYER_BELOW);

  wm_client_update_state(&s, h, 2, 9999);
  assert(hot->state_below == true);
  assert(hot->layer == LAYER_BELOW);

  printf("test_state_idempotent_and_unknown passed\n");
  cleanup_server(&s);
}

static bool atom_in_state_list(const xcb_atom_t* atoms_list, uint32_t count, xcb_atom_t needle) {
  for (uint32_t i = 0; i < count; i++) {
    if (atoms_list[i] == needle)
      return true;
  }
  return false;
}

static void test_urgency_hint_maps_to_ewmh_state(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms.WM_HINTS = 800;
  atoms._NET_WM_STATE = 801;
  atoms._NET_WM_STATE_DEMANDS_ATTENTION = 802;

  handle_t h = add_mapped_client(&s, 6001, 6101);
  client_hot_t* hot = server_chot(&s, h);

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_HINTS;

  struct {
    xcb_get_property_reply_t r;
    uint32_t data[9];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.value_len = 9;
  reply.r.type = XCB_ATOM_WM_HINTS;
  reply.data[0] = XCB_ICCCM_WM_HINT_X_URGENCY;

  wm_handle_reply(&s, &slot, &reply.r, NULL);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* state = find_prop_call(hot->xid, atoms._NET_WM_STATE, false);
  assert(state != NULL);
  assert(atom_in_state_list((xcb_atom_t*)state->data, state->len, atoms._NET_WM_STATE_DEMANDS_ATTENTION));

  // Clear urgency
  reply.data[0] = 0;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  wm_flush_dirty(&s, monotonic_time_ns());

  state = find_prop_call(hot->xid, atoms._NET_WM_STATE, false);
  assert(state != NULL);
  assert(!atom_in_state_list((xcb_atom_t*)state->data, state->len, atoms._NET_WM_STATE_DEMANDS_ATTENTION));

  printf("test_urgency_hint_maps_to_ewmh_state passed\n");
  cleanup_server(&s);
}

static void test_optional_state_properties_stored_in_cold(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_WINDOW_OPACITY = 1200;
  atoms._NET_WM_BYPASS_COMPOSITOR = 1201;
  atoms._NET_WM_ICON_GEOMETRY = 1202;
  atoms._NET_WM_FULLSCREEN_MONITORS = 1203;
  atoms._NET_WM_STATE_FULLSCREEN = 1204;

  handle_t h = add_mapped_client(&s, 7001, 7101);
  client_hot_t* hot = server_chot(&s, h);
  client_cold_t* cold = server_ccold(&s, h);
  assert(hot && cold);

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;

  struct {
    xcb_get_property_reply_t r;
    uint32_t data[4];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.type = XCB_ATOM_CARDINAL;

  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_WINDOW_OPACITY;
  reply.r.value_len = 1;
  reply.data[0] = 0x7f010203u;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(cold->window_opacity_valid);
  assert(cold->window_opacity == 0x7f010203u);
  const struct stub_prop_call* opacity_set = find_prop_call(hot->frame, atoms._NET_WM_WINDOW_OPACITY, false);
  assert(opacity_set != NULL);
  assert(opacity_set->len == 1);
  assert(((const uint32_t*)opacity_set->data)[0] == 0x7f010203u);

  reply.r.value_len = 0;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(!cold->window_opacity_valid);
  const struct stub_prop_call* opacity_del = find_prop_call(hot->frame, atoms._NET_WM_WINDOW_OPACITY, true);
  assert(opacity_del != NULL);

  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_BYPASS_COMPOSITOR;
  reply.r.value_len = 1;
  reply.data[0] = 2;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(cold->bypass_compositor_valid);
  assert(cold->bypass_compositor == 2u);
  const struct stub_prop_call* bypass_set = find_prop_call(hot->frame, atoms._NET_WM_BYPASS_COMPOSITOR, false);
  assert(bypass_set != NULL);
  assert(bypass_set->len == 1);
  assert(((const uint32_t*)bypass_set->data)[0] == 2u);

  reply.data[0] = 0;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(!cold->bypass_compositor_valid);
  assert(cold->bypass_compositor == 0u);
  const struct stub_prop_call* bypass_del = find_prop_call(hot->frame, atoms._NET_WM_BYPASS_COMPOSITOR, true);
  assert(bypass_del != NULL);

  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_ICON_GEOMETRY;
  reply.r.value_len = 4;
  reply.data[0] = 10;
  reply.data[1] = 20;
  reply.data[2] = 30;
  reply.data[3] = 40;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(cold->icon_geometry_valid);
  assert(cold->icon_geometry.x == 10);
  assert(cold->icon_geometry.y == 20);
  assert(cold->icon_geometry.w == 30);
  assert(cold->icon_geometry.h == 40);

  reply.r.value_len = 0;
  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(!cold->icon_geometry_valid);

  xcb_client_message_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = hot->xid;
  ev.type = atoms._NET_WM_FULLSCREEN_MONITORS;
  ev.data.data32[0] = 1;
  ev.data.data32[1] = 2;
  ev.data.data32[2] = 3;
  ev.data.data32[3] = 4;
  wm_handle_client_message(&s, &ev);
  assert(cold->fullscreen_monitors_valid);
  assert(cold->fullscreen_monitors[0] == 1u);
  assert(cold->fullscreen_monitors[1] == 2u);
  assert(cold->fullscreen_monitors[2] == 3u);
  assert(cold->fullscreen_monitors[3] == 4u);

  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_FULLSCREEN);
  const struct stub_prop_call* fullscreen_monitors = find_prop_call(hot->xid, atoms._NET_WM_FULLSCREEN_MONITORS, false);
  assert(fullscreen_monitors != NULL);
  assert(fullscreen_monitors->len == 4);
  const uint32_t* monitors = (const uint32_t*)fullscreen_monitors->data;
  assert(monitors[0] == 1u);
  assert(monitors[1] == 2u);
  assert(monitors[2] == 3u);
  assert(monitors[3] == 4u);

  printf("test_optional_state_properties_stored_in_cold passed\n");
  cleanup_server(&s);
}

int main(void) {
  test_active_window_updates();
  test_active_window_exits_show_desktop_mode();
  test_restore_exits_show_desktop_mode();
  test_client_list_add_remove();
  test_client_list_remove_keeps_order();
  test_client_list_includes_all_managed();
  test_desktop_props_publish_and_switch();
  test_net_wm_desktop_reply_sets_sticky_and_desktop();
  test_net_wm_desktop_reply_clamps_out_of_range();
  test_net_wm_desktop_reply_moves_after_manage();
  test_window_type_desktop_defaults_sticky();
  test_window_type_desktop_respects_net_wm_desktop();
  test_desktop_type_then_net_wm_desktop_updates();
  test_strut_updates_workarea();
  test_workarea_publishes_per_desktop_array();
  test_maximize_and_fullscreen_use_client_desktop_monitor_workarea();
  test_window_type_dock_layer();
  test_state_below_sticky_skip_applies();
  test_manage_phase_ignores_startup_maximize_state();
  test_hidden_state_not_toggled_by_off_desktop_visibility();
  test_hidden_state_tracks_iconify_restore_and_wm_state();
  test_show_desktop_round_trip_clears_hidden_state();
  test_state_idempotent_and_unknown();
  test_urgency_hint_maps_to_ewmh_state();
  test_optional_state_properties_stored_in_cold();
  return 0;
}
