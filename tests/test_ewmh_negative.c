#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
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

static const struct stub_prop_call* find_prop_call(xcb_window_t win, xcb_atom_t atom, bool deleted) {
  for (int i = stub_prop_calls_len - 1; i >= 0; i--) {
    if (stub_prop_calls[i].window == win && stub_prop_calls[i].atom == atom && stub_prop_calls[i].deleted == deleted) {
      return &stub_prop_calls[i];
    }
  }
  return NULL;
}

static xcb_get_property_reply_t* make_string_reply(xcb_atom_t type, const char* value, int len) {
  size_t total = sizeof(xcb_get_property_reply_t) + (size_t)len;
  xcb_get_property_reply_t* rep = calloc(1, total);
  if (!rep)
    return NULL;
  rep->format = 8;
  rep->type = type;
  rep->value_len = (uint32_t)len;
  rep->length = (uint32_t)((len + 3) / 4);
  if (len > 0 && value) {
    memcpy(xcb_get_property_value(rep), value, (size_t)len);
  }
  return rep;
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
  s->desktop_count = 1;
  s->current_desktop = 0;
  s->workarea = (rect_t){0, 0, 0, 0};

  arena_init(&s->tick_arena, 4096);
  cookie_jar_init(&s->cookie_jar);
  slotmap_init(&s->clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
  hash_map_init(&s->window_to_client);
  hash_map_init(&s->frame_to_client);
  hash_map_init(&s->pending_unmanaged_states);
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
  hash_map_destroy(&s->window_to_client);
  hash_map_destroy(&s->frame_to_client);
  for (size_t i = 0; i < s->pending_unmanaged_states.capacity; i++) {
    hash_map_entry_t* entry = &s->pending_unmanaged_states.entries[i];
    if (!entry->key || !entry->value)
      continue;
    small_vec_t* v = (small_vec_t*)entry->value;
    for (size_t j = 0; j < v->length; j++) {
      free(v->items[j]);
    }
    small_vec_destroy(v);
    free(v);
  }
  hash_map_destroy(&s->pending_unmanaged_states);
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
  hot->manage_phase = MANAGE_DONE;
  hot->server = (rect_t){10, 10, 200, 150};
  hot->desired = hot->server;

  list_init(&hot->focus_node);
  list_init(&hot->transients_head);
  list_init(&hot->transient_sibling);

  hash_map_insert(&s->window_to_client, win, handle_to_ptr(h));
  hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));
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

static void send_net_restack_window(server_t* s, xcb_window_t win, xcb_window_t sibling, uint32_t detail) {
  xcb_client_message_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = win;
  ev.type = atoms._NET_RESTACK_WINDOW;
  ev.data.data32[0] = 2;
  ev.data.data32[1] = sibling;
  ev.data.data32[2] = detail;
  wm_handle_client_message(s, &ev);
}

static void assert_layer_order(const server_t* s, int layer, const handle_t* expected, size_t expected_len) {
  const small_vec_t* v = &s->layers[layer];
  assert(v->length == expected_len);
  for (size_t i = 0; i < expected_len; i++) {
    assert((handle_t)(uintptr_t)v->items[i] == expected[i]);
  }
}

static void test_malformed_wm_state_format_ignored(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_STATE = 100;
  atoms._NET_WM_STATE_FULLSCREEN = 101;

  handle_t h = add_mapped_client(&s, 1001, 1101);
  client_hot_t* hot = server_chot(&s, h);

  xcb_client_message_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.type = atoms._NET_WM_STATE;
  ev.window = hot->xid;
  ev.format = 8;
  ev.data.data32[0] = 1;
  ev.data.data32[1] = atoms._NET_WM_STATE_FULLSCREEN;

  wm_handle_client_message(&s, &ev);

  assert(hot->layer == LAYER_NORMAL);
  assert(!(hot->flags & CLIENT_FLAG_UNDECORATED));

  printf("test_malformed_wm_state_format_ignored passed\n");
  cleanup_server(&s);
}

static void test_unknown_window_type_ignored(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_WINDOW_TYPE = 200;

  handle_t h = add_mapped_client(&s, 2001, 2101);
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
  reply.types[0] = 9999;

  wm_handle_reply(&s, &slot, &reply.r, NULL);

  assert(hot->type == WINDOW_TYPE_NORMAL);
  assert(!hot->type_from_net);

  printf("test_unknown_window_type_ignored passed\n");
  cleanup_server(&s);
}

static void test_unmanaged_wm_state_is_stashed(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_STATE = 205;
  atoms._NET_WM_STATE_FULLSCREEN = 206;

  xcb_window_t win = 0x424242;
  xcb_client_message_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = win;
  ev.type = atoms._NET_WM_STATE;
  ev.data.data32[0] = 1;
  ev.data.data32[1] = atoms._NET_WM_STATE_FULLSCREEN;
  ev.data.data32[2] = XCB_ATOM_NONE;

  wm_handle_client_message(&s, &ev);

  small_vec_t* v = (small_vec_t*)hash_map_get(&s.pending_unmanaged_states, win);
  assert(v != NULL);
  assert(v->length == 1);
  pending_state_msg_t* msg = (pending_state_msg_t*)v->items[0];
  assert(msg != NULL);
  assert(msg->action == 1);
  assert(msg->p1 == atoms._NET_WM_STATE_FULLSCREEN);
  assert(msg->p2 == XCB_ATOM_NONE);

  xcb_destroy_notify_event_t de;
  memset(&de, 0, sizeof(de));
  de.response_type = XCB_DESTROY_NOTIFY;
  de.window = win;
  de.event = s.root;
  wm_handle_destroy_notify(&s, &de);

  assert(hash_map_get(&s.pending_unmanaged_states, win) == NULL);

  printf("test_unmanaged_wm_state_is_stashed passed\n");
  cleanup_server(&s);
}

static void test_strut_partial_invalid_ranges_ignored(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WORKAREA = 300;
  atoms._NET_WM_STRUT_PARTIAL = 301;
  s.desktop_count = 1;

  handle_t h = add_mapped_client(&s, 3001, 3101);

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
  reply.data[0] = 100;
  reply.data[4] = 50;
  reply.data[5] = 40;

  wm_handle_reply(&s, &slot, &reply.r, NULL);
  wm_flush_dirty(&s, monotonic_time_ns());

  const struct stub_prop_call* wa = find_prop_call(s.root, atoms._NET_WORKAREA, false);
  assert(wa != NULL);
  const uint32_t* vals = (const uint32_t*)wa->data;
  assert(vals[0] == 0);
  assert(vals[1] == 0);
  assert(vals[2] == 1920);
  assert(vals[3] == 1080);

  printf("test_strut_partial_invalid_ranges_ignored passed\n");
  cleanup_server(&s);
}

static void test_property_spam_no_crash(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  atoms._NET_WM_NAME = 400;
  atoms.UTF8_STRING = 401;

  handle_t h = add_mapped_client(&s, 4001, 4101);
  client_hot_t* hot = server_chot(&s, h);

  cookie_slot_t slot = {0};
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;

  for (int i = 0; i < 256; i++) {
    xcb_get_property_reply_t* rep = make_string_reply(atoms.UTF8_STRING, "spam", 4);
    wm_handle_reply(&s, &slot, rep, NULL);
    free(rep);
  }

  client_cold_t* cold = server_ccold(&s, h);
  assert(cold->has_net_wm_name);
  assert(cold->base_title != NULL);

  printf("test_property_spam_no_crash passed\n");
  cleanup_server(&s);
}

static void test_active_window_rejects_stale_application_timestamp(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  s.desktop_count = 2;
  s.current_desktop = 0;

  handle_t h1 = add_mapped_client(&s, 5001, 5101);
  handle_t h2 = add_mapped_client(&s, 5002, 5102);
  client_hot_t* a = server_chot(&s, h1);
  client_hot_t* b = server_chot(&s, h2);
  assert(a && b);

  a->desktop = 0;
  b->desktop = 0;
  a->user_time = 500;

  wm_set_focus(&s, h1);
  assert(s.focused_client == h1);

  send_net_active_window(&s, b->xid, 1, 400);

  assert(s.current_desktop == 0);
  assert(s.focused_client == h1);

  printf("test_active_window_rejects_stale_application_timestamp passed\n");
  cleanup_server(&s);
}

static void test_active_window_rejects_non_user_cross_desktop_activation(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  s.desktop_count = 2;
  s.current_desktop = 0;

  handle_t h1 = add_mapped_client(&s, 6001, 6101);
  handle_t h2 = add_mapped_client(&s, 6002, 6102);
  client_hot_t* a = server_chot(&s, h1);
  client_hot_t* b = server_chot(&s, h2);
  assert(a && b);

  a->desktop = 0;
  b->desktop = 1;
  a->user_time = 700;

  wm_set_focus(&s, h1);
  assert(s.focused_client == h1);

  send_net_active_window(&s, b->xid, 1, 800);

  assert(s.current_desktop == 0);
  assert(s.focused_client == h1);

  printf("test_active_window_rejects_non_user_cross_desktop_activation passed\n");
  cleanup_server(&s);
}

static void test_active_window_user_request_can_switch_desktop(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  s.desktop_count = 2;
  s.current_desktop = 0;

  handle_t h1 = add_mapped_client(&s, 7001, 7101);
  handle_t h2 = add_mapped_client(&s, 7002, 7102);
  client_hot_t* a = server_chot(&s, h1);
  client_hot_t* b = server_chot(&s, h2);
  assert(a && b);

  a->desktop = 0;
  b->desktop = 1;
  a->user_time = 700;

  wm_set_focus(&s, h1);
  assert(s.focused_client == h1);

  send_net_active_window(&s, b->xid, 2, 1);

  assert(s.current_desktop == 1);
  assert(s.focused_client == h2);

  printf("test_active_window_user_request_can_switch_desktop passed\n");
  cleanup_server(&s);
}

static void test_active_window_rejects_missing_timestamp_when_focus_time_known(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  s.desktop_count = 2;
  s.current_desktop = 0;

  handle_t h1 = add_mapped_client(&s, 8001, 8101);
  handle_t h2 = add_mapped_client(&s, 8002, 8102);
  client_hot_t* a = server_chot(&s, h1);
  client_hot_t* b = server_chot(&s, h2);
  assert(a && b);

  a->desktop = 0;
  b->desktop = 0;
  a->user_time = 900;

  wm_set_focus(&s, h1);
  assert(s.focused_client == h1);

  send_net_active_window(&s, b->xid, 1, XCB_CURRENT_TIME);

  assert(s.current_desktop == 0);
  assert(s.focused_client == h1);

  printf("test_active_window_rejects_missing_timestamp_when_focus_time_known passed\n");
  cleanup_server(&s);
}

static void test_restack_unknown_sibling_uses_no_sibling_semantics(void) {
  struct mode_case {
    uint32_t detail;
    bool expect_raise;
    const char* label;
  };
  static const struct mode_case cases[] = {
      {XCB_STACK_MODE_ABOVE, true, "above"},
      {XCB_STACK_MODE_BELOW, false, "below"},
      {XCB_STACK_MODE_TOP_IF, true, "top_if"},
      {XCB_STACK_MODE_BOTTOM_IF, false, "bottom_if"},
      {XCB_STACK_MODE_OPPOSITE, true, "opposite"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const struct mode_case* tc = &cases[i];

    server_t s;
    setup_server(&s);
    xcb_stubs_reset();

    handle_t h1 = add_mapped_client(&s, 9001, 9101);
    handle_t h2 = add_mapped_client(&s, 9002, 9102);
    handle_t h3 = add_mapped_client(&s, 9003, 9103);
    stack_raise(&s, h1);
    stack_raise(&s, h2);
    stack_raise(&s, h3);

    send_net_restack_window(&s, 9002, 0xDEADBEEFu, tc->detail);

    if (tc->expect_raise) {
      handle_t order[] = {h1, h3, h2};
      assert_layer_order(&s, LAYER_NORMAL, order, 3);
    }
    else {
      handle_t order[] = {h2, h1, h3};
      assert_layer_order(&s, LAYER_NORMAL, order, 3);
    }

    printf("test_restack_unknown_sibling_uses_no_sibling_semantics (%s) passed\n", tc->label);
    cleanup_server(&s);
  }
}

static void test_restack_desktop_mismatch_sibling_is_ignored(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  s.desktop_count = 2;
  s.current_desktop = 0;

  handle_t h_helper = add_mapped_client(&s, 9101, 9201);
  handle_t h_sibling = add_mapped_client(&s, 9102, 9202);
  handle_t h_target = add_mapped_client(&s, 9103, 9203);

  client_hot_t* helper = server_chot(&s, h_helper);
  client_hot_t* sibling = server_chot(&s, h_sibling);
  client_hot_t* target = server_chot(&s, h_target);
  assert(helper && sibling && target);

  helper->desktop = 0;
  target->desktop = 0;
  sibling->desktop = 1;

  stack_raise(&s, h_helper);
  stack_raise(&s, h_sibling);
  stack_raise(&s, h_target);

  send_net_restack_window(&s, target->xid, sibling->xid, XCB_STACK_MODE_BOTTOM_IF);

  handle_t order[] = {h_target, h_helper, h_sibling};
  assert_layer_order(&s, LAYER_NORMAL, order, 3);

  printf("test_restack_desktop_mismatch_sibling_is_ignored passed\n");
  cleanup_server(&s);
}

static void test_restack_iconified_sibling_is_ignored(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();

  handle_t h_helper = add_mapped_client(&s, 9201, 9301);
  handle_t h_target = add_mapped_client(&s, 9202, 9302);
  handle_t h_sibling = add_mapped_client(&s, 9203, 9303);

  client_hot_t* target = server_chot(&s, h_target);
  client_hot_t* sibling = server_chot(&s, h_sibling);
  assert(target && sibling);

  stack_raise(&s, h_helper);
  stack_raise(&s, h_target);
  stack_raise(&s, h_sibling);

  wm_client_iconify(&s, h_sibling);
  assert(sibling->state == STATE_UNMAPPED);

  send_net_restack_window(&s, target->xid, sibling->xid, XCB_STACK_MODE_BOTTOM_IF);

  handle_t order[] = {h_target, h_helper};
  assert_layer_order(&s, LAYER_NORMAL, order, 2);

  printf("test_restack_iconified_sibling_is_ignored passed\n");
  cleanup_server(&s);
}

int main(void) {
  test_malformed_wm_state_format_ignored();
  test_unknown_window_type_ignored();
  test_unmanaged_wm_state_is_stashed();
  test_strut_partial_invalid_ranges_ignored();
  test_property_spam_no_crash();
  test_active_window_rejects_stale_application_timestamp();
  test_active_window_rejects_non_user_cross_desktop_activation();
  test_active_window_user_request_can_switch_desktop();
  test_active_window_rejects_missing_timestamp_when_focus_time_known();
  test_restack_unknown_sibling_uses_no_sibling_semantics();
  test_restack_desktop_mismatch_sibling_is_ignored();
  test_restack_iconified_sibling_is_ignored();
  return 0;
}
