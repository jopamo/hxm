#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

extern xcb_atom_t stub_last_prop_atom;
extern uint32_t stub_last_prop_len;
extern uint8_t stub_last_prop_data[1024];

static void cleanup_server(server_t* s) {
  for (uint32_t i = 1; i < s->clients.cap; i++) {
    if (!s->clients.hdr[i].live)
      continue;
    handle_t h = handle_make(i, s->clients.hdr[i].gen);
    client_hot_t* hot = server_chot(s, h);
    if (!hot)
      continue;
    render_free(&hot->render_ctx);
    if (hot->icon_surface)
      cairo_surface_destroy(hot->icon_surface);
  }
  small_vec_destroy(&s->active_clients);
  slotmap_destroy(&s->clients);
  config_destroy(&s->config);
  free(s->conn);
}

void test_gtk_extents_toggle_decorations(void) {
  atoms._GTK_FRAME_EXTENTS = 100;
  atoms._NET_FRAME_EXTENTS = 200;

  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t*)malloc(1);

  config_init_defaults(&s.config);
  s.config.theme.border_width = 5;
  s.config.theme.title_height = 20;

  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) {
    config_destroy(&s.config);
    free(s.conn);
    return;
  }
  small_vec_init(&s.active_clients);

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  small_vec_push(&s.active_clients, handle_to_ptr(h));
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  hot->self = h;
  hot->xid = 123;
  hot->frame = 456;
  hot->state = STATE_MAPPED;
  hot->manage_phase = MANAGE_DONE;
  hot->depth = s.root_depth;
  hot->desired.x = 50;
  hot->desired.y = 60;
  hot->desired.w = 400;
  hot->desired.h = 300;

  struct {
    xcb_get_property_reply_t rep;
    uint32_t val[4];
  } reply_struct;
  memset(&reply_struct, 0, sizeof(reply_struct));
  reply_struct.rep.format = 32;
  reply_struct.rep.type = XCB_ATOM_CARDINAL;
  reply_struct.rep.value_len = 4;
  reply_struct.rep.length = 4;
  reply_struct.val[0] = 8;
  reply_struct.val[1] = 8;
  reply_struct.val[2] = 24;
  reply_struct.val[3] = 8;

  cookie_slot_t slot;
  slot.client = h;
  slot.type = COOKIE_GET_PROPERTY;
  slot.data = (uint64_t)atoms._GTK_FRAME_EXTENTS;

  wm_handle_reply(&s, &slot, &reply_struct.rep, NULL);

  assert(hot->gtk_frame_extents_set);
  assert(!(hot->flags & CLIENT_FLAG_UNDECORATED));
  assert(hot->dirty & DIRTY_GEOM);

  stub_last_prop_atom = 0;
  wm_flush_dirty(&s, monotonic_time_ns());

  assert(stub_last_prop_atom == atoms._NET_FRAME_EXTENTS);
  assert(stub_last_prop_len == 4);
  uint32_t* extents_check = (uint32_t*)stub_last_prop_data;
  assert(extents_check[0] == 0);
  assert(extents_check[1] == 0);
  assert(extents_check[2] == 0);
  assert(extents_check[3] == 0);

  hot->dirty = DIRTY_NONE;

  struct {
    xcb_get_property_reply_t rep;
  } empty_reply;
  memset(&empty_reply, 0, sizeof(empty_reply));
  empty_reply.rep.format = 0;
  empty_reply.rep.length = 0;

  wm_handle_reply(&s, &slot, &empty_reply.rep, NULL);

  assert(!hot->gtk_frame_extents_set);
  assert((hot->flags & CLIENT_FLAG_UNDECORATED) == 0);
  assert(hot->dirty & DIRTY_GEOM);

  stub_last_prop_atom = 0;
  wm_flush_dirty(&s, monotonic_time_ns());

  uint16_t bw = s.config.theme.border_width;
  uint16_t hh = s.config.theme.handle_height;
  uint16_t bottom = (hh > bw) ? hh : bw;

  assert(stub_last_prop_atom == atoms._NET_FRAME_EXTENTS);
  uint32_t* extents = (uint32_t*)stub_last_prop_data;
  assert(extents[0] == 5);
  assert(extents[1] == 5);
  assert(extents[2] == 25);
  assert(extents[3] == bottom);

  printf("test_gtk_extents_toggle_decorations passed\n");

  cleanup_server(&s);
}

void test_gtk_configure_request_extents(void) {
  server_t s;
  memset(&s, 0, sizeof(s));

  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t)))
    return;
  small_vec_init(&s.active_clients);

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  small_vec_push(&s.active_clients, handle_to_ptr(h));
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  hot->self = h;
  hot->gtk_frame_extents_set = true;
  hot->gtk_extents.left = 8;
  hot->gtk_extents.right = 8;
  hot->gtk_extents.top = 24;
  hot->gtk_extents.bottom = 8;

  pending_config_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
  ev.x = 200;
  ev.y = 100;
  ev.width = 300;
  ev.height = 200;

  wm_handle_configure_request(&s, h, &ev);

  // Standard behavior: GTK adjustment (shadow offset -> visible pos)
  assert(hot->desired.x == 208);
  assert(hot->desired.y == 124);
  assert(hot->desired.w == 300);
  assert(hot->desired.h == 200);
  assert(hot->dirty & DIRTY_GEOM);

  hot->dirty = DIRTY_NONE;
  ev.width = 10;
  ev.height = 10;

  wm_handle_configure_request(&s, h, &ev);

  assert(hot->desired.w == 10);
  assert(hot->desired.h == 10);
  assert(hot->dirty & DIRTY_GEOM);

  printf("test_gtk_configure_request_extents passed\n");

  small_vec_destroy(&s.active_clients);
  slotmap_destroy(&s.clients);
}

int main(void) {
  test_gtk_extents_toggle_decorations();
  test_gtk_configure_request_extents();
  return 0;
}
