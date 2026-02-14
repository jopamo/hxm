#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern int stub_install_colormap_count;
extern xcb_colormap_t stub_last_installed_colormap;

static void setup_server(server_t* s) {
  memset(s, 0, sizeof(*s));
  s->is_test = true;
  xcb_stubs_reset();
  s->conn = xcb_connect(NULL, NULL);
  atoms_init(s->conn);

  s->root = 1;
  s->default_colormap = 555;

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
      if (cold) {
        if (cold->colormap_windows)
          free(cold->colormap_windows);
        arena_destroy(&cold->string_arena);
      }
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
  hot->state = STATE_MAPPED;
  hot->stacking_index = -1;
  hot->stacking_layer = -1;
  list_init(&hot->focus_node);
  list_init(&hot->transients_head);
  list_init(&hot->transient_sibling);

  cold->can_focus = false;

  hash_map_insert(&s->window_to_client, xid, handle_to_ptr(h));
  hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));
  return h;
}

static void test_colormap_fallback_install(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, 100, 110);
  client_hot_t* hot = server_chot(&s, h);
  hot->colormap = 10;
  hot->frame_colormap_owned = true;
  hot->frame_colormap = 11;

  stub_install_colormap_count = 0;
  wm_set_focus(&s, h);
  wm_flush_dirty(&s, monotonic_time_ns());
  assert(stub_install_colormap_count == 2);
  assert(stub_last_installed_colormap == 11);

  printf("test_colormap_fallback_install passed\n");
  cleanup_server(&s);
}

static void test_colormap_windows_list_install(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, 200, 210);
  client_hot_t* hot = server_chot(&s, h);
  client_cold_t* cold = server_ccold(&s, h);
  hot->colormap = 20;
  hot->frame_colormap_owned = true;
  hot->frame_colormap = 21;

  cold->colormap_windows_len = 2;
  cold->colormap_windows = malloc(sizeof(xcb_window_t) * 2);
  cold->colormap_windows[0] = hot->xid;
  cold->colormap_windows[1] = hot->frame;

  stub_install_colormap_count = 0;
  wm_set_focus(&s, h);
  wm_flush_dirty(&s, monotonic_time_ns());
  assert(stub_install_colormap_count == 2);
  assert(stub_last_installed_colormap == 21);

  printf("test_colormap_windows_list_install passed\n");
  cleanup_server(&s);
}

static void test_colormap_windows_update_on_focus(void) {
  server_t s;
  setup_server(&s);

  atoms.WM_COLORMAP_WINDOWS = 500;
  handle_t h = add_client(&s, 300, 310);
  client_hot_t* hot = server_chot(&s, h);
  hot->colormap = 30;

  wm_set_focus(&s, h);
  stub_install_colormap_count = 0;

  struct {
    xcb_get_property_reply_t r;
    xcb_window_t wins[1];
  } reply;
  memset(&reply, 0, sizeof(reply));
  reply.r.format = 32;
  reply.r.type = XCB_ATOM_WINDOW;
  reply.r.value_len = 1;
  reply.wins[0] = hot->xid;

  cookie_slot_t slot = {0};
  slot.type = COOKIE_GET_PROPERTY;
  slot.client = h;
  slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_COLORMAP_WINDOWS;

  wm_handle_reply(&s, &slot, &reply.r, NULL);
  assert(stub_install_colormap_count == 1);
  assert(stub_last_installed_colormap == hot->colormap);

  printf("test_colormap_windows_update_on_focus passed\n");
  cleanup_server(&s);
}

static void test_colormap_notify_triggers_install(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, 400, 410);
  client_hot_t* hot = server_chot(&s, h);
  client_cold_t* cold = server_ccold(&s, h);
  hot->colormap = 40;
  cold->colormap_windows_len = 1;
  cold->colormap_windows = malloc(sizeof(xcb_window_t));
  cold->colormap_windows[0] = hot->xid;

  wm_set_focus(&s, h);
  stub_install_colormap_count = 0;

  xcb_colormap_notify_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.window = hot->xid;
  wm_handle_colormap_notify(&s, &ev);

  assert(stub_install_colormap_count == 1);
  assert(stub_last_installed_colormap == hot->colormap);

  printf("test_colormap_notify_triggers_install passed\n");
  cleanup_server(&s);
}

int main(void) {
  test_colormap_fallback_install();
  test_colormap_windows_list_install();
  test_colormap_windows_update_on_focus();
  test_colormap_notify_triggers_install();
  return 0;
}
