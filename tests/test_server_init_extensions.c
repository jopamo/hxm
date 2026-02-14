#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

#include "event.h"
#include "wm.h"

extern void xcb_stubs_reset(void);

// We need to link with xcb_stubs.c but override some functions.
// Since xcb_stubs.c defines them as strong symbols, we must use --wrap in
// linker.

// Flags for controlling mocks
static bool fail_damage_reply = false;
static bool fail_randr_reply = false;
static xcb_window_t restore_active_window = XCB_NONE;

// xcb_stubs.c defines xcb_get_extension_data. We wrap it to ensure present=1.
const xcb_query_extension_reply_t* __real_xcb_get_extension_data(xcb_connection_t* c, xcb_extension_t* ext);
const xcb_query_extension_reply_t* __wrap_xcb_get_extension_data(xcb_connection_t* c, xcb_extension_t* ext) {
  // Call real stub first to init
  const xcb_query_extension_reply_t* r = __real_xcb_get_extension_data(c, ext);
  // Cast away const to force present=1 for our tests
  ((xcb_query_extension_reply_t*)r)->present = 1;
  return r;
}

// Damage query version reply wrapper
xcb_damage_query_version_reply_t* __wrap_xcb_damage_query_version_reply(xcb_connection_t* c, xcb_damage_query_version_cookie_t cookie, xcb_generic_error_t** e) {
  (void)c;
  (void)cookie;
  (void)e;
  if (fail_damage_reply)
    return NULL;

  xcb_damage_query_version_reply_t* r = calloc(1, sizeof(*r));
  r->major_version = 1;
  r->minor_version = 1;
  return r;
}

// RandR query version reply wrapper
xcb_randr_query_version_reply_t* __wrap_xcb_randr_query_version_reply(xcb_connection_t* c, xcb_randr_query_version_cookie_t cookie, xcb_generic_error_t** e) {
  (void)c;
  (void)cookie;
  (void)e;
  if (fail_randr_reply)
    return NULL;

  xcb_randr_query_version_reply_t* r = calloc(1, sizeof(*r));
  r->major_version = 1;
  r->minor_version = 5;
  return r;
}

// Get property reply wrapper to hook _NET_ACTIVE_WINDOW
xcb_get_property_reply_t* __real_xcb_get_property_reply(xcb_connection_t* c, xcb_get_property_cookie_t cookie, xcb_generic_error_t** e);

xcb_get_property_reply_t* __wrap_xcb_get_property_reply(xcb_connection_t* c, xcb_get_property_cookie_t cookie, xcb_generic_error_t** e) {
  if (restore_active_window != XCB_NONE) {
    // We can't easily know WHICH property request this is without tracking
    // cookies. But for this test, we can cheat: if we set the flag, return the
    // active window reply once. Or better: xcb_stubs has a hook! But we are
    // wrapping it. Let's rely on the fact that server_init calls
    // _NET_ACTIVE_WINDOW after _NET_CURRENT_DESKTOP. This is brittle. A better
    // way: check if we are in server_init? No.

    // Let's construct a fake reply that looks like an ATOM_WINDOW.
    // The real stub returns NONE/0.
    // If we want to simulate restore, we need to return a window.
    // But server_init calls get_property twice: for DESKTOP and ACTIVE_WINDOW.
    // Let's just return what we want for ALL calls if set, assuming others
    // handle garbage gracefully? No, invalid property type might break things.

    // Let's implement a simple counter or just assume we can return a generic
    // "valid" reply and if it matches the expected type in the caller, it
    // works. server_init checks: if (r->type == XCB_ATOM_WINDOW && r->format ==
    // 32 && len >= 4)

    xcb_get_property_reply_t* r = calloc(1, sizeof(*r) + 4);
    r->type = XCB_ATOM_WINDOW;
    r->format = 32;
    r->value_len = 1;  // 1 item of 32 bits
    memcpy(xcb_get_property_value(r), &restore_active_window, 4);

    // Reset so we don't spam it (though server_init calls generic helper)
    // If we clear it, subsequent calls get real stub.
    // BUT server_init calls current_desktop first.
    // We need a way to distinguish.
    // Actually, let's just make ALL property replies return this window if set.
    // The desktop check expects XCB_ATOM_CARDINAL. If we return WINDOW, it will
    // ignore it. Perfect.
    return r;
  }
  return __real_xcb_get_property_reply(c, cookie, e);
}

// Stub for damage query version (needed because xcb_stubs.c doesn't have it)
xcb_damage_query_version_cookie_t xcb_damage_query_version(xcb_connection_t* c, uint32_t major, uint32_t minor) {
  (void)c;
  (void)major;
  (void)minor;
  return (xcb_damage_query_version_cookie_t){0};
}

// We need a way to check results. server_t is modified in place.

void reset_mocks(void) {
  fail_damage_reply = false;
  fail_randr_reply = false;
  restore_active_window = XCB_NONE;
  xcb_stubs_reset();
}

void damage_fail_test(void) {
  printf("Running damage_fail_test... ");
  reset_mocks();
  fail_damage_reply = true;

  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  server_init(&s);

  if (s.damage_supported) {
    fprintf(stderr, "FAILED: damage_supported is true\n");
    exit(1);
  }
  server_cleanup(&s);
  printf("PASSED\n");
}

void randr_fail_test(void) {
  printf("Running randr_fail_test... ");
  reset_mocks();
  fail_randr_reply = true;

  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  server_init(&s);

  if (s.randr_supported) {
    fprintf(stderr, "FAILED: randr_supported is true\n");
    exit(1);
  }
  server_cleanup(&s);
  printf("PASSED\n");
}

void restore_active_test(void) {
  printf("Running restore_active_test... ");
  reset_mocks();
  restore_active_window = 0x1234;

  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;
  server_init(&s);

  if (s.initial_focus != 0x1234) {
    fprintf(stderr, "FAILED: initial_focus=%u, expected 0x1234\n", s.initial_focus);
    exit(1);
  }
  server_cleanup(&s);
  printf("PASSED\n");
}

int main(void) {
  damage_fail_test();
  randr_fail_test();
  restore_active_test();
  return 0;
}
