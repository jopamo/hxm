#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "src/wm_internal.h"  // For wm_handle_reply prototype
#include "wm.h"
#include "xcb_utils.h"

// Stubs for XCB reply structures from xcb/xproto.h
// We need to define the structure layout to match what wm_handle_reply expects
// cast to xcb_get_geometry_reply_t*

void test_tiny_window_expansion(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t*)malloc(1);

  // Setup basic config
  config_init_defaults(&s.config);

  // Initialize slotmap
  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) {
    fprintf(stderr, "Failed to init slotmap\n");
    exit(1);
  }

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  hot->self = h;
  hot->xid = 123;
  hot->state = STATE_NEW;
  hot->manage_phase = MANAGE_PHASE1;  // We are in initial discovery

  // Initial geometry is 0 (uninitialized)
  hot->server.w = 0;
  hot->server.h = 0;
  hot->desired.w = 0;
  hot->desired.h = 0;

  // Construct a fake reply for COOKIE_GET_GEOMETRY
  xcb_get_geometry_reply_t reply;
  memset(&reply, 0, sizeof(reply));
  reply.response_type = 1;
  reply.depth = 24;
  reply.sequence = 0;
  reply.length = 0;
  reply.root = 1;
  reply.x = 0;
  reply.y = 0;
  reply.width = 1;   // TINY WINDOW
  reply.height = 1;  // TINY WINDOW
  reply.border_width = 0;

  cookie_slot_t slot;
  slot.client = h;
  slot.type = COOKIE_GET_GEOMETRY;
  slot.data = 0;
  slot.sequence = 0;
  slot.txn_id = 0;

  // Process the reply
  wm_handle_reply(&s, &slot, &reply, NULL);

  // Assert that the window WAS "rescued" (expanded to default 800x600 or
  // reasonable min) The policy is now to expand tiny windows to at least 50x20
  // or default 800x600
  printf("Geometry after reply: %dx%d\n", hot->server.w, hot->server.h);

  if (hot->server.w >= 50 && hot->server.h >= 20) {
    printf("PASS: Tiny window WAS expanded (Policy confirmed)\n");
  }
  else {
    printf("FAIL: Tiny window was NOT expanded. Geometry: %dx%d\n", hot->server.w, hot->server.h);
    assert(hot->server.w >= 50);
    assert(hot->server.h >= 20);
  }

  // Cleanup
  slotmap_destroy(&s.clients);
  config_destroy(&s.config);
  free(s.conn);
}

int main(void) {
  test_tiny_window_expansion();
  return 0;
}