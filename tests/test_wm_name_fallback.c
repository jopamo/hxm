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

bool __real_cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data, uint64_t txn_id, cookie_handler_fn handler);

static bool g_force_cookie_push_failure = false;

bool __wrap_cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data, uint64_t txn_id, cookie_handler_fn handler) {
  if (g_force_cookie_push_failure)
    return false;
  return __real_cookie_jar_push(cj, sequence, type, client, data, txn_id, handler);
}

static xcb_get_property_reply_t* make_string_reply(xcb_atom_t type, const char* value, int len) {
  size_t total = sizeof(xcb_get_property_reply_t) + (size_t)len;
  xcb_get_property_reply_t* rep = calloc(1, total);
  if (!rep)
    return NULL;
  rep->format = 8;
  rep->type = type;
  rep->value_len = (uint32_t)len;
  if (len > 0 && value) {
    memcpy(xcb_get_property_value(rep), value, (size_t)len);
  }
  return rep;
}

void test_net_wm_name_fallback(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.conn = xcb_connect(NULL, NULL);
  g_force_cookie_push_failure = false;

  atoms._NET_WM_NAME = 10;
  atoms.WM_NAME = 11;
  atoms.UTF8_STRING = 12;

  cookie_jar_init(&s.cookie_jar);
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

  xcb_get_property_reply_t* rep = make_string_reply(atoms.UTF8_STRING, "modern", 6);
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;
  wm_handle_reply(&s, &slot, rep, NULL);
  free(rep);

  assert(cold->has_net_wm_name);
  assert(strcmp(cold->title, "modern") == 0);

  rep = make_string_reply(XCB_ATOM_STRING, "legacy", 6);
  slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_NAME;
  wm_handle_reply(&s, &slot, rep, NULL);
  free(rep);
  assert(strcmp(cold->title, "modern") == 0);

  rep = make_string_reply(atoms.UTF8_STRING, "", 0);
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;
  wm_handle_reply(&s, &slot, rep, NULL);
  free(rep);
  assert(!cold->has_net_wm_name);

  rep = make_string_reply(XCB_ATOM_STRING, "legacy", 6);
  slot.data = ((uint64_t)hot->xid << 32) | atoms.WM_NAME;
  wm_handle_reply(&s, &slot, rep, NULL);
  free(rep);
  assert(strcmp(cold->title, "legacy") == 0);

  printf("test_net_wm_name_fallback passed\n");

  cookie_jar_destroy(&s.cookie_jar);
  arena_destroy(&cold->string_arena);
  slotmap_destroy(&s.clients);
  xcb_disconnect(s.conn);
}

void test_net_wm_name_fallback_enqueue_failure_finalizes_manage(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.conn = xcb_connect(NULL, NULL);

  atoms._NET_WM_NAME = 20;
  atoms.WM_NAME = 21;
  atoms.UTF8_STRING = 22;

  cookie_jar_init(&s.cookie_jar);
  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t)))
    return;

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  assert(h != HANDLE_INVALID);
  assert(hot_ptr != NULL);
  assert(cold_ptr != NULL);
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  client_cold_t* cold = (client_cold_t*)cold_ptr;
  hot->xid = 456;
  hot->state = STATE_NEW;
  hot->manage_phase = MANAGE_PHASE1;
  hot->pending_replies = 1;
  arena_init(&cold->string_arena, 512);

  cookie_slot_t slot;
  memset(&slot, 0, sizeof(slot));
  slot.type = COOKIE_GET_PROPERTY;
  slot.client = h;
  slot.data = ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME;

  xcb_get_property_reply_t* rep = make_string_reply(atoms.UTF8_STRING, "", 0);
  g_force_cookie_push_failure = true;
  wm_handle_reply(&s, &slot, rep, NULL);
  g_force_cookie_push_failure = false;
  free(rep);

  assert(hot->pending_replies == 0);
  assert(hot->state == STATE_READY);
  assert(!cookie_jar_has_pending(&s.cookie_jar));

  printf("test_net_wm_name_fallback_enqueue_failure_finalizes_manage passed\n");

  cookie_jar_destroy(&s.cookie_jar);
  arena_destroy(&cold->string_arena);
  slotmap_destroy(&s.clients);
  xcb_disconnect(s.conn);
}

int main(void) {
  test_net_wm_name_fallback();
  test_net_wm_name_fallback_enqueue_failure_finalizes_manage();
  return 0;
}
