#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"

// Mock server and atoms
server_t s;

// Mock handler
static bool g_handler_called = false;
static void mock_handler(server_t* s, const cookie_slot_t* slot, void* reply, xcb_generic_error_t* err) {
  (void)s;
  (void)slot;
  (void)reply;
  (void)err;
  g_handler_called = true;
}

void test_stale_reply_dropped(void) {
  memset(&s, 0, sizeof(s));
  slotmap_init(&s.clients, 10, sizeof(client_hot_t), sizeof(client_cold_t));

  handle_t h = slotmap_alloc(&s.clients, NULL, NULL);
  client_hot_t* hot = server_chot(&s, h);
  hot->self = h;
  hot->xid = 123;
  hot->last_applied_txn_id = 10;

  // A reply with txn_id 5 (stale)
  cookie_slot_t stale_slot = {.sequence = 1, .type = COOKIE_GET_PROPERTY, .client = h, .txn_id = 5, .handler = mock_handler, .live = true};

  xcb_get_property_reply_t dummy_reply;
  memset(&dummy_reply, 0, sizeof(dummy_reply));

  g_handler_called = false;
  wm_handle_reply(&s, &stale_slot, &dummy_reply, NULL);
  assert(hot->last_applied_txn_id == 10);

  // A reply with txn_id 15 (fresh)
  cookie_slot_t fresh_slot = {.sequence = 2, .type = COOKIE_GET_PROPERTY, .client = h, .txn_id = 15, .handler = mock_handler, .live = true};

  wm_handle_reply(&s, &fresh_slot, &dummy_reply, NULL);
  assert(hot->last_applied_txn_id == 15);

  printf("test_stale_reply_dropped passed\n");
}

int main(void) {
  test_stale_reply_dropped();
  return 0;
}
