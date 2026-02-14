#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static xcb_atom_t get_atom(xcb_connection_t* conn, const char* name) {
  xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
  xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, NULL);
  if (!reply)
    return XCB_ATOM_NONE;
  xcb_atom_t atom = reply->atom;
  free(reply);
  return atom;
}

int main(void) {
  xcb_connection_t* conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(conn)) {
    fprintf(stderr, "normal_client: unable to connect to X server\n");
    return 1;
  }

  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
  if (!screen) {
    fprintf(stderr, "normal_client: no screen\n");
    xcb_disconnect(conn);
    return 1;
  }

  xcb_window_t win = xcb_generate_id(conn);
  uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE};

  xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 40, 40, 200, 120, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

  xcb_atom_t wm_class = get_atom(conn, "WM_CLASS");
  xcb_atom_t wm_name = get_atom(conn, "WM_NAME");
  xcb_atom_t net_wm_name = get_atom(conn, "_NET_WM_NAME");
  xcb_atom_t utf8 = get_atom(conn, "UTF8_STRING");

  const char class_data[] = "hxm-normal\0HxmNormal\0";
  if (wm_class != XCB_ATOM_NONE) {
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, wm_class, XCB_ATOM_STRING, 8, (uint32_t)sizeof(class_data), class_data);
  }

  const char name_data[] = "hxm-normal";
  if (wm_name != XCB_ATOM_NONE) {
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, wm_name, XCB_ATOM_STRING, 8, (uint32_t)(sizeof(name_data) - 1), name_data);
  }
  if (net_wm_name != XCB_ATOM_NONE && utf8 != XCB_ATOM_NONE) {
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_name, utf8, 8, (uint32_t)(sizeof(name_data) - 1), name_data);
  }

  xcb_map_window(conn, win);
  xcb_flush(conn);

  printf("%u\n", win);
  fflush(stdout);

  while (1) {
    xcb_generic_event_t* e = xcb_wait_for_event(conn);
    if (!e)
      break;
    free(e);
  }

  xcb_disconnect(conn);
  return 0;
}
