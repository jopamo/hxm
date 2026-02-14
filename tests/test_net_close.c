#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

// External stubs counters
extern int stub_send_event_count;
extern xcb_window_t stub_last_send_event_destination;
extern char stub_last_event[32];
extern int stub_kill_client_count;
extern uint32_t stub_last_kill_client_resource;

void test_net_close_window(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t*)malloc(1);

  // Setup atoms
  atoms._NET_CLOSE_WINDOW = 100;
  atoms.WM_PROTOCOLS = 10;
  atoms.WM_DELETE_WINDOW = 11;

  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t)))
    return;

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  client_cold_t* cold = (client_cold_t*)cold_ptr;

  hot->xid = 123;
  hot->state = STATE_MAPPED;

  // Associate window with client in the map
  hash_map_init(&s.window_to_client);
  hash_map_insert(&s.window_to_client, hot->xid, (void*)(uintptr_t)h);

  arena_init(&cold->string_arena, 512);

  // Case 1: Client supports WM_DELETE_WINDOW
  // We expect WM_DELETE_WINDOW to be sent to the client
  cold->protocols |= PROTOCOL_DELETE_WINDOW;
  stub_send_event_count = 0;
  stub_kill_client_count = 0;

  xcb_client_message_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = hot->xid;
  ev.type = atoms._NET_CLOSE_WINDOW;
  // timestamp and source indication (2 = pager/other clients)
  ev.data.data32[0] = 0;
  ev.data.data32[1] = 2;

  wm_handle_client_message(&s, &ev);

  assert(stub_send_event_count == 1);
  assert(stub_last_send_event_destination == 123);

  xcb_client_message_event_t* sent_ev = (xcb_client_message_event_t*)stub_last_event;
  assert(sent_ev->type == atoms.WM_PROTOCOLS);
  assert(sent_ev->data.data32[0] == atoms.WM_DELETE_WINDOW);

  printf("test_net_close_window (graceful) passed\n");

  // Case 2: Client does NOT support WM_DELETE_WINDOW
  // We expect xcb_kill_client to be called
  cold->protocols &= ~PROTOCOL_DELETE_WINDOW;
  stub_send_event_count = 0;
  stub_kill_client_count = 0;

  wm_handle_client_message(&s, &ev);

  assert(stub_kill_client_count == 1);
  assert(stub_last_kill_client_resource == 123);

  printf("test_net_close_window (kill) passed\n");

  // Cleanup
  arena_destroy(&cold->string_arena);
  hash_map_destroy(&s.window_to_client);
  slotmap_destroy(&s.clients);
  free(s.conn);
}

int main(void) {
  test_net_close_window();
  return 0;
}
