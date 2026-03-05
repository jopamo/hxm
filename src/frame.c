/* src/frame.c
 * Window frame decoration and management
 */

#include "frame.h"

#include <X11/cursorfont.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "hxm.h"
#include "render.h"
#include "wm_internal.h"

void frame_init_resources(server_t* s) {
  // Cursors
  xcb_font_t cursor_font = xcb_generate_id(s->conn);
  xcb_open_font(s->conn, cursor_font, strlen("cursor"), "cursor");

  struct {
    xcb_cursor_t* cursor;
    uint16_t id;
  } cursors[] = {
      {&s->cursor_left_ptr, XC_left_ptr},
      {&s->cursor_move, XC_fleur},
      {&s->cursor_resize_top, XC_top_side},
      {&s->cursor_resize_bottom, XC_bottom_side},
      {&s->cursor_resize_left, XC_left_side},
      {&s->cursor_resize_right, XC_right_side},
      {&s->cursor_resize_top_left, XC_top_left_corner},
      {&s->cursor_resize_top_right, XC_top_right_corner},
      {&s->cursor_resize_bottom_left, XC_bottom_left_corner},
      {&s->cursor_resize_bottom_right, XC_bottom_right_corner},
  };

  for (size_t i = 0; i < sizeof(cursors) / sizeof(cursors[0]); i++) {
    *cursors[i].cursor = xcb_generate_id(s->conn);
    xcb_create_glyph_cursor(s->conn, *cursors[i].cursor, cursor_font, cursor_font, cursors[i].id, cursors[i].id + 1, 0, 0, 0, 0xffff, 0xffff, 0xffff);
  }

  xcb_close_font(s->conn, cursor_font);
}

void frame_cleanup_resources(server_t* s) {
  if (!s->conn)
    return;

  if (s->cursor_left_ptr != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_left_ptr);
  if (s->cursor_move != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_move);
  if (s->cursor_resize_top != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_resize_top);
  if (s->cursor_resize_bottom != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_resize_bottom);
  if (s->cursor_resize_left != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_resize_left);
  if (s->cursor_resize_right != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_resize_right);
  if (s->cursor_resize_top_left != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_resize_top_left);
  if (s->cursor_resize_top_right != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_resize_top_right);
  if (s->cursor_resize_bottom_left != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_resize_bottom_left);
  if (s->cursor_resize_bottom_right != XCB_NONE)
    xcb_free_cursor(s->conn, s->cursor_resize_bottom_right);

  s->cursor_left_ptr = XCB_NONE;
  s->cursor_move = XCB_NONE;
  s->cursor_resize_top = XCB_NONE;
  s->cursor_resize_bottom = XCB_NONE;
  s->cursor_resize_left = XCB_NONE;
  s->cursor_resize_right = XCB_NONE;
  s->cursor_resize_top_left = XCB_NONE;
  s->cursor_resize_top_right = XCB_NONE;
  s->cursor_resize_bottom_left = XCB_NONE;
  s->cursor_resize_bottom_right = XCB_NONE;
}

#define BUTTON_WIDTH 16
#define BUTTON_HEIGHT 16
#define BUTTON_PADDING 4

static bool frame_render_disabled(void) {
  static int cached = -1;
  if (cached >= 0)
    return cached != 0;

  const char* v = getenv("HXM_DISABLE_FRAME_RENDER");
  if (!v || v[0] == '\0' || (v[0] == '0' && v[1] == '\0')) {
    cached = 0;
  }
  else {
    cached = 1;
  }
  return cached != 0;
}

static xcb_rectangle_t get_button_rect(server_t* s, client_hot_t* hot, frame_button_t btn) {
  uint32_t frame_w_calc = (uint32_t)hot->server.w + (uint32_t)(2u * (uint32_t)s->config.theme.border_width);
  if (frame_w_calc > MAX_FRAME_SIZE)
    frame_w_calc = MAX_FRAME_SIZE;
  uint16_t frame_w = (uint16_t)frame_w_calc;
  // Buttons are laid out inside the right border
  int32_t x_calc = (int32_t)frame_w - (int32_t)s->config.theme.border_width - BUTTON_PADDING - BUTTON_WIDTH;
  if (x_calc < INT16_MIN)
    x_calc = INT16_MIN;
  if (x_calc > INT16_MAX)
    x_calc = INT16_MAX;
  int16_t x = (int16_t)x_calc;
  int16_t y = (int16_t)((s->config.theme.title_height - BUTTON_HEIGHT) / 2);

  if (btn == FRAME_BUTTON_MAXIMIZE)
    x -= (BUTTON_WIDTH + BUTTON_PADDING);
  if (btn == FRAME_BUTTON_MINIMIZE)
    x -= 2 * (BUTTON_WIDTH + BUTTON_PADDING);

  return (xcb_rectangle_t){x, y, BUTTON_WIDTH, BUTTON_HEIGHT};
}

void frame_redraw(server_t* s, handle_t h, uint32_t what) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  if (what & FRAME_REDRAW_ALL) {
    hot->dirty |= DIRTY_FRAME_ALL;
  }
  else {
    if (what & FRAME_REDRAW_TITLE)
      hot->dirty |= DIRTY_FRAME_TITLE;
    if (what & FRAME_REDRAW_BORDER)
      hot->dirty |= DIRTY_FRAME_BORDER;
    if (what & FRAME_REDRAW_BUTTONS)
      hot->dirty |= DIRTY_FRAME_BUTTONS;
  }
}

void frame_redraw_region(server_t* s, handle_t h, const dirty_region_t* dirty) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot)
    return;

  if (dirty && dirty->valid) {
    dirty_region_union(&hot->frame_damage, dirty);
    // Let frame_flush use frame_damage as the clip region
  }
  else {
    hot->dirty |= DIRTY_FRAME_ALL;
  }
}

/*
 * frame_flush:
 * Render the frame decorations to the X server.
 *
 * Optimization:
 * Instead of redrawing the entire frame every time, we compute a "dirty region"
 * (clip_ptr). This allows us to only upload pixels for the title bar or a
 * single button if that's all that changed, significantly reducing bandwidth
 * and CPU usage.
 */
void frame_flush(server_t* s, handle_t h) {
  assert(s->in_commit_phase);
  client_hot_t* hot = server_chot(s, h);
  client_cold_t* cold = server_ccold(s, h);
  if (!hot || !cold)
    return;

  const uint32_t frame_dirty_mask = DIRTY_FRAME_ALL | DIRTY_FRAME_TITLE | DIRTY_FRAME_BUTTONS | DIRTY_FRAME_BORDER | DIRTY_TITLE | DIRTY_FRAME_STYLE;
  uint32_t f_dirty = hot->dirty & frame_dirty_mask;

  if (!f_dirty && !hot->frame_damage.valid)
    return;

  if (frame_render_disabled()) {
    hot->dirty &= ~frame_dirty_mask;
    dirty_region_reset(&hot->frame_damage);
    return;
  }

  if (hot->flags & CLIENT_FLAG_UNDECORATED) {
    hot->dirty &= ~frame_dirty_mask;
    dirty_region_reset(&hot->frame_damage);
    return;
  }

  bool active = (hot->flags & CLIENT_FLAG_FOCUSED);

  uint16_t bw = s->config.theme.border_width;
  uint16_t th = s->config.theme.title_height;
  uint16_t bottom_h = bw;

  uint32_t frame_w_calc = (uint32_t)hot->server.w + (uint32_t)(2u * bw);
  uint32_t frame_h_calc = (uint32_t)hot->server.h + (uint32_t)th + (uint32_t)bottom_h;
  if (frame_w_calc > MAX_FRAME_SIZE)
    frame_w_calc = MAX_FRAME_SIZE;
  if (frame_h_calc > MAX_FRAME_SIZE)
    frame_h_calc = MAX_FRAME_SIZE;
  uint16_t frame_w = (uint16_t)frame_w_calc;
  uint16_t frame_h = (uint16_t)frame_h_calc;

  // Frames are always created with the root visual/depth
  xcb_visualtype_t* visual = s->root_visual_type;
  if (!visual) {
    LOG_WARN("No root visual found, skipping redraw for client %u", hot->xid);
    return;
  }

  const dirty_region_t* clip_ptr = NULL;
  dirty_region_t partial_clip = {0};

  if (!(hot->dirty & (DIRTY_FRAME_ALL | DIRTY_FRAME_STYLE))) {
    if (hot->frame_damage.valid) {
      clip_ptr = &hot->frame_damage;
    }
    else if (hot->dirty & (DIRTY_FRAME_TITLE | DIRTY_TITLE)) {
      partial_clip = dirty_region_make(0, 0, frame_w, (uint16_t)s->config.theme.title_height);
      clip_ptr = &partial_clip;
    }
    else if (hot->dirty & DIRTY_FRAME_BUTTONS) {
      // Button area is roughly right side of titlebar
      uint16_t btn_area_w = (uint16_t)(3 * (BUTTON_WIDTH + BUTTON_PADDING) + BUTTON_PADDING);
      // Fix: Include border width in the clip region calculation
      // Buttons are positioned relative to the right edge minus border
      // Clip region is in frame coordinates (0,0 is top-left of frame)
      int32_t clip_x_calc = (int32_t)frame_w - (int32_t)btn_area_w - (int32_t)s->config.theme.border_width;
      if (clip_x_calc < 0)
        clip_x_calc = 0;
      if (clip_x_calc > INT16_MAX)
        clip_x_calc = INT16_MAX;
      int16_t clip_x = (int16_t)clip_x_calc;

      uint32_t clip_w_calc = (uint32_t)btn_area_w + (uint32_t)s->config.theme.border_width;
      if (clip_w_calc > UINT16_MAX)
        clip_w_calc = UINT16_MAX;

      uint32_t clip_h_calc = s->config.theme.title_height;
      if (clip_h_calc > UINT16_MAX)
        clip_h_calc = UINT16_MAX;

      partial_clip = dirty_region_make(clip_x, 0, (uint16_t)clip_w_calc, (uint16_t)clip_h_calc);
      clip_ptr = &partial_clip;
    }
  }

  render_frame(s->conn, hot->frame, visual, &cold->render_ctx, (int)s->root_depth, s->is_test, cold->title ? cold->title : "", active, frame_w, frame_h,
               &s->config.theme, cold->icon_surface ? cold->icon_surface : s->default_icon, clip_ptr);

  hot->dirty &= ~frame_dirty_mask;
  dirty_region_reset(&hot->frame_damage);
}

frame_button_t frame_get_button_at(server_t* s, handle_t h, int16_t x, int16_t y) {
  client_hot_t* hot = server_chot(s, h);
  if (!hot || (hot->flags & CLIENT_FLAG_UNDECORATED))
    return FRAME_BUTTON_NONE;

  frame_button_t btns[] = {FRAME_BUTTON_CLOSE, FRAME_BUTTON_MAXIMIZE, FRAME_BUTTON_MINIMIZE};
  for (int i = 0; i < 3; i++) {
    xcb_rectangle_t br = get_button_rect(s, hot, btns[i]);
    if (x >= br.x && x < br.x + br.width && y >= br.y && y < br.y + br.height) {
      return btns[i];
    }
  }

  return FRAME_BUTTON_NONE;
}
