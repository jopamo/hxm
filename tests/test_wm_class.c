#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

void test_wm_class_split(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t*)malloc(1);
  atoms.WM_CLASS = 1;

  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t)))
    return;

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  assert(h != HANDLE_INVALID);
  assert(hot_ptr != NULL);
  assert(cold_ptr != NULL);
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  client_cold_t* cold = (client_cold_t*)cold_ptr;
  hot->xid = 123;
  hot->state = STATE_NEW;
  hot->pending_replies = 10;
  arena_init(&cold->string_arena, 512);

  // Mock reply for WM_CLASS with "instance\0class\0"
  struct {
    xcb_get_property_reply_t reply;
    char data[32];
  } mock_r;
  memset(&mock_r, 0, sizeof(mock_r));
  mock_r.reply.format = 8;
  mock_r.reply.type = XCB_ATOM_STRING;
  mock_r.reply.value_len = 13;
  memcpy(mock_r.data, "xterm\0XTerm\0", 13);

  cookie_slot_t slot;
  slot.type = COOKIE_GET_PROPERTY;
  slot.client = h;
  slot.data = ((uint64_t)123 << 32) | atoms.WM_CLASS;

  wm_handle_reply(&s, &slot, &mock_r, NULL);

  assert(cold->wm_instance != NULL);
  assert(cold->wm_class != NULL);
  assert(strcmp(cold->wm_instance, "xterm") == 0);
  assert(strcmp(cold->wm_class, "XTerm") == 0);

  // Test update with same values (no new allocation in arena ideally, but at
  // least no change)
  char* old_instance = cold->wm_instance;
  char* old_class = cold->wm_class;
  wm_handle_reply(&s, &slot, &mock_r, NULL);
  assert(cold->wm_instance == old_instance);
  assert(cold->wm_class == old_class);

  // Test update with different values
  mock_r.reply.value_len = 11;
  memcpy(mock_r.data, "urxvt\0URxvt\0", 11);
  wm_handle_reply(&s, &slot, &mock_r, NULL);
  assert(strcmp(cold->wm_instance, "urxvt") == 0);
  assert(strcmp(cold->wm_class, "URxvt") == 0);
  assert(cold->wm_instance != old_instance);

  printf("test_wm_class_split passed\n");

  arena_destroy(&cold->string_arena);
  for (uint32_t i = 1; i < s.clients.cap; i++) {
    if (s.clients.hdr[i].live) {
      handle_t h = handle_make(i, s.clients.hdr[i].gen);
      client_hot_t* hot = server_chot(&s, h);
      if (hot) {
        render_free(&hot->render_ctx);
        if (hot->icon_surface)
          cairo_surface_destroy(hot->icon_surface);
      }
    }
  }
  slotmap_destroy(&s.clients);
  free(s.conn);
}

int main(void) {
  test_wm_class_split();
  return 0;
}
