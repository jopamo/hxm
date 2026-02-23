/* src/wm.c
 * Window manager core logic
 *
 * This module implements the core "business logic" of the window manager:
 * - Seizing control of the X display (wm_become)
 * - Handling core X events (MapRequest, ConfigureRequest)
 * - Implementing EWMH/ICCCM protocols
 * - Managing window placement, maximize, and minimize logic
 */

#include "wm.h"

#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>

#include "client.h"
#include "event.h"
#include "frame.h"
#include "hxm.h"
#include "snap.h"
#include "wm_internal.h"

// Small helpers

enum {
  NET_WM_MOVERESIZE_SIZE_TOPLEFT = 0,
  NET_WM_MOVERESIZE_SIZE_TOP = 1,
  NET_WM_MOVERESIZE_SIZE_TOPRIGHT = 2,
  NET_WM_MOVERESIZE_SIZE_RIGHT = 3,
  NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT = 4,
  NET_WM_MOVERESIZE_SIZE_BOTTOM = 5,
  NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT = 6,
  NET_WM_MOVERESIZE_SIZE_LEFT = 7,
  NET_WM_MOVERESIZE_MOVE = 8,
  NET_WM_MOVERESIZE_SIZE_KEYBOARD = 9,
  NET_WM_MOVERESIZE_MOVE_KEYBOARD = 10,
  NET_WM_MOVERESIZE_CANCEL = 11,
};

static void ignore_one_unmap(client_hot_t* hot) {
  if (!hot)
    return;
  if (hot->ignore_unmap == 0)
    return;
  hot->ignore_unmap--;
}

static void add_ignore_unmaps(client_hot_t* hot, uint8_t n) {
  if (!hot)
    return;
  uint16_t sum = (uint16_t)hot->ignore_unmap + (uint16_t)n;
  hot->ignore_unmap = (sum > UINT8_MAX) ? UINT8_MAX : (uint8_t)sum;
}

static bool check_wm_s0_available(server_t* s) {
  xcb_get_selection_owner_cookie_t cookie = xcb_get_selection_owner(s->conn, atoms.WM_S0);
  xcb_get_selection_owner_reply_t* reply = xcb_get_selection_owner_reply(s->conn, cookie, NULL);
  if (!reply)
    return false;
  bool available = (reply->owner == XCB_NONE);
  free(reply);
  return available;
}

static void set_root_cursor(server_t* s, uint16_t cursor_font_id) {
  xcb_font_t font = xcb_generate_id(s->conn);
  xcb_open_font(s->conn, font, (uint16_t)strlen("cursor"), "cursor");
  xcb_cursor_t cursor = xcb_generate_id(s->conn);
  xcb_create_glyph_cursor(s->conn, cursor, font, font, cursor_font_id, cursor_font_id + 1, 0, 0, 0, 0xffff, 0xffff, 0xffff);
  uint32_t mask = XCB_CW_CURSOR;
  uint32_t values[] = {cursor};
  xcb_change_window_attributes(s->conn, s->root, mask, values);
  xcb_free_cursor(s->conn, cursor);
  xcb_close_font(s->conn, font);
}

static bool should_raise_on_click(client_hot_t* c, xcb_button_t button) {
  (void)c;
  return (button == 1 || button == 3);
}

static bool wm_is_above_in_layer(const server_t* s, const client_hot_t* a, const client_hot_t* b) {
  if (!a || !b)
    return false;
  if (a->layer != b->layer)
    return false;

  const small_vec_t* v = &s->layers[a->layer];
  bool seen_b = false;
  for (size_t i = 0; i < v->length; i++) {
    handle_t h = (handle_t)(uintptr_t)v->items[i];
    const client_hot_t* cur = server_chot((server_t*)s, h);
    if (!cur)
      continue;
    if (cur == a)
      return seen_b;
    if (cur == b)
      seen_b = true;
  }
  return false;
}

void wm_set_frame_extents_for_window(server_t* s, xcb_window_t win, bool undecorated) {
  uint32_t bw = undecorated ? 0 : s->config.theme.border_width;
  uint32_t th = undecorated ? 0 : s->config.theme.title_height;
  uint32_t bottom_h = bw;
  uint32_t extents[4] = {bw, bw, th + bw, bottom_h};
  xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, win, atoms._NET_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 32, 4, extents);
}

void wm_update_monitors(server_t* s) {
  if (!s->conn)
    return;

  uint32_t active_count = 0;
  monitor_t* next_monitors = NULL;

  if (!s->randr_supported) {
    active_count = 1;
    next_monitors = calloc(1, sizeof(monitor_t));
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
    next_monitors[0].geom.x = 0;
    next_monitors[0].geom.y = 0;
    next_monitors[0].geom.w = (uint16_t)screen->width_in_pixels;
    next_monitors[0].geom.h = (uint16_t)screen->height_in_pixels;
    next_monitors[0].workarea = next_monitors[0].geom;
  }
  else {
    xcb_randr_get_screen_resources_current_cookie_t r_c = xcb_randr_get_screen_resources_current(s->conn, s->root);
    xcb_randr_get_screen_resources_current_reply_t* res = xcb_randr_get_screen_resources_current_reply(s->conn, r_c, NULL);

    if (res) {
      xcb_randr_crtc_t* crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
      int num_crtcs = xcb_randr_get_screen_resources_current_crtcs_length(res);

      next_monitors = calloc((size_t)num_crtcs, sizeof(monitor_t));

      for (int i = 0; i < num_crtcs; i++) {
        xcb_randr_get_crtc_info_cookie_t c_c = xcb_randr_get_crtc_info(s->conn, crtcs[i], res->config_timestamp);
        xcb_randr_get_crtc_info_reply_t* crtc = xcb_randr_get_crtc_info_reply(s->conn, c_c, NULL);

        if (crtc) {
          if (crtc->mode != XCB_NONE) {
            next_monitors[active_count].geom.x = crtc->x;
            next_monitors[active_count].geom.y = crtc->y;
            next_monitors[active_count].geom.w = crtc->width;
            next_monitors[active_count].geom.h = crtc->height;
            next_monitors[active_count].workarea = next_monitors[active_count].geom;
            active_count++;
          }
          free(crtc);
        }
      }
      free(res);
    }
  }

  if (next_monitors) {
    if (s->monitors)
      free(s->monitors);
    s->monitors = next_monitors;
    s->monitor_count = active_count;
    LOG_INFO("Monitor update: %u monitors detected", s->monitor_count);
  }
}

void wm_get_monitor_geometry(server_t* s, client_hot_t* hot, rect_t* out_geom) {
  // Default to first monitor or whole screen
  if (s->monitor_count > 0) {
    *out_geom = s->monitors[0].geom;
  }
  else {
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
    out_geom->x = 0;
    out_geom->y = 0;
    out_geom->w = (uint16_t)screen->width_in_pixels;
    out_geom->h = (uint16_t)screen->height_in_pixels;
  }

  if (s->monitor_count <= 1)
    return;

  // Use center of the window to determine which monitor it is on
  int center_x = hot->server.x + hot->server.w / 2;
  int center_y = hot->server.y + hot->server.h / 2;

  for (uint32_t i = 0; i < s->monitor_count; i++) {
    monitor_t* m = &s->monitors[i];
    if (center_x >= m->geom.x && center_x < m->geom.x + (int)m->geom.w && center_y >= m->geom.y && center_y < m->geom.y + (int)m->geom.h) {
      *out_geom = m->geom;
      break;
    }
  }
}

static void wm_client_apply_maximize(server_t* s, client_hot_t* hot) {
  uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
  uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;

  if (hot->maximized_horz) {
    int32_t w = (int32_t)s->workarea.w - 2 * (int32_t)bw;
    hot->desired.x = s->workarea.x;
    hot->desired.w = (uint16_t)((w > 0) ? w : 0);
  }

  if (hot->maximized_vert) {
    int32_t h = (int32_t)s->workarea.h - (int32_t)th - (int32_t)bw;
    hot->desired.y = s->workarea.y;
    hot->desired.h = (uint16_t)((h > 0) ? h : 0);
  }
}

void wm_client_set_maximize(server_t* s, client_hot_t* hot, bool max_horz, bool max_vert) {
  if (!hot)
    return;
  if (hot->layer == LAYER_FULLSCREEN)
    return;

  bool had_any = hot->maximized_horz || hot->maximized_vert;
  bool want_any = max_horz || max_vert;

  if (want_any && !had_any) {
    hot->saved_maximize_geom = hot->server;
    hot->saved_maximize_valid = true;
  }

  hot->maximized_horz = max_horz;
  hot->maximized_vert = max_vert;

  if (!want_any) {
    if (hot->saved_maximize_valid) {
      hot->desired = hot->saved_maximize_geom;
      hot->saved_maximize_valid = false;
    }
    else if (hot->state != STATE_NEW) {
      hot->desired = hot->server;
    }
  }
  else {
    if (hot->state != STATE_NEW) {
      hot->desired = hot->server;
    }
    if (hot->saved_maximize_valid) {
      if (!max_horz) {
        hot->desired.x = hot->saved_maximize_geom.x;
        hot->desired.w = hot->saved_maximize_geom.w;
      }
      if (!max_vert) {
        hot->desired.y = hot->saved_maximize_geom.y;
        hot->desired.h = hot->saved_maximize_geom.h;
      }
    }
    wm_client_apply_maximize(s, hot);
  }

  hot->dirty |= DIRTY_GEOM | DIRTY_STATE;
}

/*
 * wm_become:
 * Try to become the Window Manager for the screen.
 *
 * 1. Acquire WM_S0 selection: This is the cooperative lock used by EWMH/ICCCM
 *    to ensure only one WM is running.
 * 2. Select SubstructureRedirect: This is the X protocol mechanism that
 * intercepts window map/configure requests, allowing us to control placement.
 *    If this fails (BadAccess), another WM is already running.
 */
void wm_become(server_t* s) {
  xcb_connection_t* conn = s->conn;
  xcb_window_t root = s->root;

  // Retry loop for WM_S0 acquisition (handle race during restart)
  for (int i = 0; i < 10; i++) {
    if (check_wm_s0_available(s))
      break;
    struct timespec ts = {0, 100000000};  // 100ms
    nanosleep(&ts, NULL);
  }

  if (!check_wm_s0_available(s)) {
    LOG_ERROR("Refusing to become WM: WM_S0 is already owned");
    return;
  }

  // Select SubstructureRedirect on root (WM_S0 ownership)
  uint32_t root_events = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS;

  xcb_void_cookie_t cwa = xcb_change_window_attributes_checked(conn, root, XCB_CW_EVENT_MASK, &root_events);
  xcb_generic_error_t* err = xcb_request_check(conn, cwa);
  if (err) {
    // If this hits, it's usually BadAccess (another WM)
    LOG_ERROR("Failed to select root events (likely another WM). error_code=%u", err->error_code);
    free(err);
    return;
  }

  // Set cursor (left_ptr)
  set_root_cursor(s, XC_left_ptr);

  // Set _NET_SUPPORTED with basic atoms
  xcb_atom_t supported_atoms[] = {
      atoms._NET_SUPPORTED,
      atoms._NET_SUPPORTING_WM_CHECK,
      atoms._NET_CLIENT_LIST,
      atoms._NET_CLIENT_LIST_STACKING,
      atoms._NET_ACTIVE_WINDOW,
      atoms._GTK_FRAME_EXTENTS,
      atoms._NET_WM_NAME,
      atoms._NET_WM_VISIBLE_NAME,
      atoms._NET_WM_ICON_NAME,
      atoms._NET_WM_VISIBLE_ICON_NAME,
      atoms._NET_WM_PING,
      atoms._NET_WM_STATE,
      atoms._NET_WM_STATE_FULLSCREEN,
      atoms._NET_WM_STATE_ABOVE,
      atoms._NET_WM_STATE_BELOW,
      atoms._NET_WM_STATE_STICKY,
      atoms._NET_WM_STATE_DEMANDS_ATTENTION,
      atoms._NET_WM_STATE_HIDDEN,
      atoms._NET_WM_STATE_MAXIMIZED_HORZ,
      atoms._NET_WM_STATE_MAXIMIZED_VERT,
      atoms._NET_WM_STATE_FOCUSED,
      atoms._NET_WM_STATE_SKIP_TASKBAR,
      atoms._NET_WM_STATE_SKIP_PAGER,
      atoms._NET_WM_WINDOW_TYPE,
      atoms._NET_WM_WINDOW_TYPE_DOCK,
      atoms._NET_WM_WINDOW_TYPE_DIALOG,
      atoms._NET_WM_WINDOW_TYPE_NOTIFICATION,
      atoms._NET_WM_WINDOW_TYPE_NORMAL,
      atoms._NET_WM_WINDOW_TYPE_DESKTOP,
      atoms._NET_WM_WINDOW_TYPE_SPLASH,
      atoms._NET_WM_WINDOW_TYPE_TOOLBAR,
      atoms._NET_WM_WINDOW_TYPE_UTILITY,
      atoms._NET_WM_WINDOW_TYPE_MENU,
      atoms._NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
      atoms._NET_WM_WINDOW_TYPE_POPUP_MENU,
      atoms._NET_WM_WINDOW_TYPE_TOOLTIP,
      atoms._NET_WM_WINDOW_TYPE_COMBO,
      atoms._NET_WM_WINDOW_TYPE_DND,
      atoms._NET_WM_STRUT,
      atoms._NET_WM_STRUT_PARTIAL,
      atoms._NET_NUMBER_OF_DESKTOPS,
      atoms._NET_CURRENT_DESKTOP,
      atoms._NET_VIRTUAL_ROOTS,
      atoms._NET_WM_DESKTOP,
      atoms._NET_WORKAREA,
      atoms._NET_DESKTOP_NAMES,
      atoms._NET_DESKTOP_VIEWPORT,
      atoms._NET_SHOWING_DESKTOP,
      atoms._NET_WM_ICON,
      atoms._NET_CLOSE_WINDOW,
      atoms._NET_WM_PID,
      atoms._NET_WM_USER_TIME,
      atoms._NET_WM_USER_TIME_WINDOW,
      atoms._NET_WM_SYNC_REQUEST,
      atoms._NET_WM_SYNC_REQUEST_COUNTER,
      atoms._NET_WM_ICON_GEOMETRY,
      atoms._NET_WM_WINDOW_OPACITY,
      atoms._NET_DESKTOP_GEOMETRY,
      atoms._NET_FRAME_EXTENTS,
      atoms._NET_REQUEST_FRAME_EXTENTS,
      atoms._NET_WM_ALLOWED_ACTIONS,
      atoms._NET_WM_ACTION_MOVE,
      atoms._NET_WM_ACTION_RESIZE,
      atoms._NET_WM_ACTION_MINIMIZE,
      atoms._NET_WM_ACTION_STICK,
      atoms._NET_WM_ACTION_MAXIMIZE_HORZ,
      atoms._NET_WM_ACTION_MAXIMIZE_VERT,
      atoms._NET_WM_ACTION_FULLSCREEN,
      atoms._NET_WM_ACTION_CHANGE_DESKTOP,
      atoms._NET_WM_ACTION_CLOSE,
      atoms._NET_WM_ACTION_ABOVE,
      atoms._NET_WM_ACTION_BELOW,
      atoms._NET_WM_MOVERESIZE,
      atoms._NET_MOVERESIZE_WINDOW,
      atoms._NET_RESTACK_WINDOW,
      atoms._NET_WM_FULLSCREEN_MONITORS,
  };

  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_SUPPORTED, XCB_ATOM_ATOM, 32, (uint32_t)(sizeof(supported_atoms) / sizeof(supported_atoms[0])), supported_atoms);

  // Create supporting WM check window
  s->supporting_wm_check = xcb_generate_id(conn);
  uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
  uint32_t values[] = {1, XCB_EVENT_MASK_PROPERTY_CHANGE};

  xcb_create_window(conn, XCB_COPY_FROM_PARENT, s->supporting_wm_check, root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, mask, values);

  // Acquire WM_S0 selection (EWMH)
  xcb_set_selection_owner(conn, s->supporting_wm_check, atoms.WM_S0, XCB_CURRENT_TIME);

  // Verify ownership (defensive)
  {
    xcb_get_selection_owner_cookie_t ck = xcb_get_selection_owner(conn, atoms.WM_S0);
    xcb_get_selection_owner_reply_t* rep = xcb_get_selection_owner_reply(conn, ck, NULL);
    if (!rep || rep->owner != s->supporting_wm_check) {
      LOG_ERROR("Failed to acquire WM_S0 selection");
      free(rep);
      return;
    }
    free(rep);
  }

  // Set _NET_SUPPORTING_WM_CHECK on root and on the window
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 32, 1, &s->supporting_wm_check);
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, s->supporting_wm_check, atoms._NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 32, 1, &s->supporting_wm_check);

  // Set _NET_WM_NAME on supporting window (and optionally root)
  const char* wm_name = "hxm";
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, s->supporting_wm_check, atoms._NET_WM_NAME, atoms.UTF8_STRING, 8, (uint32_t)strlen(wm_name), wm_name);
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_WM_NAME, atoms.UTF8_STRING, 8, (uint32_t)strlen(wm_name), wm_name);

  // Set _NET_WM_PID
  uint32_t pid = (uint32_t)getpid();
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, s->supporting_wm_check, atoms._NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);
  // Also set on root for completeness (some pagers check root)
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);

  // Map the window so it is "viewable" as required by some tools
  xcb_map_window(conn, s->supporting_wm_check);

  // _NET_DESKTOP_GEOMETRY
  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
  uint32_t geometry[] = {screen->width_in_pixels, screen->height_in_pixels};
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_DESKTOP_GEOMETRY, XCB_ATOM_CARDINAL, 32, 2, geometry);

  // Initialize workarea to full screen (no struts yet)
  s->workarea.x = 0;
  s->workarea.y = 0;
  s->workarea.w = (uint16_t)screen->width_in_pixels;
  s->workarea.h = (uint16_t)screen->height_in_pixels;

  wm_publish_desktop_props(s);

  rect_t wa;
  wm_compute_workarea(s, &wa);
  wm_publish_workarea(s, &wa);

  // Initialize root lists and focus to sane empty values
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_CLIENT_LIST, XCB_ATOM_WINDOW, 32, 0, NULL);
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_CLIENT_LIST_STACKING, XCB_ATOM_WINDOW, 32, 0, NULL);
  xcb_delete_property(conn, root, atoms._NET_ACTIVE_WINDOW);
  {
    uint32_t val = 0;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_SHOWING_DESKTOP, XCB_ATOM_CARDINAL, 32, 1, &val);
  }

  xcb_flush(conn);
  LOG_INFO("Became WM on root %u, supporting %u", root, s->supporting_wm_check);
}

void wm_release(server_t* s) {
  if (!s || !s->conn)
    return;
  if (s->supporting_wm_check == XCB_WINDOW_NONE)
    return;

  xcb_connection_t* conn = s->conn;
  xcb_window_t root = s->root;
  xcb_window_t support = s->supporting_wm_check;

  xcb_delete_property(conn, root, atoms._NET_SUPPORTING_WM_CHECK);
  xcb_delete_property(conn, support, atoms._NET_SUPPORTING_WM_CHECK);
  xcb_delete_property(conn, support, atoms._NET_WM_NAME);

  xcb_set_selection_owner(conn, XCB_NONE, atoms.WM_S0, XCB_CURRENT_TIME);
  xcb_destroy_window(conn, support);
  xcb_flush(conn);

  s->supporting_wm_check = XCB_WINDOW_NONE;
}

/*
 * wm_adopt_children:
 * Scan for existing windows to manage (e.g., when restarting the WM).
 *
 * Strategy: Async Pipelining
 * We query the tree once, then iterate children and issue async
 * GetWindowAttributes requests for each. We do NOT block on replies here. The
 * replies will be handled by the cookie jar in the main loop, triggering
 * `client_manage_start`.
 */
void wm_adopt_children(server_t* s) {
  LOG_INFO("Adopting existing windows...");
  xcb_query_tree_cookie_t cookie = xcb_query_tree(s->conn, s->root);
  xcb_query_tree_reply_t* reply = xcb_query_tree_reply(s->conn, cookie, NULL);
  if (!reply)
    return;

  xcb_window_t* children = xcb_query_tree_children(reply);
  int len = xcb_query_tree_children_length(reply);

  for (int i = 0; i < len; i++) {
    xcb_window_t win = children[i];
    if (win == s->supporting_wm_check)
      continue;

    // Defer decision: use async attributes check via cookie jar and adopt in
    // reply handler
    xcb_get_window_attributes_cookie_t ck = xcb_get_window_attributes(s->conn, win);
    cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_WINDOW_ATTRIBUTES, HANDLE_INVALID, (uint64_t)win, s->txn_id, wm_handle_reply);
  }

  free(reply);
}

void wm_handle_map_request(server_t* s, xcb_map_request_event_t* ev) {
  TRACE_LOG("map_request win=%u parent=%u", ev->window, ev->parent);

  handle_t h = server_get_client_by_window(s, ev->window);
  if (h != HANDLE_INVALID) {
    client_hot_t* hot = server_chot(s, h);
    if (hot && hot->state == STATE_UNMAPPED) {
      LOG_INFO("Restoring unmapped client %u from MapRequest", ev->window);
      LOG_DEBUG("wm_handle_map_request: Restoring client %lx", h);
      wm_client_restore(s, h);
    }
    else {
      LOG_DEBUG("Ignoring MapRequest for already managed client %u (state %d)", ev->window, hot ? hot->state : -1);
    }
    return;
  }

  // Check attributes asynchronously before managing
  xcb_get_window_attributes_cookie_t ck = xcb_get_window_attributes(s->conn, ev->window);
  cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_CHECK_MANAGE_MAP_REQUEST, HANDLE_INVALID, (uint64_t)ev->window, s->txn_id, wm_handle_reply);
}

void wm_handle_unmap_notify(server_t* s, xcb_unmap_notify_event_t* ev) {
  TRACE_LOG("unmap_notify win=%u event=%u from_configure=%u", ev->window, ev->event, ev->from_configure);
  handle_t h = server_get_client_by_window(s, ev->window);
  if (h == HANDLE_INVALID) {
    h = server_get_client_by_frame(s, ev->window);
  }
  if (h == HANDLE_INVALID)
    return;

  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  if (hot->ignore_unmap > 0 && (ev->window == hot->xid || ev->window == hot->frame)) {
    TRACE_LOG("unmap_notify ignore h=%lx win=%u count=%u", h, ev->window, hot->ignore_unmap);
    ignore_one_unmap(hot);
    return;
  }

  // Ignore unmaps of the frame itself (only care about client window unmaps)
  if (ev->window != hot->xid)
    return;

  // Process if reported on the window itself, its frame parent, or the root
  // Applications withdrawing will trigger this
  if (ev->event != hot->xid && ev->event != hot->frame && ev->event != s->root)
    return;

  TRACE_LOG("unmap_notify unmanage h=%lx xid=%u frame=%u", h, hot->xid, hot->frame);
  client_unmanage(s, h);
}

void wm_handle_destroy_notify(server_t* s, xcb_destroy_notify_event_t* ev) {
  TRACE_LOG("destroy_notify win=%u event=%u", ev->window, ev->event);

  // Clean up pending states for unmanaged windows
  small_vec_t* v = (small_vec_t*)hash_map_get(&s->pending_unmanaged_states, ev->window);
  if (v) {
    for (size_t i = 0; i < v->length; i++)
      free(v->items[i]);
    small_vec_destroy(v);
    free(v);
    hash_map_remove(&s->pending_unmanaged_states, ev->window);
  }

  handle_t h = server_get_client_by_window(s, ev->window);
  if (h == HANDLE_INVALID) {
    h = server_get_client_by_frame(s, ev->window);
  }

  if (h == HANDLE_INVALID)
    return;

  client_hot_t* hot = server_chot(s, h);
  if (hot) {
    // If the destroyed window is the frame, don't unmanage the client if it's
    // still managed
    if (ev->window == hot->frame) {
      if (hot->state == STATE_MAPPED) {
        LOG_WARN("Frame %u destroyed for managed client %lx. Marking frame dead.", ev->window, h);
        if (s->interaction_handle == h) {
          wm_cancel_interaction(s);
        }
        return;
      }
    }
    hot->state = STATE_DESTROYED;
  }
  client_unmanage(s, h);
}

/*
 * wm_handle_configure_request:
 * Handle a client's request to change its geometry (e.g., self-resize).
 *
 * We update the `desired` geometry but do NOT apply it immediately.
 * The `DIRTY_GEOM` flag is set, and the actual X11 configure event will be sent
 * during the flush phase. This allows us to coalesce multiple requests and
 * apply constraints (min/max size, aspect ratio) in one place.
 */
void wm_handle_configure_request(server_t* s, handle_t h, pending_config_t* ev) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  TRACE_LOG("configure_request h=%lx xid=%u mask=0x%x x=%d y=%d w=%u h=%u bw=%u", h, hot->xid, ev->mask, ev->x, ev->y, ev->width, ev->height, ev->border_width);

  if (ev->mask & XCB_CONFIG_WINDOW_X)
    hot->desired.x = ev->x;
  if (ev->mask & XCB_CONFIG_WINDOW_Y)
    hot->desired.y = ev->y;
  if (ev->mask & XCB_CONFIG_WINDOW_WIDTH)
    hot->desired.w = ev->width;
  if (ev->mask & XCB_CONFIG_WINDOW_HEIGHT)
    hot->desired.h = ev->height;

  hot->geometry_from_configure = true;

  // Reworked GTK handling: treat as standard windows for now
  if (hot->gtk_frame_extents_set) {
    if (ev->mask & XCB_CONFIG_WINDOW_X)
      hot->desired.x += (int16_t)hot->gtk_extents.left;
    if (ev->mask & XCB_CONFIG_WINDOW_Y)
      hot->desired.y += (int16_t)hot->gtk_extents.top;
    // Do not adjust width/height; client requests full size including shadows
  }

  bool is_panel = (hot->type == WINDOW_TYPE_DOCK || hot->type == WINDOW_TYPE_DESKTOP);
  if (!is_panel) {
    client_constrain_size(&hot->hints, hot->hints_flags, &hot->desired.w, &hot->desired.h);
  }
  hot->dirty |= DIRTY_GEOM;

  LOG_DEBUG("Client %lx desired geom updated: %d,%d %dx%d (mask %x)", h, hot->desired.x, hot->desired.y, hot->desired.w, hot->desired.h, ev->mask);
}

void wm_handle_configure_notify(server_t* s, handle_t h, xcb_configure_notify_event_t* ev) {
  (void)s;
  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  TRACE_LOG("configure_notify h=%lx win=%u x=%d y=%d w=%u h=%u bw=%u above=%u", h, ev->window, ev->x, ev->y, ev->width, ev->height, ev->border_width, ev->above_sibling);

  if (ev->window == hot->frame) {
    hot->server.x = ev->x;
    hot->server.y = ev->y;
    LOG_DEBUG("Client %lx frame pos updated: %d,%d", h, ev->x, ev->y);
  }
  else if (ev->window == hot->xid) {
    hot->server.w = ev->width;
    hot->server.h = ev->height;
    LOG_DEBUG("Client %lx window size updated: %dx%d", h, ev->width, ev->height);
  }
}

void wm_handle_property_notify(server_t* s, handle_t h, xcb_property_notify_event_t* ev) {
  if (ev->atom == atoms._NET_WM_ALLOWED_ACTIONS || ev->atom == atoms._NET_FRAME_EXTENTS || ev->atom == atoms.WM_STATE || ev->atom == atoms._NET_WM_VISIBLE_NAME ||
      ev->atom == atoms._NET_WM_VISIBLE_ICON_NAME) {
    return;
  }
  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  TRACE_LOG("property_notify h=%lx xid=%u atom=%u (%s) state=%u", h, hot->xid, ev->atom, atom_name(ev->atom), ev->state);
  if (ev->atom == atoms.WM_NAME || ev->atom == atoms._NET_WM_NAME) {
    hot->dirty |= DIRTY_TITLE;
  }
  else if (ev->atom == atoms.WM_HINTS) {
    hot->dirty |= DIRTY_HINTS;
  }
  else if (ev->atom == atoms.WM_NORMAL_HINTS) {
    hot->dirty |= DIRTY_HINTS;
  }
  else if (ev->atom == atoms.WM_COLORMAP_WINDOWS) {
    hot->dirty |= DIRTY_HINTS;
  }
  else if (ev->atom == atoms.WM_PROTOCOLS) {
    xcb_get_property_cookie_t ck = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_PROTOCOLS, XCB_ATOM_ATOM, 0, 32);
    cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms.WM_PROTOCOLS, s->txn_id, wm_handle_reply);
  }
  else if (ev->atom == atoms._NET_WM_ICON) {
    xcb_get_property_cookie_t ck = xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_ICON, XCB_ATOM_CARDINAL, 0, 1048576);
    cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_ICON, s->txn_id, wm_handle_reply);
  }
  else if (ev->atom == atoms._NET_WM_STATE) {
    xcb_get_property_cookie_t ck = xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_STATE, XCB_ATOM_ATOM, 0, 32);
    cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_STATE, s->txn_id, wm_handle_reply);
  }
  else if (ev->atom == atoms._NET_WM_WINDOW_TYPE) {
    xcb_get_property_cookie_t ck = xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 0, 32);
    cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_WINDOW_TYPE, s->txn_id, wm_handle_reply);
  }
  else if (ev->atom == atoms._NET_WM_DESKTOP) {
    xcb_get_property_cookie_t ck = xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 0, 1);
    cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_DESKTOP, s->txn_id, wm_handle_reply);
  }
  else if (ev->atom == atoms._NET_WM_SYNC_REQUEST_COUNTER) {
    xcb_get_property_cookie_t ck = xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_SYNC_REQUEST_COUNTER, XCB_ATOM_CARDINAL, 0, 1);
    cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_SYNC_REQUEST_COUNTER, s->txn_id, wm_handle_reply);
  }
  else if (ev->atom == atoms._NET_WM_STRUT || ev->atom == atoms._NET_WM_STRUT_PARTIAL) {
    hot->dirty |= DIRTY_STRUT;
    // Waterfall: Always request PARTIAL first. If it fails/empty, we fallback
    // to STRUT in reply handler
    xcb_get_property_cookie_t ck = xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 0, 12);
    cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | (uint32_t)atoms._NET_WM_STRUT_PARTIAL, s->txn_id, wm_handle_reply);
  }
  else if (ev->atom == atoms._NET_WM_WINDOW_OPACITY) {
    hot->dirty |= DIRTY_OPACITY;
  }
  else if (ev->atom == atoms._MOTIF_WM_HINTS) {
    hot->dirty |= DIRTY_HINTS;
  }
  else if (ev->atom == atoms._GTK_FRAME_EXTENTS) {
    hot->dirty |= DIRTY_HINTS;
  }
  else if (ev->atom == atoms._NET_WM_BYPASS_COMPOSITOR) {
    hot->dirty |= DIRTY_BYPASS_COMPOSITOR;
  }
}

static bool colormap_list_contains(const client_cold_t* cold, xcb_window_t win) {
  if (!cold || !cold->colormap_windows || cold->colormap_windows_len == 0)
    return false;
  for (uint32_t i = 0; i < cold->colormap_windows_len; i++) {
    if (cold->colormap_windows[i] == win)
      return true;
  }
  return false;
}

void wm_handle_colormap_notify(server_t* s, xcb_colormap_notify_event_t* ev) {
  if (!s || !ev)
    return;
  if (s->focused_client == HANDLE_INVALID)
    return;

  client_hot_t* hot = server_chot(s, s->focused_client);
  if (!hot)
    return;
  client_cold_t* cold = server_ccold(s, s->focused_client);

  bool match = false;
  if (cold && cold->colormap_windows_len > 0) {
    match = colormap_list_contains(cold, ev->window);
  }
  else {
    match = (ev->window == hot->xid || ev->window == hot->frame);
  }

  if (match) {
    wm_install_client_colormap(s, hot);
  }
}

void wm_client_apply_state_set(server_t* s, handle_t h, const client_state_set_t* set) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot || !set)
    return;

  if (hot->maximized_horz != set->max_horz || hot->maximized_vert != set->max_vert) {
    wm_client_set_maximize(s, hot, set->max_horz, set->max_vert);
  }

  if (set->above) {
    if (!hot->state_above)
      wm_client_update_state(s, h, 1, atoms._NET_WM_STATE_ABOVE);
  }
  else if (hot->state_above) {
    wm_client_update_state(s, h, 0, atoms._NET_WM_STATE_ABOVE);
  }

  if (set->below && !set->above) {
    if (!hot->state_below)
      wm_client_update_state(s, h, 1, atoms._NET_WM_STATE_BELOW);
  }
  else if (hot->state_below) {
    wm_client_update_state(s, h, 0, atoms._NET_WM_STATE_BELOW);
  }

  if (set->urgent) {
    if (!(hot->flags & CLIENT_FLAG_URGENT))
      wm_client_update_state(s, h, 1, atoms._NET_WM_STATE_DEMANDS_ATTENTION);
  }
  else if (hot->flags & CLIENT_FLAG_URGENT) {
    wm_client_update_state(s, h, 0, atoms._NET_WM_STATE_DEMANDS_ATTENTION);
  }

  if (set->fullscreen) {
    if (hot->layer != LAYER_FULLSCREEN)
      wm_client_update_state(s, h, 1, atoms._NET_WM_STATE_FULLSCREEN);
  }
  else if (hot->layer == LAYER_FULLSCREEN) {
    wm_client_update_state(s, h, 0, atoms._NET_WM_STATE_FULLSCREEN);
  }

  if (set->sticky != hot->sticky) {
    wm_client_toggle_sticky(s, h);
  }

  if (hot->skip_taskbar != set->skip_taskbar) {
    hot->skip_taskbar = set->skip_taskbar;
    hot->dirty |= DIRTY_STATE;
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING;
  }

  if (hot->skip_pager != set->skip_pager) {
    hot->skip_pager = set->skip_pager;
    hot->dirty |= DIRTY_STATE;
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING;
  }
}

// Focus & keys

// Helpers

static int wm_get_resize_dir(server_t* s, client_hot_t* hot, int16_t x, int16_t y) {
  uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
  uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;
  uint16_t bottom_h = bw;

  uint16_t frame_w = hot->server.w + 2 * bw;
  uint16_t frame_h = hot->server.h + th + bottom_h;

  int dir = RESIZE_NONE;
  if (x < bw)
    dir |= RESIZE_LEFT;
  if (x >= frame_w - bw)
    dir |= RESIZE_RIGHT;
  if (y < bw)
    dir |= RESIZE_TOP;  // Top border (part of titlebar area technically)
  if (y >= frame_h - bw)
    dir |= RESIZE_BOTTOM;

  return dir;
}

static void wm_update_cursor(server_t* s, xcb_window_t win, int dir) {
  xcb_cursor_t c = s->cursor_left_ptr;
  if (dir == RESIZE_TOP)
    c = s->cursor_resize_top;
  else if (dir == RESIZE_BOTTOM)
    c = s->cursor_resize_bottom;
  else if (dir == RESIZE_LEFT)
    c = s->cursor_resize_left;
  else if (dir == RESIZE_RIGHT)
    c = s->cursor_resize_right;
  else if (dir == (RESIZE_TOP | RESIZE_LEFT))
    c = s->cursor_resize_top_left;
  else if (dir == (RESIZE_TOP | RESIZE_RIGHT))
    c = s->cursor_resize_top_right;
  else if (dir == (RESIZE_BOTTOM | RESIZE_LEFT))
    c = s->cursor_resize_bottom_left;
  else if (dir == (RESIZE_BOTTOM | RESIZE_RIGHT))
    c = s->cursor_resize_bottom_right;

  xcb_change_window_attributes(s->conn, win, XCB_CW_CURSOR, &c);
}

void wm_cancel_interaction(server_t* s) {
  if (s->interaction_mode == INTERACTION_NONE)
    return;
  xcb_window_t frame = s->interaction_window;
  handle_t h = s->interaction_handle;
  client_hot_t* hot_clear = server_chot(s, h);
  if (hot_clear) {
    hot_clear->snap_preview_active = false;
    hot_clear->snap_preview_edge = SNAP_NONE;
  }
  s->interaction_mode = INTERACTION_NONE;
  s->interaction_window = XCB_NONE;
  s->interaction_handle = HANDLE_INVALID;
  s->interaction_resize_dir = RESIZE_NONE;
  s->interaction_time = 0;
  xcb_ungrab_pointer(s->conn, XCB_CURRENT_TIME);
  if (frame != XCB_NONE) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) {
      handle_t fh = server_get_client_by_frame(s, frame);
      hot = server_chot(s, fh);
    }
    if (hot) {
      wm_update_cursor(s, hot->frame, RESIZE_NONE);
      hot->last_cursor_dir = RESIZE_NONE;
    }
  }
  LOG_INFO("Ended interaction");
}

void wm_start_interaction(server_t* s, handle_t h, client_hot_t* hot, bool start_move, int resize_dir, int16_t root_x, int16_t root_y, uint32_t time, bool is_keyboard) {
  (void)h;
  assert(s->interaction_mode == INTERACTION_NONE);
  s->interaction_mode = start_move ? INTERACTION_MOVE : INTERACTION_RESIZE;
  s->interaction_resize_dir = resize_dir;
  s->interaction_window = hot->frame;
  s->interaction_handle = h;
  s->interaction_time = time;
  s->interaction_requires_buttons = !is_keyboard;

  s->interaction_start_x = start_move ? hot->desired.x : hot->server.x;
  s->interaction_start_y = start_move ? hot->desired.y : hot->server.y;
  s->interaction_start_w = start_move ? hot->desired.w : hot->server.w;
  s->interaction_start_h = start_move ? hot->desired.h : hot->server.h;

  s->interaction_pointer_x = root_x;
  s->interaction_pointer_y = root_y;
  s->last_interaction_flush = 0;

  xcb_cursor_t cursor = XCB_NONE;
  if (start_move) {
    cursor = s->cursor_move;
  }
  else if (resize_dir == RESIZE_TOP)
    cursor = s->cursor_resize_top;
  else if (resize_dir == RESIZE_BOTTOM)
    cursor = s->cursor_resize_bottom;
  else if (resize_dir == RESIZE_LEFT)
    cursor = s->cursor_resize_left;
  else if (resize_dir == RESIZE_RIGHT)
    cursor = s->cursor_resize_right;
  else if (resize_dir == (RESIZE_TOP | RESIZE_LEFT))
    cursor = s->cursor_resize_top_left;
  else if (resize_dir == (RESIZE_TOP | RESIZE_RIGHT))
    cursor = s->cursor_resize_top_right;
  else if (resize_dir == (RESIZE_BOTTOM | RESIZE_LEFT))
    cursor = s->cursor_resize_bottom_left;
  else if (resize_dir == (RESIZE_BOTTOM | RESIZE_RIGHT))
    cursor = s->cursor_resize_bottom_right;

  xcb_grab_pointer_cookie_t cookie = xcb_grab_pointer(s->conn, 0, s->root, XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_POINTER_MOTION, XCB_GRAB_MODE_ASYNC,
                                                      XCB_GRAB_MODE_ASYNC, XCB_NONE, cursor, time ? time : XCB_CURRENT_TIME);

  xcb_grab_pointer_reply_t* reply = xcb_grab_pointer_reply(s->conn, cookie, NULL);
  bool success = false;
  if (reply) {
    if (reply->status == XCB_GRAB_STATUS_SUCCESS) {
      success = true;
      LOG_INFO("grab_pointer success status=%u on root=%u", reply->status, s->root);
    }
    else {
      LOG_ERROR("grab_pointer failed status=%u on root=%u", reply->status, s->root);
    }
    free(reply);
  }
  else {
    LOG_ERROR("grab_pointer reply was NULL");
  }

  if (!success) {
    wm_cancel_interaction(s);
    return;
  }

  LOG_INFO("Started interactive %s for client %lx (dir=%d)", start_move ? "MOVE" : "RESIZE", h, resize_dir);
}

// Mouse interaction

void wm_handle_button_press(server_t* s, xcb_button_press_event_t* ev) {
  if (s->interaction_mode == INTERACTION_MENU) {
    menu_handle_button_press(s, ev);
    return;
  }

  // Root menu and workspace scroll on empty root clicks
  if (ev->event == s->root && ev->child == XCB_NONE) {
    if (ev->detail == 2) {
      menu_show_client_list(s, ev->root_x, ev->root_y);
      return;
    }
    if (ev->detail == 3) {
      menu_show(s, ev->root_x, ev->root_y);
      return;
    }
    if (ev->detail == 4) {
      wm_switch_workspace_relative(s, -1);
      return;
    }
    if (ev->detail == 5) {
      wm_switch_workspace_relative(s, 1);
      return;
    }
  }

  // Identify target client
  bool is_frame = false;
  handle_t h = server_get_client_by_frame(s, ev->event);
  if (h != HANDLE_INVALID) {
    is_frame = true;
  }
  else {
    h = server_get_client_by_window(s, ev->event);
  }
  if (h == HANDLE_INVALID)
    return;

  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  // Focus + optional raise on click
  if (s->focused_client != h && hot->type != WINDOW_TYPE_DOCK && hot->type != WINDOW_TYPE_DESKTOP && hot->type != WINDOW_TYPE_NOTIFICATION && hot->type != WINDOW_TYPE_MENU &&
      hot->type != WINDOW_TYPE_DROPDOWN_MENU && hot->type != WINDOW_TYPE_POPUP_MENU && hot->type != WINDOW_TYPE_TOOLTIP && hot->type != WINDOW_TYPE_COMBO && hot->type != WINDOW_TYPE_DND) {
    wm_set_focus(s, h);
    if (should_raise_on_click(hot, ev->detail))
      stack_raise(s, h);
  }

  uint32_t mods = wm_clean_mods(ev->state);

  bool start_move = false;
  bool start_resize = false;
  int resize_dir = RESIZE_NONE;

  // Alt+Button1 move, Alt+Button3 resize
  if (mods == XCB_MOD_MASK_1) {
    if (ev->detail == 1)
      start_move = true;
    if (ev->detail == 3) {
      start_resize = true;
      resize_dir = RESIZE_BOTTOM | RESIZE_RIGHT;  // Default to BR for Alt-Resize
    }
  }
  else if (is_frame && ev->detail == 1) {
    // Click on frame: check for buttons first
    frame_button_t btn = frame_get_button_at(s, h, ev->event_x, ev->event_y);
    if (btn != FRAME_BUTTON_NONE) {
      if (btn == FRAME_BUTTON_CLOSE) {
        client_close(s, h);
      }
      else if (btn == FRAME_BUTTON_MAXIMIZE) {
        wm_client_toggle_maximize(s, h);
      }
      else if (btn == FRAME_BUTTON_MINIMIZE) {
        wm_client_iconify(s, h);
      }
      xcb_allow_events(s->conn, XCB_ALLOW_ASYNC_POINTER, ev->time);
      return;
    }

    // Check for border resize
    // Use event_x/y which are relative to the event window (the frame)
    resize_dir = wm_get_resize_dir(s, hot, ev->event_x, ev->event_y);

    if (resize_dir != RESIZE_NONE) {
      start_resize = true;
    }
    else {
      // Click-drag on frame (titlebar/content) acts like move
      start_move = true;
    }
  }

  if (start_move && !client_can_move(hot))
    start_move = false;
  if (start_resize && !client_can_resize(hot))
    start_resize = false;

  if (start_move && s->snap_enabled && hot->snap_active) {
    hot->desired = hot->snap_restore_frame_rect;
    hot->snap_active = false;
    hot->snap_edge = SNAP_NONE;
    hot->snap_preview_active = false;
    hot->snap_preview_edge = SNAP_NONE;
    hot->dirty |= DIRTY_GEOM;
  }

  if (!start_move && !start_resize) {
    xcb_allow_events(s->conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
    return;
  }

  wm_start_interaction(s, h, hot, start_move, resize_dir, ev->root_x, ev->root_y, ev->time, false);

  xcb_allow_events(s->conn, XCB_ALLOW_ASYNC_POINTER, ev->time);
}

void wm_handle_button_release(server_t* s, xcb_button_release_event_t* ev) {
  if (s->interaction_mode == INTERACTION_MENU) {
    menu_handle_button_release(s, ev);
    return;
  }

  if (s->interaction_mode != INTERACTION_NONE) {
    LOG_INFO("ButtonRelease in interaction");
  }

  if (s->interaction_mode == INTERACTION_MOVE) {
    handle_t h = s->interaction_handle;
    client_hot_t* hot = server_chot(s, h);
    if (hot) {
      bool can_snap = s->snap_enabled && client_can_move(hot) && !hot->override_redirect && hot->layer != LAYER_FULLSCREEN && hot->type != WINDOW_TYPE_DOCK;
      if (can_snap && hot->snap_preview_active) {
        hot->snap_restore_frame_rect = hot->server;
        hot->desired = hot->snap_preview_frame_rect;
        hot->snap_active = true;
        hot->snap_edge = hot->snap_preview_edge;
        hot->dirty |= DIRTY_GEOM;
      }
      hot->snap_preview_active = false;
      hot->snap_preview_edge = SNAP_NONE;
    }
  }

  wm_cancel_interaction(s);
}

void wm_handle_motion_notify(server_t* s, xcb_motion_notify_event_t* ev) {
  if (s->interaction_mode == INTERACTION_MENU) {
    menu_handle_pointer_motion(s, ev->root_x, ev->root_y);
    return;
  }

  if (s->interaction_mode == INTERACTION_NONE) {
    // Handle cursor updates on frame hover
    handle_t h = server_get_client_by_frame(s, ev->event);
    if (h != HANDLE_INVALID) {
      client_hot_t* hot = server_chot(s, h);
      if (hot) {
        int dir = wm_get_resize_dir(s, hot, ev->event_x, ev->event_y);
        // Optimization: only update if changed
        // Initialize last_cursor_dir to -1 or something in client alloc?
        // client_manage_start zeroes the struct (via calloc in slotmap or
        // explicit init?) slotmap_alloc does NOT zero. client_manage_start
        // does: hot->state = STATE_NEW; ... I need to init last_cursor_dir in
        // client_manage_start or handle it here. Let's assume initialized to 0
        // (RESIZE_NONE) is fine, but we might miss first update if it starts at
        // 0. Better to use a separate logic or just force it once? Actually,
        // RESIZE_NONE is 0. If we start at 0, and we are at 0, we do nothing
        // But we want to set it to LeftPtr initially. Let's rely on frame
        // creation setting cursor to None (inherit) or LeftPtr? Frame created
        // with mask, but no cursor attribute set? X default is "None"
        // (inherit). So initial 0 (RESIZE_NONE) -> updates to LeftPtr. If
        // hot->last_cursor_dir matches, we skip. We need to ensure we
        // initialize it to something invalid, e.g. -1
        if (hot->last_cursor_dir != dir) {
          wm_update_cursor(s, hot->frame, dir);
          hot->last_cursor_dir = dir;
        }
      }
    }
    return;
  }

  LOG_DEBUG("MotionNotify interactive: root=%d,%d event=%u child=%u", ev->root_x, ev->root_y, ev->event, ev->child);

  // Safety: if no buttons are down, force end interaction
  // This prevents "stuck drag" if we missed a ButtonRelease or if interaction
  // started late
  if (s->interaction_requires_buttons && !(ev->state & (XCB_KEY_BUT_MASK_BUTTON_1 | XCB_KEY_BUT_MASK_BUTTON_2 | XCB_KEY_BUT_MASK_BUTTON_3 | XCB_KEY_BUT_MASK_BUTTON_4 | XCB_KEY_BUT_MASK_BUTTON_5))) {
    LOG_INFO(
        "MotionNotify with no buttons down (state=0x%x): forcing "
        "interaction end",
        ev->state);
    wm_cancel_interaction(s);
    return;
  }

  handle_t h = server_get_client_by_frame(s, s->interaction_window);
  client_hot_t* hot = server_chot(s, h);
  if (!hot) {
    LOG_WARN("Interaction client not found h=%lx window=%u", h, s->interaction_window);
    s->interaction_mode = INTERACTION_NONE;
    xcb_ungrab_pointer(s->conn, XCB_CURRENT_TIME);
    return;
  }

  int dx = ev->root_x - s->interaction_pointer_x;
  int dy = ev->root_y - s->interaction_pointer_y;

  if (s->interaction_mode == INTERACTION_MOVE) {
    hot->desired.x = (int16_t)(s->interaction_start_x + dx);
    hot->desired.y = (int16_t)(s->interaction_start_y + dy);
    hot->dirty |= DIRTY_GEOM;

    hot->snap_preview_active = false;
    hot->snap_preview_edge = SNAP_NONE;

    if (s->snap_enabled && client_can_move(hot) && !hot->override_redirect && hot->layer != LAYER_FULLSCREEN && hot->type != WINDOW_TYPE_DOCK) {
      rect_t wa = s->workarea;
      int mid = wm_monitor_at_point(s, ev->root_x, ev->root_y);
      if (mid >= 0 && (uint32_t)mid < s->monitor_count) {
        wa = s->monitors[mid].workarea;
      }
      snap_candidate_t cand = snap_compute_candidate(ev->root_x, ev->root_y, wa, (int)s->snap_threshold_px);
      if (cand.active) {
        hot->snap_preview_active = true;
        hot->snap_preview_edge = cand.edge;
        hot->snap_preview_frame_rect = cand.rect;
      }
    }
    return;
  }

  // Resize logic
  int new_w = s->interaction_start_w;
  int new_h = s->interaction_start_h;

  if (s->interaction_resize_dir & RESIZE_RIGHT) {
    new_w += dx;
  }
  else if (s->interaction_resize_dir & RESIZE_LEFT) {
    new_w -= dx;
  }

  if (s->interaction_resize_dir & RESIZE_BOTTOM) {
    new_h += dy;
  }
  else if (s->interaction_resize_dir & RESIZE_TOP) {
    new_h -= dy;
  }

  bool is_panel = (hot->type == WINDOW_TYPE_DOCK || hot->type == WINDOW_TYPE_DESKTOP);

  // Constrain size
  if (is_panel) {
    if (new_w < 1)
      new_w = 1;
    if (new_h < 1)
      new_h = 1;
  }
  else {
    if (new_w < MIN_FRAME_SIZE)
      new_w = MIN_FRAME_SIZE;
    if (new_w > MAX_FRAME_SIZE)
      new_w = MAX_FRAME_SIZE;
    if (new_h < MIN_FRAME_SIZE)
      new_h = MIN_FRAME_SIZE;
    if (new_h > MAX_FRAME_SIZE)
      new_h = MAX_FRAME_SIZE;
  }

  uint16_t w = (uint16_t)new_w;
  uint16_t h_val = (uint16_t)new_h;

  // Apply hints
  if (!is_panel) {
    client_constrain_size(&hot->hints, hot->hints_flags, &w, &h_val);
  }
  // Adjust position if resizing top/left
  // Determine effective delta
  int dw = (int)w - s->interaction_start_w;
  int dh = (int)h_val - s->interaction_start_h;

  if (s->interaction_resize_dir & RESIZE_LEFT) {
    hot->desired.x = (int16_t)(s->interaction_start_x - dw);
  }
  else {
    hot->desired.x = (int16_t)s->interaction_start_x;
  }

  if (s->interaction_resize_dir & RESIZE_TOP) {
    hot->desired.y = (int16_t)(s->interaction_start_y - dh);
  }
  else {
    hot->desired.y = (int16_t)s->interaction_start_y;
  }

  hot->desired.w = w;
  hot->desired.h = h_val;

  hot->dirty |= DIRTY_GEOM;
}

// EWMH client messages / state

void wm_client_update_state(server_t* s, handle_t h, uint32_t action, xcb_atom_t prop) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  if (prop == atoms._NET_WM_STATE_FOCUSED)
    return;

  if (prop == atoms._NET_WM_STATE_HIDDEN) {
    // Ignore state changes during initial management
    if (hot->state == STATE_NEW) {
      LOG_DEBUG("Ignoring _NET_WM_STATE_HIDDEN change for client %lx in STATE_NEW", h);
      return;
    }

    if (action == 2) {
      if (hot->state == STATE_MAPPED) {
        wm_client_iconify(s, h);
      }
      else if (hot->state == STATE_UNMAPPED) {
        LOG_DEBUG("wm_client_update_state: Restoring client %lx (action=2, hidden)", h);
        wm_client_restore(s, h);
      }
    }
    else if (action == 1) {
      wm_client_iconify(s, h);
    }
    else if (action == 0) {
      if (hot->state == STATE_UNMAPPED) {
        LOG_DEBUG("wm_client_update_state: Restoring client %lx (action=0, hidden)", h);
        wm_client_restore(s, h);
      }
    }
    return;
  }

  bool add = false;

  if (action == 1) {
    add = true;
  }
  else if (action == 0) {
    add = false;
  }
  else if (action == 2) {
    if (prop == atoms._NET_WM_STATE_FULLSCREEN)
      add = (hot->layer != LAYER_FULLSCREEN);
    else if (prop == atoms._NET_WM_STATE_ABOVE)
      add = !hot->state_above;
    else if (prop == atoms._NET_WM_STATE_BELOW)
      add = !hot->state_below;
    else if (prop == atoms._NET_WM_STATE_STICKY)
      add = !hot->sticky;
    else if (prop == atoms._NET_WM_STATE_DEMANDS_ATTENTION)
      add = !(hot->flags & CLIENT_FLAG_URGENT);
    else if (prop == atoms._NET_WM_STATE_MAXIMIZED_HORZ)
      add = !hot->maximized_horz;
    else if (prop == atoms._NET_WM_STATE_MAXIMIZED_VERT)
      add = !hot->maximized_vert;
    else if (prop == atoms._NET_WM_STATE_SKIP_TASKBAR)
      add = !hot->skip_taskbar;
    else if (prop == atoms._NET_WM_STATE_SKIP_PAGER)
      add = !hot->skip_pager;
    else
      return;
  }
  else {
    return;
  }

  if (prop == atoms._NET_WM_STATE_FULLSCREEN) {
    if (add && hot->layer != LAYER_FULLSCREEN) {
      hot->saved_geom = hot->server;
      hot->saved_layer = hot->layer;
      hot->saved_state_mask = hot->flags & CLIENT_FLAG_UNDECORATED;
      hot->saved_maximized_horz = hot->maximized_horz;
      hot->saved_maximized_vert = hot->maximized_vert;
      hot->maximized_horz = false;
      hot->maximized_vert = false;
      hot->layer = LAYER_FULLSCREEN;
      hot->flags |= CLIENT_FLAG_UNDECORATED;

      if (hot->fullscreen_monitors_valid) {
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_FULLSCREEN_MONITORS, XCB_ATOM_CARDINAL, 32, 4, hot->fullscreen_monitors);
      }
      else {
        xcb_delete_property(s->conn, hot->xid, atoms._NET_WM_FULLSCREEN_MONITORS);
      }

      if (s->config.fullscreen_use_workarea) {
        hot->desired = s->workarea;
      }
      else {
        wm_get_monitor_geometry(s, hot, &hot->desired);
      }

      hot->dirty |= DIRTY_GEOM | DIRTY_STATE | DIRTY_STACK;
    }
    else if (!add && hot->layer == LAYER_FULLSCREEN) {
      hot->layer = client_layer_from_state(hot);
      hot->desired = hot->saved_geom;
      hot->flags = (hot->flags & ~CLIENT_FLAG_UNDECORATED) | (hot->saved_state_mask & CLIENT_FLAG_UNDECORATED);
      hot->maximized_horz = hot->saved_maximized_horz;
      hot->maximized_vert = hot->saved_maximized_vert;
      xcb_delete_property(s->conn, hot->xid, atoms._NET_WM_FULLSCREEN_MONITORS);
      hot->dirty |= DIRTY_GEOM | DIRTY_STATE | DIRTY_STACK;
    }
    return;
  }

  if (prop == atoms._NET_WM_STATE_ABOVE) {
    if (add) {
      hot->state_above = true;
      hot->state_below = false;
    }
    else {
      hot->state_above = false;
    }
    if (hot->layer != LAYER_FULLSCREEN) {
      uint8_t desired = client_layer_from_state(hot);
      if (hot->layer != desired) {
        hot->layer = desired;
        hot->dirty |= DIRTY_STACK;
      }
    }
    hot->dirty |= DIRTY_STATE;
    return;
  }

  if (prop == atoms._NET_WM_STATE_BELOW) {
    if (add) {
      hot->state_below = true;
      hot->state_above = false;
    }
    else {
      hot->state_below = false;
    }
    if (hot->layer != LAYER_FULLSCREEN) {
      uint8_t desired = client_layer_from_state(hot);
      if (hot->layer != desired) {
        hot->layer = desired;
        hot->dirty |= DIRTY_STACK;
      }
    }
    hot->dirty |= DIRTY_STATE;
    return;
  }

  if (prop == atoms._NET_WM_STATE_STICKY) {
    if (hot->sticky != add)
      wm_client_toggle_sticky(s, h);
    return;
  }

  if (prop == atoms._NET_WM_STATE_SKIP_TASKBAR) {
    if (hot->skip_taskbar != add) {
      hot->skip_taskbar = add;
      hot->dirty |= DIRTY_STATE;
      s->root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING;
    }
    return;
  }

  if (prop == atoms._NET_WM_STATE_SKIP_PAGER) {
    if (hot->skip_pager != add) {
      hot->skip_pager = add;
      hot->dirty |= DIRTY_STATE;
      s->root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING;
    }
    return;
  }

  if (prop == atoms._NET_WM_STATE_DEMANDS_ATTENTION) {
    if (add)
      hot->flags |= CLIENT_FLAG_URGENT;
    else
      hot->flags &= ~CLIENT_FLAG_URGENT;
    hot->dirty |= DIRTY_STATE | DIRTY_FRAME_STYLE;
    return;
  }

  if (prop == atoms._NET_WM_STATE_MAXIMIZED_HORZ || prop == atoms._NET_WM_STATE_MAXIMIZED_VERT) {
    bool new_horz = hot->maximized_horz;
    bool new_vert = hot->maximized_vert;

    if (prop == atoms._NET_WM_STATE_MAXIMIZED_HORZ)
      new_horz = add;
    if (prop == atoms._NET_WM_STATE_MAXIMIZED_VERT)
      new_vert = add;

    wm_client_set_maximize(s, hot, new_horz, new_vert);
    return;
  }
}

void wm_handle_client_message(server_t* s, xcb_client_message_event_t* ev) {
  TRACE_ONLY({
    TRACE_LOG("client_message type=%u window=%u format=%u data=[%u,%u,%u,%u,%u]", ev->type, ev->window, ev->format, ev->data.data32[0], ev->data.data32[1], ev->data.data32[2], ev->data.data32[3],
              ev->data.data32[4]);
  });
  if (ev->type == atoms.WM_PROTOCOLS && ev->window == s->root && ev->format == 32 && ev->data.data32[0] == atoms._NET_WM_PING) {
    LOG_DEBUG("Received _NET_WM_PING reply for window %u", ev->data.data32[2]);
    return;
  }

  if (ev->type == atoms.WM_CHANGE_STATE && ev->format == 32) {
    handle_t h = server_get_client_by_window(s, ev->window);
    if (h != HANDLE_INVALID) {
      uint32_t state = ev->data.data32[0];
      if (state == XCB_ICCCM_WM_STATE_ICONIC) {
        wm_client_iconify(s, h);
      }
      else if (state == XCB_ICCCM_WM_STATE_NORMAL) {
        LOG_DEBUG("wm_handle_client_message: Restoring client %lx (WM_CHANGE_STATE)", h);
        wm_client_restore(s, h);
      }
    }
    return;
  }

  if (ev->type == atoms._NET_CURRENT_DESKTOP) {
    uint32_t desktop = ev->data.data32[0];
    TRACE_LOG("_NET_CURRENT_DESKTOP request=%u", desktop);
    if (desktop >= s->desktop_count) {
      LOG_INFO("Client requested switch to desktop %u (out of range)", desktop);
      return;
    }
    LOG_INFO("Client requested switch to desktop %u", desktop);
    wm_switch_workspace(s, desktop);
    return;
  }

  if (ev->type == atoms._NET_SHOWING_DESKTOP) {
    if (ev->window != s->root || ev->format != 32)
      return;
    TRACE_LOG("_NET_SHOWING_DESKTOP request=%u", ev->data.data32[0]);
    wm_set_showing_desktop(s, ev->data.data32[0] != 0);
    return;
  }

  if (ev->type == atoms._NET_DESKTOP_GEOMETRY) {
    if (ev->window != s->root || ev->format != 32)
      return;
    TRACE_LOG("_NET_DESKTOP_GEOMETRY request=%u,%u", ev->data.data32[0], ev->data.data32[1]);
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
    uint32_t geometry[] = {screen->width_in_pixels, screen->height_in_pixels};
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_DESKTOP_GEOMETRY, XCB_ATOM_CARDINAL, 32, 2, geometry);
    return;
  }

  if (ev->type == atoms._NET_DESKTOP_VIEWPORT) {
    if (ev->window != s->root || ev->format != 32)
      return;
    TRACE_LOG("_NET_DESKTOP_VIEWPORT request=%u,%u", ev->data.data32[0], ev->data.data32[1]);
    uint32_t* viewport = calloc(s->desktop_count * 2, sizeof(uint32_t));
    if (viewport) {
      xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_DESKTOP_VIEWPORT, XCB_ATOM_CARDINAL, 32, s->desktop_count * 2, viewport);
      free(viewport);
    }
    return;
  }

  if (ev->type == atoms._NET_NUMBER_OF_DESKTOPS) {
    uint32_t requested = ev->data.data32[0];
    TRACE_LOG("_NET_NUMBER_OF_DESKTOPS request=%u", requested);
    if (requested == 0)
      requested = 1;
    if (requested != s->desktop_count) {
      LOG_INFO("Client requested %u desktops", requested);
      s->desktop_count = requested;
      if (s->current_desktop >= s->desktop_count)
        s->current_desktop = 0;

      for (size_t i = 0; i < s->active_clients.length; i++) {
        handle_t h = ptr_to_handle(s->active_clients.items[i]);
        client_hot_t* hot = server_chot(s, h);
        if (!hot || hot->sticky)
          continue;
        if (hot->desktop >= (int32_t)s->desktop_count) {
          hot->desktop = (int32_t)s->current_desktop;
          uint32_t prop_val = (uint32_t)hot->desktop;
          xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 32, 1, &prop_val);
        }
      }
    }
    wm_publish_desktop_props(s);
    s->workarea_dirty = true;
    return;
  }

  if (ev->type == atoms._NET_WM_DESKTOP) {
    handle_t h = server_get_client_by_window(s, ev->window);
    if (h != HANDLE_INVALID) {
      uint32_t desktop = ev->data.data32[0];
      TRACE_LOG("_NET_WM_DESKTOP h=%lx desktop=%u", h, desktop);
      if (desktop != 0xFFFFFFFFu && desktop >= s->desktop_count) {
        LOG_INFO("Client requested move to desktop %u (out of range)", desktop);
        return;
      }
      wm_client_move_to_workspace(s, h, desktop, false);
    }
    return;
  }

  if (ev->type == atoms._NET_ACTIVE_WINDOW) {
    handle_t h = server_get_client_by_window(s, ev->window);
    if (h != HANDLE_INVALID) {
      client_hot_t* hot = server_chot(s, h);
      if (!hot)
        return;
      TRACE_LOG("_NET_ACTIVE_WINDOW h=%lx xid=%u state=%d desktop=%d", h, hot->xid, hot->state, hot->desktop);

      if (hot->manage_phase != MANAGE_DONE || hot->frame == XCB_WINDOW_NONE) {
        LOG_DEBUG("Ignoring _NET_ACTIVE_WINDOW for incomplete client %lx phase=%d", h, hot->manage_phase);
        return;
      }

      if (!hot->sticky && hot->desktop >= 0 && (uint32_t)hot->desktop != s->current_desktop) {
        wm_switch_workspace(s, (uint32_t)hot->desktop);
      }
      bool was_unmapped = (hot->state == STATE_UNMAPPED);
      if (was_unmapped) {
        LOG_DEBUG(
            "wm_handle_client_message: Restoring client %lx "
            "(NET_ACTIVE_WINDOW)",
            h);
        wm_client_restore(s, h);
      }
      wm_set_focus(s, h);
      if (!s->config.focus_raise && !was_unmapped) {
        stack_raise(s, h);
      }
    }
    return;
  }

  if (ev->type == atoms._NET_WM_STATE) {
    if (ev->format != 32)
      return;
    handle_t h = server_get_client_by_window(s, ev->window);
    uint32_t action = ev->data.data32[0];
    xcb_atom_t p1 = ev->data.data32[1];
    xcb_atom_t p2 = ev->data.data32[2];
    TRACE_LOG("_NET_WM_STATE h=%lx win=%u action=%u p1=%s(%u) p2=%s(%u)", h, ev->window, action, atom_name(p1), p1, atom_name(p2), p2);

    if (h != HANDLE_INVALID) {
      client_hot_t* hot = server_chot(s, h);
      if (!hot)
        return;

      if (hot->manage_phase != MANAGE_DONE || hot->frame == XCB_WINDOW_NONE) {
        if (hot->pending_state_count < 4) {
          LOG_DEBUG("Queueing _NET_WM_STATE for incomplete client %lx", h);
          hot->pending_state_msgs[hot->pending_state_count].action = action;
          hot->pending_state_msgs[hot->pending_state_count].p1 = p1;
          hot->pending_state_msgs[hot->pending_state_count].p2 = p2;
          hot->pending_state_count++;
        }
        else {
          LOG_WARN("Dropping _NET_WM_STATE for client %lx (queue full)", h);
        }
        return;
      }

      if (p1 != XCB_ATOM_NONE) {
        if (p1 == atoms._NET_WM_STATE_HIDDEN) {
          LOG_DEBUG("Ignoring client request for _NET_WM_STATE_HIDDEN (WM owned)");
        }
        else {
          wm_client_update_state(s, h, action, p1);
        }
      }
      if (p2 != XCB_ATOM_NONE) {
        if (p2 == atoms._NET_WM_STATE_HIDDEN) {
          LOG_DEBUG("Ignoring client request for _NET_WM_STATE_HIDDEN (WM owned)");
        }
        else {
          wm_client_update_state(s, h, action, p2);
        }
      }
    }
    else {
      LOG_DEBUG("Ignoring _NET_WM_STATE for unmanaged window %u", ev->window);
    }
    return;
  }

  if (ev->type == atoms._NET_WM_FULLSCREEN_MONITORS) {
    if (ev->format != 32)
      return;
    handle_t h = server_get_client_by_window(s, ev->window);
    if (h == HANDLE_INVALID)
      return;
    client_hot_t* hot = server_chot(s, h);
    if (!hot)
      return;
    TRACE_LOG("_NET_WM_FULLSCREEN_MONITORS h=%lx [%u,%u,%u,%u]", h, ev->data.data32[0], ev->data.data32[1], ev->data.data32[2], ev->data.data32[3]);

    hot->fullscreen_monitors[0] = ev->data.data32[0];
    hot->fullscreen_monitors[1] = ev->data.data32[1];
    hot->fullscreen_monitors[2] = ev->data.data32[2];
    hot->fullscreen_monitors[3] = ev->data.data32[3];
    hot->fullscreen_monitors_valid = true;

    if (hot->layer == LAYER_FULLSCREEN) {
      xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_FULLSCREEN_MONITORS, XCB_ATOM_CARDINAL, 32, 4, hot->fullscreen_monitors);
    }
    return;
  }

  if (ev->type == atoms._NET_REQUEST_FRAME_EXTENTS) {
    xcb_window_t target = ev->window;
    if (target == s->root)
      return;

    bool undecorated = false;
    handle_t h = server_get_client_by_window(s, target);
    if (h != HANDLE_INVALID) {
      client_hot_t* hot = server_chot(s, h);
      if (hot)
        undecorated = (hot->flags & CLIENT_FLAG_UNDECORATED) != 0;
      TRACE_LOG("_NET_REQUEST_FRAME_EXTENTS win=%u undecorated=%d (managed)", target, undecorated);
      wm_set_frame_extents_for_window(s, target, undecorated);
      return;
    }

    // Async path for unmanaged windows: ask for _MOTIF_WM_HINTS and reply
    // later
    xcb_get_property_cookie_t ck = xcb_get_property(s->conn, 0, target, atoms._MOTIF_WM_HINTS, XCB_ATOM_ANY, 0, 5);
    cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_PROPERTY_FRAME_EXTENTS, HANDLE_INVALID, (uintptr_t)target, s->txn_id, wm_handle_reply);
    TRACE_LOG("_NET_REQUEST_FRAME_EXTENTS win=%u queued async motif hints", target);
    return;
  }

  if (ev->type == atoms._NET_RESTACK_WINDOW) {
    if (ev->format != 32)
      return;
    handle_t h = server_get_client_by_window(s, ev->window);
    if (h == HANDLE_INVALID)
      return;
    client_hot_t* hot = server_chot(s, h);
    if (!hot)
      return;

    xcb_window_t sibling_win = (xcb_window_t)ev->data.data32[1];
    uint32_t detail = ev->data.data32[2];
    TRACE_LOG("_NET_RESTACK_WINDOW h=%lx sibling=%u detail=%u", h, sibling_win, detail);
    handle_t sibling_h = (sibling_win != XCB_NONE) ? server_get_client_by_window(s, sibling_win) : HANDLE_INVALID;
    client_hot_t* sib = (sibling_h != HANDLE_INVALID) ? server_chot(s, sibling_h) : NULL;
    bool have_sibling = (sib != NULL);
    bool same_layer = have_sibling && stack_current_layer(hot) == stack_current_layer(sib);

    switch (detail) {
      case XCB_STACK_MODE_ABOVE:
        if (have_sibling) {
          stack_place_above(s, h, sibling_h);
        }
        else {
          stack_raise(s, h);
        }
        break;
      case XCB_STACK_MODE_BELOW:
        if (have_sibling) {
          stack_place_below(s, h, sibling_h);
        }
        else {
          stack_lower(s, h);
        }
        break;
      case XCB_STACK_MODE_TOP_IF:
        if (!have_sibling || !same_layer || !wm_is_above_in_layer(s, hot, sib)) {
          if (have_sibling)
            stack_place_above(s, h, sibling_h);
          else
            stack_raise(s, h);
        }
        break;
      case XCB_STACK_MODE_BOTTOM_IF:
        if (!have_sibling || !same_layer) {
          stack_lower(s, h);
        }
        else if (wm_is_above_in_layer(s, hot, sib)) {
          stack_place_below(s, h, sibling_h);
        }
        break;
      case XCB_STACK_MODE_OPPOSITE:
        if (have_sibling && same_layer) {
          if (wm_is_above_in_layer(s, hot, sib)) {
            stack_place_below(s, h, sibling_h);
          }
          else {
            stack_place_above(s, h, sibling_h);
          }
        }
        else {
          stack_raise(s, h);
        }
        break;
      default:
        break;
    }
    return;
  }

  if (ev->type == atoms._NET_MOVERESIZE_WINDOW) {
    if (ev->format != 32)
      return;

    xcb_window_t target = ev->window;
    uint32_t flags = ev->data.data32[0];
    uint32_t gravity = flags & 0xFFu;
    bool has_x = (flags & (1u << 8)) != 0;
    bool has_y = (flags & (1u << 9)) != 0;
    bool has_w = (flags & (1u << 10)) != 0;
    bool has_h = (flags & (1u << 11)) != 0;

    int32_t x = (int32_t)ev->data.data32[1];
    int32_t y = (int32_t)ev->data.data32[2];
    uint32_t w = ev->data.data32[3];
    uint32_t h_val = ev->data.data32[4];
    TRACE_LOG(
        "_NET_MOVERESIZE_WINDOW win=%u flags=0x%x gravity=%u x=%d y=%d "
        "w=%u h=%u",
        target, flags, gravity, x, y, w, h_val);

    handle_t h = server_get_client_by_window(s, target);
    if (h == HANDLE_INVALID) {
      uint16_t mask = 0;
      uint32_t values[4];
      int i = 0;
      if (has_x) {
        mask |= XCB_CONFIG_WINDOW_X;
        values[i++] = (uint32_t)x;
      }
      if (has_y) {
        mask |= XCB_CONFIG_WINDOW_Y;
        values[i++] = (uint32_t)y;
      }
      if (has_w) {
        mask |= XCB_CONFIG_WINDOW_WIDTH;
        values[i++] = w;
      }
      if (has_h) {
        mask |= XCB_CONFIG_WINDOW_HEIGHT;
        values[i++] = h_val;
      }
      if (mask)
        xcb_configure_window(s->conn, target, mask, values);
      return;
    }

    client_hot_t* hot = server_chot(s, h);
    if (!hot)
      return;

    bool use_frame_coords = (gravity == 10);
    uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
    uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;

    pending_config_t pc = {0};
    if (has_x) {
      int32_t adj_x = use_frame_coords ? x : (x - (int32_t)bw);
      pc.x = (int16_t)adj_x;
      pc.mask |= XCB_CONFIG_WINDOW_X;
    }
    if (has_y) {
      int32_t adj_y = use_frame_coords ? y : (y - (int32_t)th);
      pc.y = (int16_t)adj_y;
      pc.mask |= XCB_CONFIG_WINDOW_Y;
    }
    if (has_w) {
      pc.width = (uint16_t)w;
      pc.mask |= XCB_CONFIG_WINDOW_WIDTH;
    }
    if (has_h) {
      pc.height = (uint16_t)h_val;
      pc.mask |= XCB_CONFIG_WINDOW_HEIGHT;
    }

    if (pc.mask)
      wm_handle_configure_request(s, h, &pc);
    return;
  }

  if (ev->type == atoms._NET_WM_MOVERESIZE) {
    if (ev->format != 32)
      return;
    handle_t h = server_get_client_by_window(s, ev->window);
    if (h == HANDLE_INVALID)
      return;
    client_hot_t* hot = server_chot(s, h);
    if (!hot)
      return;

    uint32_t direction = ev->data.data32[2];
    TRACE_LOG("_NET_WM_MOVERESIZE h=%lx dir=%u button=%u source=%u", h, direction, ev->data.data32[3], ev->data.data32[4]);
    if (direction == NET_WM_MOVERESIZE_CANCEL) {
      wm_cancel_interaction(s);
      return;
    }

    bool use_pointer_query = (direction == NET_WM_MOVERESIZE_SIZE_KEYBOARD || direction == NET_WM_MOVERESIZE_MOVE_KEYBOARD);

    bool start_move = false;
    int resize_dir = RESIZE_NONE;

    switch (direction) {
      case NET_WM_MOVERESIZE_MOVE:
      case NET_WM_MOVERESIZE_MOVE_KEYBOARD:
        start_move = true;
        break;
      case NET_WM_MOVERESIZE_SIZE_TOPLEFT:
        resize_dir = RESIZE_TOP | RESIZE_LEFT;
        break;
      case NET_WM_MOVERESIZE_SIZE_TOP:
        resize_dir = RESIZE_TOP;
        break;
      case NET_WM_MOVERESIZE_SIZE_TOPRIGHT:
        resize_dir = RESIZE_TOP | RESIZE_RIGHT;
        break;
      case NET_WM_MOVERESIZE_SIZE_RIGHT:
        resize_dir = RESIZE_RIGHT;
        break;
      case NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT:
        resize_dir = RESIZE_BOTTOM | RESIZE_RIGHT;
        break;
      case NET_WM_MOVERESIZE_SIZE_BOTTOM:
        resize_dir = RESIZE_BOTTOM;
        break;
      case NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT:
        resize_dir = RESIZE_BOTTOM | RESIZE_LEFT;
        break;
      case NET_WM_MOVERESIZE_SIZE_LEFT:
        resize_dir = RESIZE_LEFT;
        break;
      case NET_WM_MOVERESIZE_SIZE_KEYBOARD:
        resize_dir = RESIZE_BOTTOM | RESIZE_RIGHT;
        break;
      default:
        return;
    }

    if (!start_move && resize_dir == RESIZE_NONE)
      return;

    if (s->focused_client != h && hot->type != WINDOW_TYPE_DOCK && hot->type != WINDOW_TYPE_DESKTOP && hot->type != WINDOW_TYPE_NOTIFICATION && hot->type != WINDOW_TYPE_MENU &&
        hot->type != WINDOW_TYPE_DROPDOWN_MENU && hot->type != WINDOW_TYPE_POPUP_MENU && hot->type != WINDOW_TYPE_TOOLTIP && hot->type != WINDOW_TYPE_COMBO && hot->type != WINDOW_TYPE_DND) {
      wm_set_focus(s, h);
      stack_raise(s, h);
    }

    if (s->interaction_mode != INTERACTION_NONE) {
      wm_cancel_interaction(s);
    }

    int16_t root_x = (int16_t)ev->data.data32[0];
    int16_t root_y = (int16_t)ev->data.data32[1];
    bool is_keyboard = (direction == NET_WM_MOVERESIZE_SIZE_KEYBOARD || direction == NET_WM_MOVERESIZE_MOVE_KEYBOARD);
    use_pointer_query |= (root_x == -1 || root_y == -1);

    TRACE_LOG("_NET_WM_MOVERESIZE h=%lx root=%d,%d use_query=%d", h, root_x, root_y, use_pointer_query);

    if (use_pointer_query) {
      uintptr_t data = (start_move ? 0x100 : 0) | (is_keyboard ? 0x200 : 0) | (uintptr_t)resize_dir;
      xcb_query_pointer_cookie_t ck = xcb_query_pointer(s->conn, s->root);
      cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_QUERY_POINTER, h, data, s->txn_id, wm_handle_reply);
    }
    else {
      wm_start_interaction(s, h, hot, start_move, resize_dir, root_x, root_y, 0, is_keyboard);
    }
    return;
  }

  if (ev->type == atoms._NET_CLOSE_WINDOW) {
    xcb_window_t target = ev->window;
    if (target == s->root && ev->format == 32) {
      target = (xcb_window_t)ev->data.data32[0];
    }
    handle_t h = server_get_client_by_window(s, target);
    if (h != HANDLE_INVALID) {
      TRACE_LOG("_NET_CLOSE_WINDOW h=%lx target=%u", h, target);
      LOG_INFO("Client requested close via _NET_CLOSE_WINDOW");
      client_close(s, h);
    }
    return;
  }
}

// Placement / workspaces

void wm_place_window(server_t* s, handle_t h) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  if (hot->type == WINDOW_TYPE_DOCK || hot->type == WINDOW_TYPE_DESKTOP || hot->type == WINDOW_TYPE_NOTIFICATION || hot->type == WINDOW_TYPE_MENU || hot->type == WINDOW_TYPE_DROPDOWN_MENU ||
      hot->type == WINDOW_TYPE_POPUP_MENU || hot->type == WINDOW_TYPE_TOOLTIP || hot->type == WINDOW_TYPE_COMBO || hot->type == WINDOW_TYPE_DND) {
    return;
  }

  // 1. Check rules/types for explicit placement
  if (hot->placement == PLACEMENT_CENTER) {
    hot->desired.x = (int16_t)(s->workarea.x + (s->workarea.w - hot->desired.w) / 2);
    hot->desired.y = (int16_t)(s->workarea.y + (s->workarea.h - hot->desired.h) / 2);
    return;
  }
  else if (hot->placement == PLACEMENT_MOUSE) {
    xcb_query_pointer_cookie_t cookie = xcb_query_pointer(s->conn, s->root);
    xcb_query_pointer_reply_t* reply = xcb_query_pointer_reply(s->conn, cookie, NULL);
    if (reply) {
      hot->desired.x = (int16_t)(reply->root_x - hot->desired.w / 2);
      hot->desired.y = (int16_t)(reply->root_y - hot->desired.h / 2);
      free(reply);
    }
  }
  else if (hot->transient_for != HANDLE_INVALID) {
    // Transients: center over parent
    client_hot_t* parent = server_chot(s, hot->transient_for);
    if (parent) {
      hot->desired.x = (int16_t)(parent->server.x + (parent->server.w - hot->desired.w) / 2);
      hot->desired.y = (int16_t)(parent->server.y + (parent->server.h - hot->desired.h) / 2);
      return;
    }
  }

  // Honor user/program position if specified (only if no rule applied)
  if (hot->placement == PLACEMENT_DEFAULT && (hot->hints_flags & XCB_ICCCM_SIZE_HINT_US_POSITION)) {
    return;
  }

  // Clamp to workarea
  if (hot->desired.x < s->workarea.x)
    hot->desired.x = s->workarea.x;
  if (hot->desired.y < s->workarea.y)
    hot->desired.y = s->workarea.y;

  if ((int32_t)hot->desired.x + (int32_t)hot->desired.w > (int32_t)s->workarea.x + (int32_t)s->workarea.w) {
    hot->desired.x = (int16_t)(s->workarea.x + s->workarea.w - hot->desired.w);
  }
  if ((int32_t)hot->desired.y + (int32_t)hot->desired.h > (int32_t)s->workarea.y + (int32_t)s->workarea.h) {
    hot->desired.y = (int16_t)(s->workarea.y + s->workarea.h - hot->desired.h);
  }

  if (hot->desired.x < s->workarea.x)
    hot->desired.x = s->workarea.x;
  if (hot->desired.y < s->workarea.y)
    hot->desired.y = s->workarea.y;
}

void wm_client_refresh_title(server_t* s, handle_t h) {
  client_hot_t* hot = server_chot(s, h);
  client_cold_t* cold = server_ccold(s, h);
  if (!hot || !cold)
    return;

  cold->title = cold->base_title;

  if (cold->has_net_wm_name && cold->title && cold->title[0] != '\0') {
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_VISIBLE_NAME, atoms.UTF8_STRING, 8, (uint32_t)strlen(cold->title), cold->title);
  }
  else {
    xcb_delete_property(s->conn, hot->xid, atoms._NET_WM_VISIBLE_NAME);
  }

  if (cold->has_net_wm_icon_name && cold->base_icon_name && cold->base_icon_name[0] != '\0') {
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_VISIBLE_ICON_NAME, atoms.UTF8_STRING, 8, (uint32_t)strlen(cold->base_icon_name), cold->base_icon_name);
  }
  else {
    xcb_delete_property(s->conn, hot->xid, atoms._NET_WM_VISIBLE_ICON_NAME);
  }

  hot->dirty |= DIRTY_TITLE | DIRTY_FRAME_STYLE;
}

void wm_client_toggle_maximize(server_t* s, handle_t h) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  bool want = !(hot->maximized_horz && hot->maximized_vert);
  wm_client_set_maximize(s, hot, want, want);
  LOG_INFO("Client %u maximized toggle: %d", hot->xid, want);
}

void wm_client_iconify(server_t* s, handle_t h) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot || hot->state != STATE_MAPPED)
    return;

  LOG_INFO("Iconifying client %u", hot->xid);
  TRACE_LOG("iconify h=%lx xid=%u frame=%u layer=%d", h, hot->xid, hot->frame, hot->layer);

  hot->state = STATE_UNMAPPED;
  add_ignore_unmaps(hot, 2);
  xcb_unmap_window(s->conn, hot->frame);
  stack_remove(s, h);

  if (s->focused_client == h) {
    handle_t next_h = HANDLE_INVALID;
    for (list_node_t* node = s->focus_history.next; node != &s->focus_history; node = node->next) {
      client_hot_t* cand = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));
      if (cand->state == STATE_MAPPED) {
        next_h = cand->self;
        break;
      }
    }
    wm_set_focus(s, next_h);
  }

  // Set WM_STATE to IconicState
  uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};
  xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2, state_vals);

  hot->dirty |= DIRTY_STATE;
}

void wm_client_restore(server_t* s, handle_t h) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  if (hot->state != STATE_UNMAPPED) {
    LOG_WARN(
        "wm_client_restore called for client %u in state %d (not "
        "UNMAPPED), ignoring",
        hot->xid, hot->state);
    return;
  }

  LOG_INFO("Restoring client %u", hot->xid);
  TRACE_LOG("restore h=%lx xid=%u frame=%u layer=%d", h, hot->xid, hot->frame, hot->layer);

  hot->state = STATE_MAPPED;
  xcb_map_window(s->conn, hot->xid);
  xcb_map_window(s->conn, hot->frame);

  uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
  xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2, state_vals);

  hot->dirty |= DIRTY_STATE;
  stack_raise(s, h);
}
