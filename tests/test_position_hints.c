#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "event.h"
#include "wm.h"

static void setup_server(server_t* s) {
  memset(s, 0, sizeof(*s));
  s->is_test = true;
  s->conn = xcb_connect(NULL, NULL);
  s->workarea.x = 0;
  s->workarea.y = 0;
  s->workarea.w = 800;
  s->workarea.h = 600;
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
  xcb_disconnect(s->conn);
}

static handle_t add_client(server_t* s, int16_t x, int16_t y, uint16_t w, uint16_t height) {
  void *hot_ptr = NULL, *cold_ptr = NULL;
  handle_t handle = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
  client_hot_t* hot = (client_hot_t*)hot_ptr;
  client_cold_t* cold = (client_cold_t*)cold_ptr;
  memset(hot, 0, sizeof(*hot));
  memset(cold, 0, sizeof(*cold));
  render_init(&hot->render_ctx);
  arena_init(&cold->string_arena, 128);
  hot->self = handle;
  hot->type = WINDOW_TYPE_NORMAL;
  hot->placement = PLACEMENT_DEFAULT;
  hot->desired = (rect_t){x, y, w, height};
  hot->server = hot->desired;
  list_init(&hot->focus_node);
  list_init(&hot->transients_head);
  list_init(&hot->transient_sibling);
  return handle;
}

static bool rects_intersect(const rect_t* a, const rect_t* b) {
  int32_t ax2 = (int32_t)a->x + (int32_t)a->w;
  int32_t ay2 = (int32_t)a->y + (int32_t)a->h;
  int32_t bx2 = (int32_t)b->x + (int32_t)b->w;
  int32_t by2 = (int32_t)b->y + (int32_t)b->h;
  return !(ax2 <= b->x || ay2 <= b->y || a->x >= bx2 || a->y >= by2);
}

static bool rect_intersects_any_monitor(const server_t* s, const rect_t* r) {
  if (!s || !s->monitors || s->monitor_count == 0)
    return false;
  for (uint32_t i = 0; i < s->monitor_count; i++) {
    if (rects_intersect(r, &s->monitors[i].geom))
      return true;
  }
  return false;
}

static void test_us_position_preserved(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, 100, 200, 120, 80);
  client_hot_t* hot = server_chot(&s, h);
  hot->hints_flags = XCB_ICCCM_SIZE_HINT_US_POSITION;

  wm_place_window(&s, h);
  assert(hot->desired.x == 100);
  assert(hot->desired.y == 200);

  printf("test_us_position_preserved passed\n");
  cleanup_server(&s);
}

static void test_p_position_preserved(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, 50, 60, 120, 80);
  client_hot_t* hot = server_chot(&s, h);
  hot->hints_flags = XCB_ICCCM_SIZE_HINT_P_POSITION;

  wm_place_window(&s, h);
  assert(hot->desired.x == 50);
  assert(hot->desired.y == 60);

  printf("test_p_position_preserved passed\n");
  cleanup_server(&s);
}

static void test_position_clamped_without_hint(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, -10, -10, 120, 80);
  client_hot_t* hot = server_chot(&s, h);

  wm_place_window(&s, h);
  assert(hot->desired.x == 0);
  assert(hot->desired.y == 0);

  printf("test_position_clamped_without_hint passed\n");
  cleanup_server(&s);
}

static void test_position_intersects_workarea_after_place(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, -20000, -20000, 120, 80);
  client_hot_t* hot = server_chot(&s, h);

  wm_place_window(&s, h);

  rect_t wa = s.workarea;
  assert(rects_intersect(&hot->desired, &wa));

  printf("test_position_intersects_workarea_after_place passed\n");
  cleanup_server(&s);
}

static void test_position_intersects_workarea_after_place_far_positive(void) {
  server_t s;
  setup_server(&s);

  handle_t h = add_client(&s, 9000, 7000, 200, 200);
  client_hot_t* hot = server_chot(&s, h);

  wm_place_window(&s, h);

  rect_t wa = s.workarea;
  assert(rects_intersect(&hot->desired, &wa));

  printf("test_position_intersects_workarea_after_place_far_positive passed\n");
  cleanup_server(&s);
}

static void test_position_intersects_monitor_multihead(void) {
  server_t s;
  setup_server(&s);

  s.monitor_count = 2;
  s.monitors = calloc(2, sizeof(monitor_t));
  assert(s.monitors != NULL);

  s.monitors[0].geom = (rect_t){0, 0, 800, 600};
  s.monitors[1].geom = (rect_t){800, 0, 800, 600};

  // Use a workarea that spans both monitors to simulate union placement.
  s.workarea = (rect_t){0, 0, 1600, 600};

  handle_t h = add_client(&s, 20000, 20000, 200, 200);
  client_hot_t* hot = server_chot(&s, h);

  wm_place_window(&s, h);

  assert(rect_intersects_any_monitor(&s, &hot->desired));

  printf("test_position_intersects_monitor_multihead passed\n");
  free(s.monitors);
  s.monitors = NULL;
  s.monitor_count = 0;
  cleanup_server(&s);
}

int main(void) {
  test_us_position_preserved();
  test_p_position_preserved();
  test_position_clamped_without_hint();
  test_position_intersects_workarea_after_place();
  test_position_intersects_workarea_after_place_far_positive();
  test_position_intersects_monitor_multihead();
  return 0;
}
