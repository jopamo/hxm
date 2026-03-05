#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"

extern void xcb_stubs_reset(void);
extern int stub_save_set_insert_count;
extern int stub_save_set_delete_count;
extern xcb_window_t stub_last_save_set_window;
extern int stub_reparent_window_count;
extern xcb_window_t stub_last_reparent_window;
extern xcb_window_t stub_last_reparent_parent;
extern int16_t stub_last_reparent_x;
extern int16_t stub_last_reparent_y;

static void setup_server(server_t* s) {
  memset(s, 0, sizeof(*s));
  s->is_test = true;
  xcb_stubs_reset();
  s->conn = xcb_connect(NULL, NULL);
  atoms_init(s->conn);
  s->root = 1;
  s->root_depth = 24;
  s->root_visual = 1;
  s->root_visual_type = xcb_get_visualtype(s->conn, 0);

  list_init(&s->focus_history);
  hash_map_init(&s->window_to_client);
  hash_map_init(&s->frame_to_client);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_init(&s->layers[i]);
  slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
}

static void cleanup_server(server_t* s) {
  for (uint32_t i = 1; i < s->clients.cap; i++) {
    if (s->clients.hdr[i].live) {
      handle_t h = handle_make(i, s->clients.hdr[i].gen);
      client_hot_t* hot = server_chot(s, h);
      client_cold_t* cold = server_ccold(s, h);
      if (cold)
        arena_destroy(&cold->string_arena);
      if (hot) {
        client_render_payload_destroy(cold);
      }
    }
  }
  slotmap_destroy(&s->clients);
  hash_map_destroy(&s->window_to_client);
  hash_map_destroy(&s->frame_to_client);
  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_destroy(&s->layers[i]);
  xcb_disconnect(s->conn);
}

static handle_t add_client(server_t* s, xcb_window_t xid, xcb_window_t frame) {
  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  client_cold_t* cold = (client_cold_t*)cold_ptr;
  memset(hot, 0, sizeof(*hot));
  memset(cold, 0, sizeof(*cold));
  client_render_payload_init(cold);
  arena_init(&cold->string_arena, 128);

  hot->self = h;
  hot->xid = xid;
  hot->frame = frame;
  hot->state = STATE_NEW;
  hot->type = WINDOW_TYPE_NORMAL;
  hot->layer = LAYER_NORMAL;
  hot->base_layer = LAYER_NORMAL;
  hot->desired = (rect_t){0, 0, 100, 80};
  cold->visual_id = s->root_visual;
  cold->depth = s->root_depth;
  hot->stacking_index = -1;
  hot->stacking_layer = -1;
  list_init(&hot->focus_node);
  list_init(&hot->transients_head);
  list_init(&hot->transient_sibling);

  hash_map_insert(&s->window_to_client, xid, handle_to_ptr(h));
  hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));
  return h;
}

static void test_save_set_insert_and_delete(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, 2001, 2101);
  client_hot_t* hot = server_chot(&s, h);

  stub_save_set_insert_count = 0;
  stub_save_set_delete_count = 0;

  client_finish_manage(&s, h);
  assert(stub_save_set_insert_count == 1);
  assert(stub_last_save_set_window == hot->xid);

  client_unmanage(&s, h);
  assert(stub_save_set_delete_count == 1);
  assert(stub_last_save_set_window == hot->xid);

  printf("test_save_set_insert_and_delete passed\n");
  cleanup_server(&s);
}

static void test_save_set_delete_skipped_when_destroyed(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, 2002, 2102);
  client_hot_t* hot = server_chot(&s, h);

  stub_save_set_insert_count = 0;
  stub_save_set_delete_count = 0;

  client_finish_manage(&s, h);
  assert(stub_save_set_insert_count == 1);

  hot->state = STATE_DESTROYED;
  client_unmanage(&s, h);
  assert(stub_save_set_delete_count == 0);

  printf("test_save_set_delete_skipped_when_destroyed passed\n");
  cleanup_server(&s);
}

static void test_unmanage_reparent_uses_desired_when_geom_dirty(void) {
  server_t s;
  setup_server(&s);

  s.config.theme.border_width = 2;
  s.config.theme.title_height = 20;

  handle_t h = add_client(&s, 2003, 2103);
  client_hot_t* hot = server_chot(&s, h);

  hot->state = STATE_MAPPED;
  hot->manage_phase = MANAGE_DONE;
  hot->server.x = 10;
  hot->server.y = 15;
  hot->desired.x = 110;
  hot->desired.y = 215;
  hot->dirty |= DIRTY_GEOM;

  xcb_stubs_reset();
  client_unmanage(&s, h);

  assert(stub_reparent_window_count == 1);
  assert(stub_last_reparent_window == 2003);
  assert(stub_last_reparent_parent == s.root);
  assert(stub_last_reparent_x == 112);
  assert(stub_last_reparent_y == 235);

  printf("test_unmanage_reparent_uses_desired_when_geom_dirty passed\n");
  cleanup_server(&s);
}

static void test_unmanage_reparent_gtk_desired_conversion(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, 2004, 2104);
  client_hot_t* hot = server_chot(&s, h);

  hot->state = STATE_MAPPED;
  hot->manage_phase = MANAGE_DONE;
  hot->server.x = 20;
  hot->server.y = 25;
  hot->desired.x = 300;
  hot->desired.y = 400;
  hot->gtk_frame_extents_set = true;
  hot->gtk_extents.left = 7;
  hot->gtk_extents.top = 9;
  hot->dirty |= DIRTY_GEOM;

  xcb_stubs_reset();
  client_unmanage(&s, h);

  assert(stub_reparent_window_count == 1);
  assert(stub_last_reparent_window == 2004);
  assert(stub_last_reparent_parent == s.root);
  assert(stub_last_reparent_x == 300);
  assert(stub_last_reparent_y == 400);

  printf("test_unmanage_reparent_gtk_desired_conversion passed\n");
  cleanup_server(&s);
}

int main(void) {
  test_save_set_insert_and_delete();
  test_save_set_delete_skipped_when_destroyed();
  test_unmanage_reparent_uses_desired_when_geom_dirty();
  test_unmanage_reparent_gtk_desired_conversion();
  return 0;
}
