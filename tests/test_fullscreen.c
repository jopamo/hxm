#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "config.h"
#include "event.h"
#include "hxm.h"
#include "slotmap.h"
#include "wm.h"
#include "xcb_utils.h"

extern int stub_map_window_count;
extern int stub_unmap_window_count;
extern xcb_atom_t stub_last_prop_atom;
extern uint32_t stub_last_prop_len;
extern uint8_t stub_last_prop_data[1024];

static void setup_server(server_t* s) {
  memset(s, 0, sizeof(*s));
  s->is_test = true;
  s->root = 1;
  s->root_depth = 24;
  s->root_visual_type = xcb_get_visualtype(NULL, 0);
  s->conn = (xcb_connection_t*)malloc(1);
  config_init_defaults(&s->config);
  s->config.theme.border_width = 5;
  s->config.theme.title_height = 20;
  s->config.fullscreen_use_workarea = false;
  s->workarea = (rect_t){0, 0, 800, 600};
  list_init(&s->focus_history);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_init(&s->layers[i]);
  hash_map_init(&s->window_to_client);
  hash_map_init(&s->frame_to_client);
  slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
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
  slotmap_destroy(&s->clients);
  hash_map_destroy(&s->window_to_client);
  hash_map_destroy(&s->frame_to_client);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_destroy(&s->layers[i]);
  config_destroy(&s->config);
  free(s->conn);
}

static handle_t add_client(server_t* s) {
  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  client_cold_t* cold = (client_cold_t*)cold_ptr;
  memset(hot, 0, sizeof(*hot));
  memset(cold, 0, sizeof(*cold));
  render_init(&hot->render_ctx);
  arena_init(&cold->string_arena, 128);
  hot->self = h;
  hot->xid = 1000 + handle_index(h);
  hot->frame = 2000 + handle_index(h);
  hot->state = STATE_MAPPED;
  hot->layer = LAYER_NORMAL;
  hot->base_layer = LAYER_NORMAL;
  hot->flags = CLIENT_FLAG_NONE;
  hot->server = (rect_t){100, 100, 400, 300};
  hot->desired = hot->server;
  list_init(&hot->focus_node);
  list_init(&hot->transients_head);
  list_init(&hot->transient_sibling);
  hash_map_insert(&s->window_to_client, hot->xid, handle_to_ptr(h));
  hash_map_insert(&s->frame_to_client, hot->frame, handle_to_ptr(h));
  return h;
}

void test_fullscreen_decorations(void) {
  server_t s;
  setup_server(&s);

  atoms._NET_WM_STATE_FULLSCREEN = 100;

  handle_t h = add_client(&s);
  client_hot_t* hot = server_chot(&s, h);

  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_FULLSCREEN);

  assert(hot->layer == LAYER_FULLSCREEN);
  assert(hot->flags & CLIENT_FLAG_UNDECORATED);
  assert(hot->dirty & DIRTY_GEOM);
  assert(hot->saved_layer == LAYER_NORMAL);
  assert(hot->saved_geom.x == 100);
  assert(hot->saved_geom.w == 400);
  assert((hot->saved_state_mask & CLIENT_FLAG_UNDECORATED) == 0);

  wm_client_update_state(&s, h, 0, atoms._NET_WM_STATE_FULLSCREEN);

  assert(hot->layer == LAYER_NORMAL);
  assert((hot->flags & CLIENT_FLAG_UNDECORATED) == 0);
  assert(hot->dirty & DIRTY_GEOM);
  assert(hot->desired.x == 100);
  assert(hot->desired.w == 400);

  printf("test_fullscreen_decorations passed\n");

  cleanup_server(&s);
}

void test_fullscreen_restores_flags_and_layer(void) {
  server_t s;
  setup_server(&s);

  atoms._NET_WM_STATE_FULLSCREEN = 101;

  handle_t h = add_client(&s);
  client_hot_t* hot = server_chot(&s, h);
  hot->state_above = true;
  hot->layer = LAYER_ABOVE;
  hot->base_layer = LAYER_NORMAL;
  hot->maximized_horz = true;
  hot->maximized_vert = true;

  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_FULLSCREEN);
  assert(hot->layer == LAYER_FULLSCREEN);
  assert(hot->maximized_horz == false);
  assert(hot->maximized_vert == false);

  wm_client_update_state(&s, h, 0, atoms._NET_WM_STATE_FULLSCREEN);
  assert(hot->layer == LAYER_ABOVE);
  assert(hot->state_above == true);
  assert(hot->maximized_horz == true);
  assert(hot->maximized_vert == true);

  printf("test_fullscreen_restores_flags_and_layer passed\n");

  cleanup_server(&s);
}

void test_above_below_state_layers(void) {
  server_t s;
  setup_server(&s);

  atoms._NET_WM_STATE_ABOVE = 110;
  atoms._NET_WM_STATE_BELOW = 111;

  handle_t h = add_client(&s);
  client_hot_t* hot = server_chot(&s, h);

  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_ABOVE);
  assert(hot->state_above == true);
  assert(hot->state_below == false);
  assert(hot->layer == LAYER_ABOVE);
  assert(hot->dirty & DIRTY_STACK);

  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_BELOW);
  assert(hot->state_above == false);
  assert(hot->state_below == true);
  assert(hot->layer == LAYER_BELOW);

  wm_client_update_state(&s, h, 0, atoms._NET_WM_STATE_BELOW);
  assert(hot->state_below == false);
  assert(hot->layer == LAYER_NORMAL);

  printf("test_above_below_state_layers passed\n");

  cleanup_server(&s);
}

void test_hidden_state_iconify_restore(void) {
  server_t s;
  setup_server(&s);

  atoms._NET_WM_STATE_HIDDEN = 120;
  atoms.WM_STATE = 121;

  handle_t h = add_client(&s);
  client_hot_t* hot = server_chot(&s, h);
  stack_raise(&s, h);

  stub_unmap_window_count = 0;
  stub_last_prop_atom = 0;
  wm_client_update_state(&s, h, 1, atoms._NET_WM_STATE_HIDDEN);

  assert(hot->state == STATE_UNMAPPED);
  assert(stub_unmap_window_count == 1);
  assert(stub_last_prop_atom == atoms.WM_STATE);
  uint32_t* vals = (uint32_t*)stub_last_prop_data;
  assert(vals[0] == XCB_ICCCM_WM_STATE_ICONIC);

  stub_map_window_count = 0;
  stub_last_prop_atom = 0;
  wm_client_update_state(&s, h, 0, atoms._NET_WM_STATE_HIDDEN);
  assert(hot->state == STATE_MAPPED);
  assert(stub_map_window_count == 2);
  assert(stub_last_prop_atom == atoms.WM_STATE);
  vals = (uint32_t*)stub_last_prop_data;
  assert(vals[0] == XCB_ICCCM_WM_STATE_NORMAL);

  printf("test_hidden_state_iconify_restore passed\n");

  cleanup_server(&s);
}

int main(void) {
  test_fullscreen_decorations();
  test_fullscreen_restores_flags_and_layer();
  test_above_below_state_layers();
  test_hidden_state_iconify_restore();
  return 0;
}
