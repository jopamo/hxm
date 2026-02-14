/* snap_preview.c - Preview window management */

#include "snap_preview.h"

#include <stdlib.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>

#include "hxm.h"

static void snap_preview_make_clickthrough(server_t* s) {
  const xcb_query_extension_reply_t* ext = xcb_get_extension_data(s->conn, &xcb_shape_id);
  if (!ext || !ext->present)
    return;

  // empty input region so pointer events pass through
  xcb_shape_rectangles(s->conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED, s->snap_preview_win, 0, 0, 0, NULL);
}

void snap_preview_init(server_t* s) {
  if (!s || !s->conn)
    return;
  if (s->snap_preview_win != XCB_WINDOW_NONE)
    return;

  s->snap_preview_win = xcb_generate_id(s->conn);

  uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
  uint32_t values[3];
  values[0] = 0; /* background pixel */

  uint32_t border = s->snap_preview_color;
  if (s->root_depth != 32)
    border &= 0x00FFFFFFu;
  values[1] = border; /* border pixel */

  values[2] = 1; /* override_redirect */

  xcb_create_window(s->conn, s->root_depth, s->snap_preview_win, s->root, 0, 0, 1, 1, s->snap_preview_border_px, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, mask, values);

  snap_preview_make_clickthrough(s);

  s->snap_preview_mapped = false;
}

void snap_preview_destroy(server_t* s) {
  if (!s || !s->conn)
    return;
  if (s->snap_preview_win != XCB_WINDOW_NONE) {
    xcb_destroy_window(s->conn, s->snap_preview_win);
    s->snap_preview_win = XCB_WINDOW_NONE;
    s->snap_preview_mapped = false;
  }
}

void snap_preview_apply(server_t* s, const rect_t* rect, bool show) {
  if (!s || s->snap_preview_win == XCB_WINDOW_NONE)
    return;

  if (!show || !rect) {
    if (s->snap_preview_mapped) {
      xcb_unmap_window(s->conn, s->snap_preview_win);
      s->snap_preview_mapped = false;
    }
    return;
  }

  int32_t x = rect->x;
  int32_t y = rect->y;
  uint32_t w = rect->w ? rect->w : 1;
  uint32_t h = rect->h ? rect->h : 1;

  uint32_t mask = 0;
  uint32_t values[8];
  int i = 0;

  mask |= XCB_CONFIG_WINDOW_X;
  values[i++] = (uint32_t)x;
  mask |= XCB_CONFIG_WINDOW_Y;
  values[i++] = (uint32_t)y;
  mask |= XCB_CONFIG_WINDOW_WIDTH;
  values[i++] = w;
  mask |= XCB_CONFIG_WINDOW_HEIGHT;
  values[i++] = h;
  mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
  values[i++] = s->snap_preview_border_px;
  mask |= XCB_CONFIG_WINDOW_STACK_MODE;
  values[i++] = XCB_STACK_MODE_ABOVE;

  xcb_configure_window(s->conn, s->snap_preview_win, mask, values);

  // keep border color in sync if config can change while running
  uint32_t border = s->snap_preview_color;
  if (s->root_depth != 32)
    border &= 0x00FFFFFFu;
  xcb_change_window_attributes(s->conn, s->snap_preview_win, XCB_CW_BORDER_PIXEL, &border);

  if (!s->snap_preview_mapped) {
    xcb_map_window(s->conn, s->snap_preview_win);
    s->snap_preview_mapped = true;
  }
}
