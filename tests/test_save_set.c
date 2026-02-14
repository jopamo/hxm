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
        render_free(&hot->render_ctx);
        if (hot->icon_surface)
          cairo_surface_destroy(hot->icon_surface);
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
  render_init(&hot->render_ctx);
  arena_init(&cold->string_arena, 128);

  hot->self = h;
  hot->xid = xid;
  hot->frame = frame;
  hot->state = STATE_NEW;
  hot->type = WINDOW_TYPE_NORMAL;
  hot->layer = LAYER_NORMAL;
  hot->base_layer = LAYER_NORMAL;
  hot->desired = (rect_t){0, 0, 100, 80};
  hot->visual_id = s->root_visual;
  hot->depth = s->root_depth;
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

int main(void) {
  test_save_set_insert_and_delete();
  return 0;
}
