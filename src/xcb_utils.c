/* src/xcb_utils.c
 * XCB utility functions and atom management
 *
 * Implements the initialization of the `atoms` global structure
 * and provides debug helpers for resolving atoms names.
 */

#include "xcb_utils.h"

#include <stdlib.h>
#include <string.h>

#include "hxm.h"

struct atoms atoms;
static xcb_connection_t* g_conn_ref = NULL;

static const char* atom_names[] = {
    "WM_PROTOCOLS",
    "WM_DELETE_WINDOW",
    "WM_TAKE_FOCUS",
    "_NET_WM_PING",
    "WM_STATE",
    "WM_CLASS",
    "WM_CLIENT_MACHINE",
    "WM_COLORMAP_WINDOWS",
    "WM_COMMAND",
    "WM_NAME",
    "WM_ICON_NAME",
    "WM_HINTS",
    "WM_NORMAL_HINTS",
    "WM_TRANSIENT_FOR",
    "WM_CHANGE_STATE",
    "_MOTIF_WM_HINTS",
    "_GTK_FRAME_EXTENTS",
    "_KDE_NET_WM_FRAME_STRUT",
    "_NET_WM_SYNC_REQUEST",

    "_NET_SUPPORTED",
    "_NET_CLIENT_LIST",
    "_NET_CLIENT_LIST_STACKING",
    "_NET_ACTIVE_WINDOW",
    "_NET_WM_NAME",
    "_NET_WM_VISIBLE_NAME",
    "_NET_WM_ICON_NAME",
    "_NET_WM_VISIBLE_ICON_NAME",
    "_NET_WM_STATE",
    "_NET_WM_WINDOW_TYPE",
    "_NET_WM_STRUT",
    "_NET_WM_STRUT_PARTIAL",
    "_NET_WORKAREA",
    "_NET_WM_PID",
    "_NET_WM_USER_TIME",
    "_NET_WM_USER_TIME_WINDOW",
    "_NET_WM_SYNC_REQUEST_COUNTER",
    "_NET_WM_ICON_GEOMETRY",
    "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_ABOVE",
    "_NET_WM_STATE_BELOW",
    "_NET_WM_STATE_STICKY",
    "_NET_WM_STATE_DEMANDS_ATTENTION",
    "_NET_WM_STATE_HIDDEN",
    "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_WM_STATE_MAXIMIZED_VERT",
    "_NET_WM_STATE_FOCUSED",
    "_NET_WM_STATE_MODAL",
    "_NET_WM_STATE_SHADED",
    "_NET_WM_STATE_SKIP_TASKBAR",
    "_NET_WM_STATE_SKIP_PAGER",
    "_NET_WM_WINDOW_TYPE_DOCK",
    "_NET_WM_WINDOW_TYPE_DIALOG",
    "_NET_WM_WINDOW_TYPE_NOTIFICATION",
    "_NET_WM_WINDOW_TYPE_NORMAL",
    "_NET_WM_WINDOW_TYPE_DESKTOP",
    "_NET_WM_WINDOW_TYPE_SPLASH",
    "_NET_WM_WINDOW_TYPE_TOOLBAR",
    "_NET_WM_WINDOW_TYPE_UTILITY",
    "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
    "_NET_WM_WINDOW_TYPE_POPUP_MENU",
    "_NET_WM_WINDOW_TYPE_TOOLTIP",
    "_NET_WM_WINDOW_TYPE_COMBO",
    "_NET_WM_WINDOW_TYPE_DND",
    "_NET_SUPPORTING_WM_CHECK",
    "_NET_DESKTOP_VIEWPORT",
    "_NET_NUMBER_OF_DESKTOPS",
    "_NET_CURRENT_DESKTOP",
    "_NET_VIRTUAL_ROOTS",
    "_NET_DESKTOP_NAMES",
    "_NET_WM_DESKTOP",
    "_NET_WM_ICON",
    "_NET_CLOSE_WINDOW",
    "_NET_DESKTOP_GEOMETRY",
    "_NET_FRAME_EXTENTS",
    "_NET_REQUEST_FRAME_EXTENTS",
    "_NET_SHOWING_DESKTOP",
    "_NET_WM_WINDOW_OPACITY",
    "_NET_WM_ALLOWED_ACTIONS",
    "_NET_WM_ACTION_MOVE",
    "_NET_WM_ACTION_RESIZE",
    "_NET_WM_ACTION_MINIMIZE",
    "_NET_WM_ACTION_SHADE",
    "_NET_WM_ACTION_STICK",
    "_NET_WM_ACTION_MAXIMIZE_HORZ",
    "_NET_WM_ACTION_MAXIMIZE_VERT",
    "_NET_WM_ACTION_FULLSCREEN",
    "_NET_WM_ACTION_CHANGE_DESKTOP",
    "_NET_WM_ACTION_CLOSE",
    "_NET_WM_ACTION_ABOVE",
    "_NET_WM_ACTION_BELOW",
    "_NET_WM_MOVERESIZE",
    "_NET_MOVERESIZE_WINDOW",
    "_NET_RESTACK_WINDOW",
    "_NET_WM_FULLSCREEN_MONITORS",
    "_NET_WM_FULL_PLACEMENT",
    "UTF8_STRING",
    "COMPOUND_TEXT",
    "WM_S0",
    "_NET_WM_BYPASS_COMPOSITOR",
};

/*
 * atom_name:
 * Resolve an Atom ID to its string name (for debug logging).
 *
 * Optimization:
 * XCB atom name lookup is a round-trip. To avoid stalling the log output,
 * we use a small thread-local MRU cache (8 entries). This is sufficient for
 * debugging where we typically see the same few atoms repeated.
 */
const char* atom_name(xcb_atom_t atom) {
  if (atom == XCB_ATOM_NONE)
    return "NONE";

  const xcb_atom_t* values = (const xcb_atom_t*)&atoms;
  size_t count = sizeof(atoms) / sizeof(xcb_atom_t);
  for (size_t i = 0; i < count; i++) {
    if (values[i] == atom)
      return atom_names[i];
  }

  // Dynamic resolution cache (Thread Local for safety)
  // We use a small MRU cache per thread to avoid locking.
  static _Thread_local struct {
    xcb_atom_t atom;
    char name[64];
  } cache[8];
  static _Thread_local int next_slot = 0;

  // Check cache
  for (int i = 0; i < 8; i++) {
    if (cache[i].atom == atom)
      return cache[i].name;
  }

  if (!g_conn_ref)
    return "unknown";

  // Resolve
  // Note: xcb_get_atom_name is thread-safe as it writes a request to the XCB
  // connection buffer. However, we rely on g_conn_ref being valid and stable
  // (set at startup).
  xcb_get_atom_name_cookie_t cookie = xcb_get_atom_name(g_conn_ref, atom);
  xcb_get_atom_name_reply_t* reply = xcb_get_atom_name_reply(g_conn_ref, cookie, NULL);

  if (reply) {
    int len = xcb_get_atom_name_name_length(reply);
    if (len > 63)
      len = 63;

    int slot = next_slot;
    next_slot = (next_slot + 1) % 8;

    cache[slot].atom = atom;
    memcpy(cache[slot].name, xcb_get_atom_name_name(reply), len);
    cache[slot].name[len] = '\0';

    free(reply);
    return cache[slot].name;
  }

  return "unknown";
}

void atoms_init(xcb_connection_t* conn) {
  xcb_intern_atom_cookie_t cookies[sizeof(atom_names) / sizeof(atom_names[0])];
  const size_t count = sizeof(atom_names) / sizeof(atom_names[0]);
  g_conn_ref = conn;

  for (size_t i = 0; i < count; i++) {
    cookies[i] = xcb_intern_atom(conn, 0, strlen(atom_names[i]), atom_names[i]);
  }

  xcb_intern_atom_reply_t* reply;
  xcb_atom_t* atom_ptr = (xcb_atom_t*)&atoms;
  for (size_t i = 0; i < count; i++) {
    reply = xcb_intern_atom_reply(conn, cookies[i], NULL);
    if (reply) {
      atom_ptr[i] = reply->atom;
      free(reply);
    }
    else {
      LOG_WARN("Failed to intern atom %s", atom_names[i]);
      atom_ptr[i] = XCB_ATOM_NONE;
    }
  }
}

void atoms_print(void) {
#if HXM_DIAG
  LOG_INFO("Cached atoms:");
  const char* const* name = atom_names;
  xcb_atom_t* atom_ptr = (xcb_atom_t*)&atoms;
  for (size_t i = 0; i < sizeof(atom_names) / sizeof(atom_names[0]); i++) {
    LOG_INFO("  %s: %u", *name, *atom_ptr);
    name++;
    atom_ptr++;
  }
#endif
}

xcb_connection_t* xcb_connect_cached(void) {
  xcb_connection_t* conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(conn)) {
    LOG_ERROR("Failed to connect to X server");
    return NULL;
  }
  atoms_init(conn);
  return conn;
}

__attribute__((weak)) xcb_visualtype_t* xcb_get_visualtype(xcb_connection_t* conn, xcb_visualid_t visual_id) {
  xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
  for (; screen_iter.rem; xcb_screen_next(&screen_iter)) {
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen_iter.data);
    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
      xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
      for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
        if (visual_id == visual_iter.data->visual_id) {
          return visual_iter.data;
        }
      }
    }
  }
  return NULL;
}
