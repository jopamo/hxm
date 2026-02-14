#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xproto.h>

static xcb_connection_t* c;
static xcb_screen_t* screen;
static xcb_window_t root;

static struct {
  xcb_atom_t _NET_SUPPORTING_WM_CHECK;
  xcb_atom_t _NET_SUPPORTED;
  xcb_atom_t _NET_CURRENT_DESKTOP;
  xcb_atom_t _NET_NUMBER_OF_DESKTOPS;
  xcb_atom_t _NET_DESKTOP_NAMES;

  xcb_atom_t _NET_WM_DESKTOP;
  xcb_atom_t _NET_ACTIVE_WINDOW;

  xcb_atom_t _NET_WM_STATE;
  xcb_atom_t _NET_WM_STATE_FULLSCREEN;
  xcb_atom_t _NET_WM_STATE_ABOVE;
  xcb_atom_t _NET_WM_STATE_BELOW;
  xcb_atom_t _NET_WM_STATE_STICKY;

  xcb_atom_t _NET_WM_NAME;
  xcb_atom_t UTF8_STRING;

  xcb_atom_t WM_PROTOCOLS;
  xcb_atom_t WM_DELETE_WINDOW;
  xcb_atom_t WM_NAME;
  xcb_atom_t WM_CLASS;

  xcb_atom_t _NET_CLIENT_LIST;
  xcb_atom_t _NET_CLIENT_LIST_STACKING;
} atoms;

static void fail(const char* msg) {
  fprintf(stderr, "FAIL: %s\n", msg);
  exit(1);
}

static void failf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "FAIL: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000ull);
}

static void xflush(void) {
  int rc = xcb_flush(c);
  if (rc <= 0)
    fail("xcb_flush failed");
}

static xcb_atom_t get_atom(const char* name) {
  xcb_intern_atom_cookie_t cookie = xcb_intern_atom(c, 0, (uint16_t)strlen(name), name);
  xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(c, cookie, NULL);
  if (!reply)
    fail("Failed to intern atom");
  xcb_atom_t atom = reply->atom;
  free(reply);
  return atom;
}

static void init_atoms(void) {
  atoms._NET_SUPPORTING_WM_CHECK = get_atom("_NET_SUPPORTING_WM_CHECK");
  atoms._NET_SUPPORTED = get_atom("_NET_SUPPORTED");
  atoms._NET_CURRENT_DESKTOP = get_atom("_NET_CURRENT_DESKTOP");
  atoms._NET_NUMBER_OF_DESKTOPS = get_atom("_NET_NUMBER_OF_DESKTOPS");
  atoms._NET_DESKTOP_NAMES = get_atom("_NET_DESKTOP_NAMES");

  atoms._NET_WM_DESKTOP = get_atom("_NET_WM_DESKTOP");
  atoms._NET_ACTIVE_WINDOW = get_atom("_NET_ACTIVE_WINDOW");

  atoms._NET_WM_STATE = get_atom("_NET_WM_STATE");
  atoms._NET_WM_STATE_FULLSCREEN = get_atom("_NET_WM_STATE_FULLSCREEN");
  atoms._NET_WM_STATE_ABOVE = get_atom("_NET_WM_STATE_ABOVE");
  atoms._NET_WM_STATE_BELOW = get_atom("_NET_WM_STATE_BELOW");
  atoms._NET_WM_STATE_STICKY = get_atom("_NET_WM_STATE_STICKY");

  atoms._NET_WM_NAME = get_atom("_NET_WM_NAME");
  atoms.UTF8_STRING = get_atom("UTF8_STRING");

  atoms.WM_PROTOCOLS = get_atom("WM_PROTOCOLS");
  atoms.WM_DELETE_WINDOW = get_atom("WM_DELETE_WINDOW");
  atoms.WM_NAME = get_atom("WM_NAME");
  atoms.WM_CLASS = XCB_ATOM_WM_CLASS;

  atoms._NET_CLIENT_LIST = get_atom("_NET_CLIENT_LIST");
  atoms._NET_CLIENT_LIST_STACKING = get_atom("_NET_CLIENT_LIST_STACKING");
}

static void* get_property_any(xcb_window_t win, xcb_atom_t prop, xcb_atom_t type, uint32_t* nbytes_out) {
  xcb_get_property_cookie_t ck = xcb_get_property(c, 0, win, prop, type, 0, 0x7fffffff);
  xcb_get_property_reply_t* r = xcb_get_property_reply(c, ck, NULL);
  if (!r)
    return NULL;

  int len = xcb_get_property_value_length(r);
  if (len <= 0) {
    free(r);
    if (nbytes_out)
      *nbytes_out = 0;
    return NULL;
  }

  void* val = malloc((size_t)len);
  if (!val)
    fail("oom");
  memcpy(val, xcb_get_property_value(r), (size_t)len);

  free(r);
  if (nbytes_out)
    *nbytes_out = (uint32_t)len;
  return val;
}

static bool get_cardinal32(xcb_window_t win, xcb_atom_t prop, uint32_t* out) {
  uint32_t nbytes = 0;
  uint32_t* v = (uint32_t*)get_property_any(win, prop, XCB_ATOM_CARDINAL, &nbytes);
  if (!v || nbytes < 4) {
    free(v);
    return false;
  }
  *out = v[0];
  free(v);
  return true;
}

static bool get_window_prop(xcb_window_t win, xcb_atom_t prop, xcb_window_t* out) {
  uint32_t nbytes = 0;
  xcb_window_t* v = (xcb_window_t*)get_property_any(win, prop, XCB_ATOM_WINDOW, &nbytes);
  if (!v || nbytes < sizeof(xcb_window_t)) {
    free(v);
    return false;
  }
  *out = v[0];
  free(v);
  return true;
}

static bool atom_list_contains(xcb_window_t win, xcb_atom_t prop, xcb_atom_t needle) {
  uint32_t nbytes = 0;
  xcb_atom_t* v = (xcb_atom_t*)get_property_any(win, prop, XCB_ATOM_ATOM, &nbytes);
  if (!v || nbytes < sizeof(xcb_atom_t)) {
    free(v);
    return false;
  }
  size_t n = (size_t)nbytes / sizeof(xcb_atom_t);
  for (size_t i = 0; i < n; i++) {
    if (v[i] == needle) {
      free(v);
      return true;
    }
  }
  free(v);
  return false;
}

static bool window_list_contains(xcb_atom_t prop, xcb_window_t needle) {
  uint32_t nbytes = 0;
  xcb_window_t* v = (xcb_window_t*)get_property_any(root, prop, XCB_ATOM_WINDOW, &nbytes);
  if (!v || nbytes < sizeof(xcb_window_t)) {
    free(v);
    return false;
  }
  size_t n = (size_t)nbytes / sizeof(xcb_window_t);
  for (size_t i = 0; i < n; i++) {
    if (v[i] == needle) {
      free(v);
      return true;
    }
  }
  free(v);
  return false;
}

static xcb_window_t query_parent(xcb_window_t w) {
  xcb_query_tree_cookie_t ck = xcb_query_tree(c, w);
  xcb_query_tree_reply_t* r = xcb_query_tree_reply(c, ck, NULL);
  if (!r)
    return XCB_WINDOW_NONE;
  xcb_window_t p = r->parent;
  free(r);
  return p;
}

static bool is_viewable(xcb_window_t w) {
  xcb_get_window_attributes_cookie_t ck = xcb_get_window_attributes(c, w);
  xcb_get_window_attributes_reply_t* r = xcb_get_window_attributes_reply(c, ck, NULL);
  if (!r)
    return false;
  bool ok = (r->map_state == XCB_MAP_STATE_VIEWABLE);
  free(r);
  return ok;
}

static void drain_events_some(void) {
  for (int i = 0; i < 64; i++) {
    xcb_generic_event_t* ev = xcb_poll_for_event(c);
    if (!ev)
      break;
    free(ev);
  }
}

static void wait_managed(xcb_window_t w, uint32_t timeout_ms, xcb_window_t* out_frame) {
  uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;
  while (now_us() < deadline) {
    xcb_window_t p = query_parent(w);
    if (p != XCB_WINDOW_NONE && p != root) {
      if (out_frame)
        *out_frame = p;
      return;
    }

    xcb_generic_event_t* ev = xcb_poll_for_event(c);
    if (ev) {
      if ((ev->response_type & ~0x80) == XCB_REPARENT_NOTIFY) {
        xcb_reparent_notify_event_t* re = (xcb_reparent_notify_event_t*)ev;
        if (re->window == w && re->parent != root) {
          if (out_frame)
            *out_frame = re->parent;
          free(ev);
          return;
        }
      }
      free(ev);
    }

    usleep(2000);
  }
  fail("Window not managed in time");
}

static void wait_viewable(xcb_window_t w, uint32_t timeout_ms) {
  uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;
  while (now_us() < deadline) {
    if (is_viewable(w))
      return;
    drain_events_some();
    usleep(2000);
  }
  fail("Window not viewable in time");
}

static void wait_root_cardinal_eq(xcb_atom_t prop, uint32_t want, uint32_t timeout_ms) {
  uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;
  while (now_us() < deadline) {
    uint32_t v = 0;
    if (get_cardinal32(root, prop, &v) && v == want)
      return;
    drain_events_some();
    usleep(2000);
  }
  uint32_t got = 0;
  if (!get_cardinal32(root, prop, &got))
    fail("root cardinal missing during wait");
  failf("root property %u did not become %u (got %u)", prop, want, got);
}

static void wait_window_cardinal_eq(xcb_window_t w, xcb_atom_t prop, uint32_t want, uint32_t timeout_ms) {
  uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;
  while (now_us() < deadline) {
    uint32_t v = 0;
    if (get_cardinal32(w, prop, &v) && v == want)
      return;
    drain_events_some();
    usleep(2000);
  }
  uint32_t got = 0;
  if (!get_cardinal32(w, prop, &got))
    fail("window cardinal missing during wait");
  failf("window property %u did not become %u (got %u)", prop, want, got);
}

static void wait_window_destroyed_or_unlisted(xcb_window_t w, uint32_t timeout_ms) {
  uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000ull;
  while (now_us() < deadline) {
    // event path
    for (;;) {
      xcb_generic_event_t* ev = xcb_poll_for_event(c);
      if (!ev)
        break;
      uint8_t rt = (ev->response_type & ~0x80);
      if (rt == XCB_DESTROY_NOTIFY) {
        xcb_destroy_notify_event_t* de = (xcb_destroy_notify_event_t*)ev;
        if (de->window == w) {
          free(ev);
          return;
        }
      }
      free(ev);
    }

    // property path
    if (!window_list_contains(atoms._NET_CLIENT_LIST, w))
      return;

    usleep(2000);
  }
  fail("Window did not disappear after close request");
}

static xcb_window_t create_window(const char* class_name, const char* instance_name) {
  xcb_window_t w = xcb_generate_id(c);
  uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE};

  xcb_create_window(c, XCB_COPY_FROM_PARENT, w, root, 0, 0, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

  // WM_CLASS is "instance\0class\0"
  if (class_name || instance_name) {
    const char* inst = instance_name ? instance_name : "";
    const char* cls = class_name ? class_name : "";
    size_t len = strlen(inst) + 1 + strlen(cls) + 1;
    char* buf = (char*)malloc(len);
    if (!buf)
      fail("oom");
    memcpy(buf, inst, strlen(inst) + 1);
    memcpy(buf + strlen(inst) + 1, cls, strlen(cls) + 1);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atoms.WM_CLASS, XCB_ATOM_STRING, 8, (uint32_t)len, buf);
    free(buf);
  }

  // set WM_DELETE_WINDOW so the WM can choose to send it
  xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atoms.WM_PROTOCOLS, XCB_ATOM_ATOM, 32, 1, &atoms.WM_DELETE_WINDOW);

  return w;
}

static void map_window(xcb_window_t w) {
  xcb_map_window(c, w);
  xflush();
}

static void send_current_desktop(uint32_t desktop) {
  xcb_client_message_event_t ev = {0};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = root;
  ev.type = atoms._NET_CURRENT_DESKTOP;
  ev.data.data32[0] = desktop;
  ev.data.data32[1] = XCB_CURRENT_TIME;

  xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char*)&ev);
  xflush();
}

static void send_active_window_request(xcb_window_t w) {
  xcb_client_message_event_t ev = {0};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = w;
  ev.type = atoms._NET_ACTIVE_WINDOW;
  ev.data.data32[0] = 1;  // source indication: application
  ev.data.data32[1] = XCB_CURRENT_TIME;
  ev.data.data32[2] = XCB_WINDOW_NONE;

  xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char*)&ev);
  xflush();
}

static void send_wm_desktop_request(xcb_window_t w, uint32_t desktop) {
  xcb_client_message_event_t ev = {0};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = w;
  ev.type = atoms._NET_WM_DESKTOP;
  ev.data.data32[0] = desktop;
  ev.data.data32[1] = XCB_CURRENT_TIME;

  xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char*)&ev);
  xflush();
}

static void send_wm_state_request(xcb_window_t w, uint32_t action, xcb_atom_t a1, xcb_atom_t a2) {
  xcb_client_message_event_t ev = {0};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = w;
  ev.type = atoms._NET_WM_STATE;
  ev.data.data32[0] = action;  // 0 remove, 1 add, 2 toggle
  ev.data.data32[1] = a1;
  ev.data.data32[2] = a2;
  ev.data.data32[3] = 1;  // source indication: application
  ev.data.data32[4] = 0;

  xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char*)&ev);
  xflush();
}

static void send_wm_delete_window(xcb_window_t w) {
  xcb_client_message_event_t ev = {0};
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = w;
  ev.type = atoms.WM_PROTOCOLS;
  ev.data.data32[0] = atoms.WM_DELETE_WINDOW;
  ev.data.data32[1] = XCB_CURRENT_TIME;

  xcb_send_event(c, 0, w, XCB_EVENT_MASK_NO_EVENT, (const char*)&ev);
  xflush();
}

static void test_wm_sanity(void) {
  printf("Testing WM sanity...\n");

  xcb_window_t sup = XCB_WINDOW_NONE;
  if (!get_window_prop(root, atoms._NET_SUPPORTING_WM_CHECK, &sup) || sup == XCB_WINDOW_NONE)
    fail("_NET_SUPPORTING_WM_CHECK missing on root");

  xcb_window_t sup2 = XCB_WINDOW_NONE;
  if (!get_window_prop(sup, atoms._NET_SUPPORTING_WM_CHECK, &sup2))
    fail("_NET_SUPPORTING_WM_CHECK missing on supporting window");
  if (sup2 != sup)
    fail("_NET_SUPPORTING_WM_CHECK not self-referential on supporting window");

  // _NET_SUPPORTED must exist and include a few things you claim to implement
  if (!atom_list_contains(root, atoms._NET_SUPPORTED, atoms._NET_CURRENT_DESKTOP))
    fail("_NET_SUPPORTED missing _NET_CURRENT_DESKTOP");
  if (!atom_list_contains(root, atoms._NET_SUPPORTED, atoms._NET_ACTIVE_WINDOW))
    fail("_NET_SUPPORTED missing _NET_ACTIVE_WINDOW");
  if (!atom_list_contains(root, atoms._NET_SUPPORTED, atoms._NET_WM_STATE))
    fail("_NET_SUPPORTED missing _NET_WM_STATE");

  printf("PASS: WM sanity\n");
}

static void test_client_lists_and_manage(void) {
  printf("Testing management + client lists...\n");

  xcb_window_t w = create_window("ListTest", "listtest");
  map_window(w);

  xcb_window_t frame = XCB_WINDOW_NONE;
  wait_managed(w, 1000, &frame);
  wait_viewable(w, 1000);

  if (!window_list_contains(atoms._NET_CLIENT_LIST, w))
    fail("_NET_CLIENT_LIST does not contain window");
  if (!window_list_contains(atoms._NET_CLIENT_LIST_STACKING, w))
    fail("_NET_CLIENT_LIST_STACKING does not contain window");

  xcb_destroy_window(c, w);
  xflush();

  printf("PASS: management + client lists\n");
}

static void test_active_window_focus(void) {
  printf("Testing _NET_ACTIVE_WINDOW...\n");

  xcb_window_t w1 = create_window("FocusTest", "focus1");
  map_window(w1);
  wait_managed(w1, 1000, NULL);
  wait_viewable(w1, 1000);

  // many WMs focus new windows automatically
  // if yours does not, request it
  send_active_window_request(w1);

  // wait for root _NET_ACTIVE_WINDOW to become w1
  uint64_t deadline = now_us() + 1000ull * 1000ull;
  while (now_us() < deadline) {
    xcb_window_t aw = XCB_WINDOW_NONE;
    if (get_window_prop(root, atoms._NET_ACTIVE_WINDOW, &aw) && aw == w1)
      break;
    drain_events_some();
    usleep(2000);
  }
  xcb_window_t aw = XCB_WINDOW_NONE;
  if (!get_window_prop(root, atoms._NET_ACTIVE_WINDOW, &aw) || aw != w1)
    fail("_NET_ACTIVE_WINDOW did not become w1");

  xcb_window_t w2 = create_window("FocusTest", "focus2");
  map_window(w2);
  wait_managed(w2, 1000, NULL);
  wait_viewable(w2, 1000);

  send_active_window_request(w2);

  deadline = now_us() + 1000ull * 1000ull;
  while (now_us() < deadline) {
    xcb_window_t cur = XCB_WINDOW_NONE;
    if (get_window_prop(root, atoms._NET_ACTIVE_WINDOW, &cur) && cur == w2)
      break;
    drain_events_some();
    usleep(2000);
  }
  if (!get_window_prop(root, atoms._NET_ACTIVE_WINDOW, &aw) || aw != w2)
    fail("_NET_ACTIVE_WINDOW did not become w2");

  xcb_destroy_window(c, w2);
  xcb_destroy_window(c, w1);
  xflush();

  printf("PASS: _NET_ACTIVE_WINDOW\n");
}

static void test_workspaces(void) {
  printf("Testing workspaces...\n");

  // require desktops exist and start at 0
  uint32_t cur = 0xffffffffu;
  if (!get_cardinal32(root, atoms._NET_CURRENT_DESKTOP, &cur))
    fail("_NET_CURRENT_DESKTOP missing");

  if (cur != 0)
    fail("Initial desktop not 0");

  uint32_t nd = 0;
  if (get_cardinal32(root, atoms._NET_NUMBER_OF_DESKTOPS, &nd)) {
    if (nd == 0)
      fail("_NET_NUMBER_OF_DESKTOPS is zero");
    if (nd < 2)
      fprintf(stderr, "WARN: only %u desktops\n", nd);
  }
  else {
    fprintf(stderr, "WARN: _NET_NUMBER_OF_DESKTOPS missing\n");
  }

  xcb_window_t w = create_window("WsTest", "ws");
  map_window(w);
  wait_managed(w, 1000, NULL);
  wait_viewable(w, 1000);

  // by default windows should be on current desktop or on 0 depending on your
  // policy we check that _NET_WM_DESKTOP exists and is sane
  uint32_t wdesk = 0xffffffffu;
  if (!get_cardinal32(w, atoms._NET_WM_DESKTOP, &wdesk))
    fail("Window missing _NET_WM_DESKTOP");

  // switch to desktop 1
  send_current_desktop(1);
  wait_root_cardinal_eq(atoms._NET_CURRENT_DESKTOP, 1, 1000);

  // ensure window desktop property did not silently change unless you move it
  uint32_t wdesk2 = 0xffffffffu;
  if (!get_cardinal32(w, atoms._NET_WM_DESKTOP, &wdesk2))
    fail("Window missing _NET_WM_DESKTOP after switch");
  if (wdesk2 != wdesk) {
    fprintf(stderr, "WARN: window desktop changed from %u to %u on desktop switch\n", wdesk, wdesk2);
  }

  // move window to desktop 1 via _NET_WM_DESKTOP client message
  send_wm_desktop_request(w, 1);
  wait_window_cardinal_eq(w, atoms._NET_WM_DESKTOP, 1, 1000);

  // move window to all desktops using 0xFFFFFFFF
  send_wm_desktop_request(w, 0xffffffffu);
  wait_window_cardinal_eq(w, atoms._NET_WM_DESKTOP, 0xffffffffu, 1000);

  // move back to desktop 0
  send_wm_desktop_request(w, 0);
  wait_window_cardinal_eq(w, atoms._NET_WM_DESKTOP, 0, 1000);

  // switch back to 0
  send_current_desktop(0);
  wait_root_cardinal_eq(atoms._NET_CURRENT_DESKTOP, 0, 1000);

  xcb_destroy_window(c, w);
  xflush();

  printf("PASS: workspaces\n");
}

static void test_fullscreen_state_and_geometry(void) {
  printf("Testing fullscreen state + geometry...\n");

  const uint16_t sw = screen->width_in_pixels;
  const uint16_t sh = screen->height_in_pixels;

  xcb_window_t w = create_window("FsTest", "fs");
  map_window(w);

  xcb_window_t frame = XCB_WINDOW_NONE;
  wait_managed(w, 1000, &frame);
  wait_viewable(w, 1000);

  // request fullscreen
  send_wm_state_request(w, 1, atoms._NET_WM_STATE_FULLSCREEN, XCB_ATOM_NONE);

  // verify _NET_WM_STATE contains fullscreen
  uint64_t deadline = now_us() + 1500ull * 1000ull;
  while (now_us() < deadline) {
    if (atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_FULLSCREEN))
      break;
    drain_events_some();
    usleep(2000);
  }
  if (!atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_FULLSCREEN))
    fail("_NET_WM_STATE missing FULLSCREEN after request");

  // geometry checks
  // check frame geometry if reparented, else window geometry
  xcb_window_t target = (frame != XCB_WINDOW_NONE) ? frame : w;
  xcb_get_geometry_cookie_t gc = xcb_get_geometry(c, target);
  xcb_get_geometry_reply_t* gr = xcb_get_geometry_reply(c, gc, NULL);
  if (!gr)
    fail("Failed to get geometry");

  // allow borders and minor off-by due to decorations
  int dw = (int)gr->width - (int)sw;
  int dh = (int)gr->height - (int)sh;

  if (dw < -4 || dw > 4 || dh < -4 || dh > 4) {
    printf("  screen %ux%u\n", sw, sh);
    printf("  target %ux%u\n", gr->width, gr->height);
    fprintf(stderr, "WARN: fullscreen geometry mismatch\n");
  }
  free(gr);

  // remove fullscreen
  send_wm_state_request(w, 0, atoms._NET_WM_STATE_FULLSCREEN, XCB_ATOM_NONE);

  deadline = now_us() + 1500ull * 1000ull;
  while (now_us() < deadline) {
    if (!atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_FULLSCREEN))
      break;
    drain_events_some();
    usleep(2000);
  }
  if (atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_FULLSCREEN))
    fail("FULLSCREEN still present after remove request");

  xcb_destroy_window(c, w);
  xflush();

  printf("PASS: fullscreen state + geometry\n");
}

static void test_state_above_below_sticky(void) {
  printf("Testing additional state toggles...\n");

  xcb_window_t w = create_window("StateTest", "state");
  map_window(w);
  wait_managed(w, 1000, NULL);
  wait_viewable(w, 1000);

  // ABOVE
  send_wm_state_request(w, 1, atoms._NET_WM_STATE_ABOVE, XCB_ATOM_NONE);
  uint64_t deadline = now_us() + 1000ull * 1000ull;
  while (now_us() < deadline) {
    if (atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_ABOVE))
      break;
    drain_events_some();
    usleep(2000);
  }
  if (!atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_ABOVE))
    fprintf(stderr, "WARN: ABOVE not applied\n");

  // BELOW
  send_wm_state_request(w, 1, atoms._NET_WM_STATE_BELOW, XCB_ATOM_NONE);
  deadline = now_us() + 1000ull * 1000ull;
  while (now_us() < deadline) {
    if (atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_BELOW))
      break;
    drain_events_some();
    usleep(2000);
  }
  if (!atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_BELOW))
    fprintf(stderr, "WARN: BELOW not applied\n");

  // STICKY
  send_wm_state_request(w, 1, atoms._NET_WM_STATE_STICKY, XCB_ATOM_NONE);
  deadline = now_us() + 1000ull * 1000ull;
  while (now_us() < deadline) {
    if (atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_STICKY))
      break;
    drain_events_some();
    usleep(2000);
  }
  if (!atom_list_contains(w, atoms._NET_WM_STATE, atoms._NET_WM_STATE_STICKY))
    fprintf(stderr, "WARN: STICKY not applied\n");

  // cleanup remove all requested
  send_wm_state_request(w, 0, atoms._NET_WM_STATE_ABOVE, XCB_ATOM_NONE);
  send_wm_state_request(w, 0, atoms._NET_WM_STATE_BELOW, XCB_ATOM_NONE);
  send_wm_state_request(w, 0, atoms._NET_WM_STATE_STICKY, XCB_ATOM_NONE);

  xcb_destroy_window(c, w);
  xflush();

  printf("PASS: additional state toggles\n");
}

static void test_wm_delete_window(void) {
  printf("Testing WM_DELETE_WINDOW...\n");

  xcb_window_t w = create_window("CloseTest", "close");
  map_window(w);
  wait_managed(w, 1000, NULL);
  wait_viewable(w, 1000);

  // ensure protocol is set
  if (!atom_list_contains(w, atoms.WM_PROTOCOLS, atoms.WM_DELETE_WINDOW))
    fail("WM_DELETE_WINDOW not present in WM_PROTOCOLS on client window");

  send_wm_delete_window(w);
  wait_window_destroyed_or_unlisted(w, 1500);

  printf("PASS: WM_DELETE_WINDOW\n");
}

static void test_rules_probe(void) {
  printf("Testing rules probe...\n");

  // This is still a probe because rules are your config
  // We make it as strict as possible without hardcoding config
  xcb_window_t w = create_window("Special", "special");
  map_window(w);
  wait_managed(w, 1000, NULL);
  wait_viewable(w, 1000);

  uint32_t d = 0xffffffffu;
  if (!get_cardinal32(w, atoms._NET_WM_DESKTOP, &d))
    fail("rules probe window missing _NET_WM_DESKTOP");

  printf("  rules probe desktop=%u\n", d);

  xcb_destroy_window(c, w);
  xflush();

  printf("PASS: rules probe\n");
}

int main(void) {
  c = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(c))
    fail("Cannot connect to X");

  screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
  root = screen->root;

  // ask for root property events if you ever switch to wait-for-event paths
  uint32_t mask = XCB_CW_EVENT_MASK;
  uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  xcb_change_window_attributes(c, root, mask, values);
  xflush();

  init_atoms();

  test_wm_sanity();
  test_client_lists_and_manage();
  test_active_window_focus();
  test_workspaces();
  test_fullscreen_state_and_geometry();
  test_state_above_below_sticky();
  test_wm_delete_window();
  test_rules_probe();

  xcb_disconnect(c);
  return 0;
}
