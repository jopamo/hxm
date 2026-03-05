#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/randr.h>

#include "cookie_jar.h"
#include "event.h"
#include "xcb_utils.h"
#include "../src/wm_internal.h"

extern void xcb_stubs_reset(void);
extern int (*stub_poll_for_reply_hook)(xcb_connection_t* c, unsigned int request, void** reply, xcb_generic_error_t** error);

bool __real_cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data, uint64_t txn_id, cookie_handler_fn handler);
xcb_randr_get_crtc_info_cookie_t __real_xcb_randr_get_crtc_info(xcb_connection_t* c, xcb_randr_crtc_t crtc, xcb_timestamp_t config_timestamp);

static bool g_fail_crtc_enqueue = false;
static bool g_force_zero_crtc_sequence = false;
static uint32_t g_resources_sequence = 0;
static bool g_resources_reply_delivered = false;
static int g_fake_crtc_count = 2;
static xcb_randr_crtc_t g_fake_crtcs[2] = {11, 22};

bool __wrap_cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data, uint64_t txn_id, cookie_handler_fn handler) {
  if (type == COOKIE_RANDR_GET_SCREEN_RESOURCES)
    g_resources_sequence = sequence;
  if (g_fail_crtc_enqueue && type == COOKIE_RANDR_GET_CRTC_INFO)
    return false;
  return __real_cookie_jar_push(cj, sequence, type, client, data, txn_id, handler);
}

xcb_randr_get_crtc_info_cookie_t __wrap_xcb_randr_get_crtc_info(xcb_connection_t* c, xcb_randr_crtc_t crtc, xcb_timestamp_t config_timestamp) {
  if (g_force_zero_crtc_sequence) {
    (void)c;
    (void)crtc;
    (void)config_timestamp;
    return (xcb_randr_get_crtc_info_cookie_t){0};
  }
  return __real_xcb_randr_get_crtc_info(c, crtc, config_timestamp);
}

int __wrap_xcb_randr_get_screen_resources_current_crtcs_length(const xcb_randr_get_screen_resources_current_reply_t* r) {
  (void)r;
  return g_fake_crtc_count;
}

xcb_randr_crtc_t* __wrap_xcb_randr_get_screen_resources_current_crtcs(const xcb_randr_get_screen_resources_current_reply_t* r) {
  (void)r;
  return g_fake_crtcs;
}

static int randr_poll_for_reply(xcb_connection_t* c, unsigned int request, void** reply, xcb_generic_error_t** error) {
  (void)c;
  if (error)
    *error = NULL;
  if (request != g_resources_sequence || g_resources_reply_delivered)
    return 0;

  xcb_randr_get_screen_resources_current_reply_t* res = calloc(1, sizeof(*res));
  assert(res != NULL);
  res->config_timestamp = 1;
  if (reply)
    *reply = res;
  g_resources_reply_delivered = true;
  return 1;
}

static void setup_server(server_t* s) {
  memset(s, 0, sizeof(*s));
  xcb_stubs_reset();

  s->conn = xcb_connect(NULL, NULL);
  atoms_init(s->conn);
  s->root = 1;
  s->randr_supported = true;

  cookie_jar_init(&s->cookie_jar);
  small_vec_init(&s->active_clients);
}

static void cleanup_server(server_t* s) {
  if (s->monitors)
    free(s->monitors);
  if (s->randr_pending_monitors)
    free(s->randr_pending_monitors);
  small_vec_destroy(&s->active_clients);
  cookie_jar_destroy(&s->cookie_jar);
  xcb_disconnect(s->conn);
  stub_poll_for_reply_hook = NULL;
}

static void test_randr_crtc_enqueue_failure_clears_pending_accounting(void) {
  server_t s;
  setup_server(&s);

  g_fail_crtc_enqueue = true;
  g_resources_sequence = 0;
  g_resources_reply_delivered = false;
  stub_poll_for_reply_hook = randr_poll_for_reply;

  wm_update_monitors(&s);
  assert(g_resources_sequence != 0);
  assert(cookie_jar_has_pending(&s.cookie_jar));

  cookie_jar_mark_replies_may_exist(&s.cookie_jar);
  cookie_jar_drain(&s.cookie_jar, s.conn, &s, 8);

  assert(g_resources_reply_delivered);
  assert(!cookie_jar_has_pending(&s.cookie_jar));
  assert(s.randr_pending_replies == 0);
  assert(s.randr_pending_capacity == 0);
  assert(s.randr_pending_monitors == NULL);

  printf("test_randr_crtc_enqueue_failure_clears_pending_accounting passed\n");
  cleanup_server(&s);
}

static void test_randr_crtc_zero_sequence_clears_pending_accounting(void) {
  server_t s;
  setup_server(&s);

  g_fail_crtc_enqueue = false;
  g_force_zero_crtc_sequence = true;
  g_resources_sequence = 0;
  g_resources_reply_delivered = false;
  stub_poll_for_reply_hook = randr_poll_for_reply;

  wm_update_monitors(&s);
  assert(g_resources_sequence != 0);
  assert(cookie_jar_has_pending(&s.cookie_jar));

  cookie_jar_mark_replies_may_exist(&s.cookie_jar);
  cookie_jar_drain(&s.cookie_jar, s.conn, &s, 8);
  g_force_zero_crtc_sequence = false;

  assert(g_resources_reply_delivered);
  assert(!cookie_jar_has_pending(&s.cookie_jar));
  assert(s.randr_pending_replies == 0);
  assert(s.randr_pending_capacity == 0);
  assert(s.randr_pending_monitors == NULL);

  printf("test_randr_crtc_zero_sequence_clears_pending_accounting passed\n");
  cleanup_server(&s);
}

int main(void) {
  test_randr_crtc_enqueue_failure_clears_pending_accounting();
  test_randr_crtc_zero_sequence_clears_pending_accounting();
  return 0;
}
