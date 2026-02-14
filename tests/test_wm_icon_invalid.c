#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

static xcb_get_property_reply_t* make_icon_reply(uint32_t* data, uint32_t count) {
  size_t total = sizeof(xcb_get_property_reply_t) + sizeof(uint32_t) * count;
  xcb_get_property_reply_t* rep = calloc(1, total);
  if (!rep)
    return NULL;
  rep->format = 32;
  rep->type = XCB_ATOM_CARDINAL;
  rep->value_len = count;
  if (count > 0)
    memcpy(xcb_get_property_value(rep), data, sizeof(uint32_t) * count);
  return rep;
}

void test_wm_icon_invalid(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t*)malloc(1);

  atoms._NET_WM_ICON = 99;

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
  hot->state = STATE_MAPPED;
  hot->pending_replies = 1;
  arena_init(&cold->string_arena, 512);

  cookie_slot_t slot;
  slot.type = COOKIE_GET_PROPERTY;
  slot.client = h;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_ICON;

  uint32_t huge_dims[] = {UINT32_MAX, UINT32_MAX};
  xcb_get_property_reply_t* rep = make_icon_reply(huge_dims, 2);
  wm_handle_reply(&s, &slot, rep, NULL);
  free(rep);
  assert(hot->icon_surface == NULL);

  uint32_t big_dims[] = {4097, 4097};
  rep = make_icon_reply(big_dims, 2);
  wm_handle_reply(&s, &slot, rep, NULL);
  free(rep);
  assert(hot->icon_surface == NULL);

  uint32_t truncated[] = {64, 64};
  rep = make_icon_reply(truncated, 2);
  wm_handle_reply(&s, &slot, rep, NULL);
  free(rep);
  assert(hot->icon_surface == NULL);

  printf("test_wm_icon_invalid passed\n");

  arena_destroy(&cold->string_arena);
  if (hot->icon_surface)
    cairo_surface_destroy(hot->icon_surface);
  slotmap_destroy(&s.clients);
  free(s.conn);
}

int main(void) {
  test_wm_icon_invalid();
  return 0;
}
