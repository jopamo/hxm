#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "../src/wm_internal.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

// External stubs from xcb_stubs.c
extern void xcb_stubs_reset(void);
extern void atoms_init(xcb_connection_t* conn);
extern int stub_configure_window_count;
extern xcb_window_t stub_last_config_window;
extern uint16_t stub_last_config_mask;
extern int32_t stub_last_config_x;
extern int32_t stub_last_config_y;
extern uint32_t stub_last_config_w;
extern uint32_t stub_last_config_h;
extern int stub_prop_calls_len;
extern int stub_set_input_focus_count;

// Counters for wrapped functions
static int call_wm_handle_key_press = 0;
static int call_wm_handle_key_release = 0;
static int call_wm_handle_button_press = 0;
static int call_wm_handle_button_release = 0;
static int call_menu_handle_expose_region = 0;
static int call_frame_redraw_region = 0;
static int call_wm_handle_motion_notify = 0;
static int call_wm_update_monitors = 0;
static int call_wm_compute_workarea = 0;
static int call_wm_publish_workarea = 0;
static int call_wm_set_focus = 0;
static handle_t call_wm_set_focus_last = HANDLE_INVALID;

// Wrappers
void __wrap_wm_handle_key_press(server_t* s, xcb_key_press_event_t* ev) {
  (void)s;
  (void)ev;
  call_wm_handle_key_press++;
}

void __wrap_wm_handle_key_release(server_t* s, xcb_key_release_event_t* ev) {
  (void)s;
  (void)ev;
  call_wm_handle_key_release++;
}

void __wrap_wm_handle_button_press(server_t* s, xcb_button_press_event_t* ev) {
  (void)s;
  (void)ev;
  call_wm_handle_button_press++;
}

void __wrap_wm_handle_button_release(server_t* s, xcb_button_release_event_t* ev) {
  (void)s;
  (void)ev;
  call_wm_handle_button_release++;
}

void __wrap_menu_handle_expose_region(server_t* s, dirty_region_t* region) {
  (void)s;
  (void)region;
  call_menu_handle_expose_region++;
}

void __wrap_frame_redraw_region(server_t* s, handle_t h, dirty_region_t* region) {
  (void)s;
  (void)h;
  (void)region;
  call_frame_redraw_region++;
}

void __wrap_wm_handle_motion_notify(server_t* s, xcb_motion_notify_event_t* ev) {
  (void)s;
  (void)ev;
  call_wm_handle_motion_notify++;
}

void __wrap_wm_update_monitors(server_t* s) {
  (void)s;
  call_wm_update_monitors++;
}

void __wrap_wm_compute_workarea(server_t* s, rect_t* wa) {
  (void)s;
  (void)wa;
  call_wm_compute_workarea++;
}

void __wrap_wm_publish_workarea(server_t* s, const rect_t* wa) {
  (void)s;
  (void)wa;
  call_wm_publish_workarea++;
}

void __wrap_wm_set_focus(server_t* s, handle_t h) {
  (void)s;
  call_wm_set_focus++;
  call_wm_set_focus_last = h;
}

static void reset_counters(void) {
  call_wm_handle_key_press = 0;
  call_wm_handle_key_release = 0;
  call_wm_handle_button_press = 0;
  call_wm_handle_button_release = 0;
  call_menu_handle_expose_region = 0;
  call_frame_redraw_region = 0;
  call_wm_handle_motion_notify = 0;
  call_wm_update_monitors = 0;
  call_wm_compute_workarea = 0;
  call_wm_publish_workarea = 0;
  call_wm_set_focus = 0;
  call_wm_set_focus_last = HANDLE_INVALID;
}

static void setup_server(server_t* s) {
  memset(s, 0, sizeof(*s));
  s->conn = xcb_connect(NULL, NULL);
  atoms_init(s->conn);
  arena_init(&s->tick_arena, 1024);

  small_vec_init(&s->buckets.map_requests);
  small_vec_init(&s->buckets.unmap_notifies);
  small_vec_init(&s->buckets.destroy_notifies);
  small_vec_init(&s->buckets.key_presses);
  small_vec_init(&s->buckets.key_releases);
  small_vec_init(&s->buckets.button_events);
  small_vec_init(&s->buckets.client_messages);

  hash_map_init(&s->buckets.expose_regions);
  hash_map_init(&s->buckets.configure_requests);
  hash_map_init(&s->buckets.configure_notifies);
  hash_map_init(&s->buckets.destroyed_windows);
  hash_map_init(&s->buckets.property_notifies);
  hash_map_init(&s->buckets.motion_notifies);
  hash_map_init(&s->buckets.damage_regions);

  hash_map_init(&s->window_to_client);
  hash_map_init(&s->frame_to_client);
  small_vec_init(&s->active_clients);
  list_init(&s->focus_history);
  bool ok = slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
  assert(ok);
}

static void cleanup_server(server_t* s) {
  small_vec_destroy(&s->buckets.map_requests);
  small_vec_destroy(&s->buckets.unmap_notifies);
  small_vec_destroy(&s->buckets.destroy_notifies);
  small_vec_destroy(&s->buckets.key_presses);
  small_vec_destroy(&s->buckets.key_releases);
  small_vec_destroy(&s->buckets.button_events);
  small_vec_destroy(&s->buckets.client_messages);

  hash_map_destroy(&s->buckets.expose_regions);
  hash_map_destroy(&s->buckets.configure_requests);
  hash_map_destroy(&s->buckets.configure_notifies);
  hash_map_destroy(&s->buckets.destroyed_windows);
  hash_map_destroy(&s->buckets.property_notifies);
  hash_map_destroy(&s->buckets.motion_notifies);
  hash_map_destroy(&s->buckets.damage_regions);

  hash_map_destroy(&s->window_to_client);
  hash_map_destroy(&s->frame_to_client);
  slotmap_destroy(&s->clients);
  small_vec_destroy(&s->active_clients);

  arena_destroy(&s->tick_arena);
  xcb_disconnect(s->conn);
}

static handle_t add_mapped_client(server_t* s, xcb_window_t xid, xcb_window_t frame) {
  void* hot_ptr = NULL;
  void* cold_ptr = NULL;
  handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
  assert(h != HANDLE_INVALID);
  (void)cold_ptr;

  client_hot_t* hot = (client_hot_t*)hot_ptr;
  memset(hot, 0, sizeof(*hot));
  hot->self = h;
  hot->xid = xid;
  hot->frame = frame;
  hot->state = STATE_MAPPED;
  list_init(&hot->focus_node);

  small_vec_push(&s->active_clients, handle_to_ptr(h));
  hash_map_insert(&s->window_to_client, xid, handle_to_ptr(h));
  hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));
  return h;
}

static void test_6_1_key_press_dispatch(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  xcb_key_press_event_t* ev = arena_alloc(&s.tick_arena, sizeof(*ev));
  ev->response_type = XCB_KEY_PRESS;
  ev->detail = 10;
  small_vec_push(&s.buckets.key_presses, ev);

  event_process(&s);

  assert(call_wm_handle_key_press == 1);
  assert(call_wm_handle_key_release == 0);
  printf("test_6_1_key_press_dispatch passed\n");
  cleanup_server(&s);
}

static void test_6_2_button_events_dispatch(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  xcb_button_press_event_t* bp = arena_alloc(&s.tick_arena, sizeof(*bp));
  bp->response_type = XCB_BUTTON_PRESS;
  small_vec_push(&s.buckets.button_events, bp);

  xcb_button_release_event_t* br = arena_alloc(&s.tick_arena, sizeof(*br));
  br->response_type = XCB_BUTTON_RELEASE;
  small_vec_push(&s.buckets.button_events, br);

  event_process(&s);

  assert(call_wm_handle_button_press == 1);
  assert(call_wm_handle_button_release == 1);
  printf("test_6_2_button_events_dispatch passed\n");
  cleanup_server(&s);
}

static void test_6_3_menu_expose_dispatch(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  s.menu.window = 0xabc;

  dirty_region_t* region = arena_alloc(&s.tick_arena, sizeof(*region));
  *region = dirty_region_make(0, 0, 100, 100);
  hash_map_insert(&s.buckets.expose_regions, s.menu.window, region);

  event_process(&s);

  assert(call_menu_handle_expose_region == 1);
  assert(call_frame_redraw_region == 0);

  printf("test_6_3_menu_expose_dispatch passed\n");
  cleanup_server(&s);
}

static void test_6_4_motion_notify_dispatch(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  xcb_motion_notify_event_t* mn = arena_alloc(&s.tick_arena, sizeof(*mn));
  mn->event = 0x123;
  hash_map_insert(&s.buckets.motion_notifies, mn->event, mn);

  event_process(&s);

  assert(call_wm_handle_motion_notify == 1);

  printf("test_6_4_motion_notify_dispatch passed\n");
  cleanup_server(&s);
}

static void test_6_5_configure_request_unknown_window(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  xcb_window_t win = 0x999;
  pending_config_t* pc = arena_alloc(&s.tick_arena, sizeof(*pc));
  pc->window = win;
  pc->x = 50;
  pc->y = 60;
  pc->width = 200;
  pc->height = 150;
  pc->mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

  hash_map_insert(&s.buckets.configure_requests, win, pc);

  // No client registered for 0x999, so it is unknown.

  event_process(&s);

  assert(stub_configure_window_count == 1);
  assert(stub_last_config_window == win);
  assert(stub_last_config_x == 50);
  assert(stub_last_config_y == 60);
  assert(stub_last_config_w == 200);
  assert(stub_last_config_h == 150);

  printf("test_6_5_configure_request_unknown_window passed\n");
  cleanup_server(&s);
}

static void test_6_6_randr_dirty_processing(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  s.buckets.randr_dirty = true;
  s.buckets.randr_width = 1920;
  s.buckets.randr_height = 1080;

  event_process(&s);

  assert(call_wm_update_monitors == 1);
  assert(call_wm_compute_workarea == 0);
  assert(call_wm_publish_workarea == 0);

  // event_process updates _NET_DESKTOP_GEOMETRY immediately while monitor-
  // dependent workarea publication is deferred until async RandR replies.
  assert(stub_prop_calls_len > 0);

  printf("test_6_6_randr_dirty_processing passed\n");
  cleanup_server(&s);
}

static void test_6_7_focus_in_dispatches_set_focus(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  s.root = 0x1;
  handle_t h = add_mapped_client(&s, 0x701, 0x801);

  s.buckets.focus_notify.in_valid = true;
  s.buckets.focus_notify.in.event = 0x701;
  s.buckets.focus_notify.in.mode = XCB_NOTIFY_MODE_NORMAL;
  s.buckets.focus_notify.in.detail = XCB_NOTIFY_DETAIL_NONLINEAR;
  s.buckets.focus_notify.in.sequence = 100;

  event_process(&s);

  assert(call_wm_set_focus == 1);
  assert(call_wm_set_focus_last == h);
  assert(s.last_focus_sequence == 100);

  printf("test_6_7_focus_in_dispatches_set_focus passed\n");
  cleanup_server(&s);
}

static void test_6_8_focus_noise_is_filtered(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  add_mapped_client(&s, 0x702, 0x802);

  s.buckets.focus_notify.in_valid = true;
  s.buckets.focus_notify.in.event = 0x702;
  s.buckets.focus_notify.in.mode = XCB_NOTIFY_MODE_GRAB;
  s.buckets.focus_notify.in.detail = XCB_NOTIFY_DETAIL_NONLINEAR;
  s.buckets.focus_notify.in.sequence = 110;

  event_process(&s);

  assert(call_wm_set_focus == 0);
  assert(s.last_focus_sequence == 0);

  printf("test_6_8_focus_noise_is_filtered passed\n");
  cleanup_server(&s);
}

static void test_6_9_focus_stale_sequence_is_filtered(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  add_mapped_client(&s, 0x703, 0x803);
  s.last_focus_sequence = 200;

  s.buckets.focus_notify.in_valid = true;
  s.buckets.focus_notify.in.event = 0x703;
  s.buckets.focus_notify.in.mode = XCB_NOTIFY_MODE_NORMAL;
  s.buckets.focus_notify.in.detail = XCB_NOTIFY_DETAIL_NONLINEAR;
  s.buckets.focus_notify.in.sequence = 150;

  event_process(&s);

  assert(call_wm_set_focus == 0);
  assert(s.last_focus_sequence == 200);

  printf("test_6_9_focus_stale_sequence_is_filtered passed\n");
  cleanup_server(&s);
}

static void test_6_10_focus_target_destroyed_is_filtered(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  add_mapped_client(&s, 0x704, 0x804);

  s.buckets.focus_notify.in_valid = true;
  s.buckets.focus_notify.in.event = 0x704;
  s.buckets.focus_notify.in.mode = XCB_NOTIFY_MODE_NORMAL;
  s.buckets.focus_notify.in.detail = XCB_NOTIFY_DETAIL_NONLINEAR;
  s.buckets.focus_notify.in.sequence = 220;
  hash_map_insert(&s.buckets.destroyed_windows, 0x704, (void*)1);

  event_process(&s);

  assert(call_wm_set_focus == 0);
  assert(s.last_focus_sequence == 0);

  printf("test_6_10_focus_target_destroyed_is_filtered passed\n");
  cleanup_server(&s);
}

static void test_6_11_focus_out_clears_focus(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  handle_t h = add_mapped_client(&s, 0x705, 0x805);
  s.focused_client = h;

  s.buckets.focus_notify.out_valid = true;
  s.buckets.focus_notify.out.event = 0x705;
  s.buckets.focus_notify.out.mode = XCB_NOTIFY_MODE_NORMAL;
  s.buckets.focus_notify.out.detail = XCB_NOTIFY_DETAIL_NONLINEAR_VIRTUAL;
  s.buckets.focus_notify.out.sequence = 230;

  event_process(&s);

  assert(call_wm_set_focus == 1);
  assert(call_wm_set_focus_last == HANDLE_INVALID);
  assert(s.last_focus_sequence == 230);

  printf("test_6_11_focus_out_clears_focus passed\n");
  cleanup_server(&s);
}

static void test_6_12_pointer_notify_is_hint_only(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  s.last_pointer_hint_time = 500;
  s.buckets.pointer_notify.enter_valid = true;
  s.buckets.pointer_notify.enter.time = 450;
  s.buckets.pointer_notify.leave_valid = true;
  s.buckets.pointer_notify.leave.time = 700;

  event_process(&s);

  assert(call_wm_set_focus == 0);
  assert(s.last_pointer_hint_time == 700);

  // Older timestamp should be ignored.
  s.buckets.pointer_notify.enter_valid = true;
  s.buckets.pointer_notify.enter.time = 650;
  s.buckets.pointer_notify.leave_valid = false;
  event_process(&s);
  assert(s.last_pointer_hint_time == 700);

  printf("test_6_12_pointer_notify_is_hint_only passed\n");
  cleanup_server(&s);
}

static void test_6_13_stale_root_focus_out_after_focus_commit_is_filtered(void) {
  server_t s;
  setup_server(&s);
  xcb_stubs_reset();
  reset_counters();

  s.root = 0x1;
  handle_t old_focus = add_mapped_client(&s, 0x706, 0x806);
  handle_t new_focus = add_mapped_client(&s, 0x707, 0x807);

  client_cold_t* cold = server_ccold(&s, new_focus);
  assert(cold != NULL);
  cold->can_focus = true;

  s.focused_client = new_focus;
  s.committed_focus = 0x706;
  s.last_focus_sequence = 0;

  bool flushed = wm_flush_dirty(&s, 0);
  assert(flushed);
  assert(stub_set_input_focus_count == 1);
  (void)old_focus;  // Keep old focus live in maps for event resolution.
  assert(s.last_focus_sequence != 0);

  uint16_t committed_focus_seq = s.last_focus_sequence;

  s.buckets.focus_notify.out_valid = true;
  s.buckets.focus_notify.out.event = s.root;
  s.buckets.focus_notify.out.mode = XCB_NOTIFY_MODE_NORMAL;
  s.buckets.focus_notify.out.detail = XCB_NOTIFY_DETAIL_POINTER_ROOT;
  s.buckets.focus_notify.out.sequence = (uint16_t)(committed_focus_seq - 1u);

  reset_counters();
  event_process(&s);

  assert(call_wm_set_focus == 0);
  assert(s.last_focus_sequence == committed_focus_seq);

  printf("test_6_13_stale_root_focus_out_after_focus_commit_is_filtered passed\n");
  cleanup_server(&s);
}

int main(void) {
  test_6_1_key_press_dispatch();
  test_6_2_button_events_dispatch();
  test_6_3_menu_expose_dispatch();
  test_6_4_motion_notify_dispatch();
  test_6_5_configure_request_unknown_window();
  test_6_6_randr_dirty_processing();
  test_6_7_focus_in_dispatches_set_focus();
  test_6_8_focus_noise_is_filtered();
  test_6_9_focus_stale_sequence_is_filtered();
  test_6_10_focus_target_destroyed_is_filtered();
  test_6_11_focus_out_clears_focus();
  test_6_12_pointer_notify_is_hint_only();
  test_6_13_stale_root_focus_out_after_focus_commit_is_filtered();
  return 0;
}
