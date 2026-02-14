#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
      render_free(&hot->render_ctx);
      if (hot->icon_surface)
        cairo_surface_destroy(hot->icon_surface);
    }
  }
  cookie_jar_destroy(&s->cookie_jar);
  slotmap_destroy(&s->clients);
  small_vec_destroy(&s->active_clients);
  hash_map_destroy(&s->window_to_client);
  hash_map_destroy(&s->frame_to_client);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_destroy(&s->layers[i]);
  arena_destroy(&s->tick_arena);
  config_destroy(&s->config);
  xcb_disconnect(s->conn);
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

  printf("test_active_window_updates passed\n");
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

int main(void) {
  test_active_window_updates();
  test_client_list_add_remove();
  test_client_list_includes_all_managed();
  test_desktop_props_publish_and_switch();
  test_net_wm_desktop_reply_sets_sticky_and_desktop();
  test_net_wm_desktop_reply_clamps_out_of_range();
  test_net_wm_desktop_reply_moves_after_manage();
  test_window_type_desktop_defaults_sticky();
  test_window_type_desktop_respects_net_wm_desktop();
  test_desktop_type_then_net_wm_desktop_updates();
  test_strut_updates_workarea();
  test_window_type_dock_layer();
  test_state_below_sticky_skip_applies();
  test_state_idempotent_and_unknown();
  test_urgency_hint_maps_to_ewmh_state();
  return 0;
}
