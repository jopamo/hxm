#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "ds.h"
#include "event.h"
#include "handle_conv.h"
#include "src/wm_internal.h"
#include "wm.h"

void test_resize_handle_logic(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  s.root_depth = 24;
  s.root_visual_type = xcb_get_visualtype(NULL, 0);
  s.conn = (xcb_connection_t*)malloc(1);  // Mock connection
  s.root = 1;

  // Init config
  config_init_defaults(&s.config);
  s.config.theme.border_width = 5;
  s.config.theme.title_height = 20;
  s.config.theme.handle_height = 6;

  // Init list heads
  list_init(&s.focus_history);
  for (int i = 0; i < LAYER_COUNT; i++) {
    small_vec_init(&s.layers[i]);
  }

  // Init slotmap
  if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) {
    fprintf(stderr, "Failed to init slotmap\n");
    exit(1);
  }

  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
  assert(h != HANDLE_INVALID);
  client_hot_t* hot = (client_hot_t*)hot_ptr;

  render_init(&hot->render_ctx);

  // Setup client
  hot->state = STATE_MAPPED;
  hot->frame = 999;
  hot->server.x = 100;
  hot->server.y = 100;
  hot->server.w = 200;
  hot->server.h = 200;
  hot->hints.max_w = 1000;
  hot->hints.max_h = 1000;

  list_init(&hot->transient_sibling);
  list_init(&hot->transients_head);
  list_init(&hot->focus_node);

  hot->layer = LAYER_NORMAL;
  stack_raise(&s, h);

  hash_map_init(&s.frame_to_client);
  hash_map_insert(&s.frame_to_client, 999, handle_to_ptr(h));

  xcb_button_press_event_t bev;
  memset(&bev, 0, sizeof(bev));
  bev.event = 999;
  bev.root = 1;
  bev.detail = 1;
  bev.state = 0;
  bev.root_x = 0;
  bev.root_y = 0;

  // Dimensions:
  // Frame W = 200 + 2*5 = 210
  // Frame H = 200 + 20 + 5 = 225 (bottom edge uses border width)
  //
  // Bottom border area: y >= 225 - 5 = 220.
  // Right border X: [210 - 5, 210) = [205, 210).
  // Left border X: [0, 5).

  // Case 1: Click in Bottom border center
  // x = 100 (Center), y = 220.
  bev.event_x = 100;
  bev.event_y = 220;

  wm_handle_button_press(&s, &bev);

  assert(s.interaction_mode == INTERACTION_RESIZE);
  assert(s.interaction_resize_dir == RESIZE_BOTTOM);
  printf("Case 1 Passed: Bottom Border Hit\n");

  s.interaction_mode = INTERACTION_NONE;
  s.interaction_resize_dir = RESIZE_NONE;

  // Case 2: Click in Bottom Right border corner
  bev.event_x = 207;
  bev.event_y = 220;

  wm_handle_button_press(&s, &bev);

  assert(s.interaction_mode == INTERACTION_RESIZE);
  assert(s.interaction_resize_dir == (RESIZE_BOTTOM | RESIZE_RIGHT));
  printf("Case 2 Passed: Bottom Right Border Hit\n");

  s.interaction_mode = INTERACTION_NONE;
  s.interaction_resize_dir = RESIZE_NONE;

  // Case 3: Click in Bottom Left border corner
  bev.event_x = 2;
  bev.event_y = 220;

  wm_handle_button_press(&s, &bev);

  assert(s.interaction_mode == INTERACTION_RESIZE);
  assert(s.interaction_resize_dir == (RESIZE_BOTTOM | RESIZE_LEFT));
  printf("Case 3 Passed: Bottom Left Border Hit\n");

  // Cleanup
  config_destroy(&s.config);
  hash_map_destroy(&s.frame_to_client);

  // Proper cleanup of client
  render_free(&hot->render_ctx);
  slotmap_destroy(&s.clients);

  for (int i = 0; i < LAYER_COUNT; i++)
    small_vec_destroy(&s.layers[i]);

  free(s.conn);
}

int main(void) {
  test_resize_handle_logic();
  return 0;
}
