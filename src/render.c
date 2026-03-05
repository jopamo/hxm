/* src/render.c
 * Rendering primitives and color management
 *
 * This module uses Cairo/Pango to draw window decorations.
 * It keeps a persistent cairo surface/context per frame to avoid
 * per-paint allocation churn.
 */

#include "render.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Rendering is expected to run on the WM thread
// XCB/cairo_xcb are not safely usable from multiple threads on one connection

// Helper to convert hex uint32 to RGBA
static rgba_t u32_to_rgba(uint32_t c) {
  return (rgba_t){((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, 1.0};
}

static inline bool cairo_surface_ok(cairo_surface_t* s) {
  return s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS;
}

static inline bool cairo_ctx_ok(cairo_t* cr) {
  return cr && cairo_status(cr) == CAIRO_STATUS_SUCCESS;
}

static bool get_image_wh(cairo_surface_t* s, int* out_w, int* out_h) {
  if (!s)
    return false;
  if (cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE)
    return false;
  *out_w = cairo_image_surface_get_width(s);
  *out_h = cairo_image_surface_get_height(s);
  return (*out_w > 0 && *out_h > 0);
}

static void draw_bevel(cairo_t* cr, int w, int h, bool raised, bool bevel2) {
  double alpha_light = 0.4;
  double alpha_dark = 0.4;

  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);

  if (raised) {
    // Light on top and left
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha_light);
    cairo_move_to(cr, 0, h);
    cairo_line_to(cr, 0, 0);
    cairo_line_to(cr, w, 0);
    cairo_stroke(cr);

    // Dark on bottom and right
    cairo_set_source_rgba(cr, 0, 0, 0, alpha_dark);
    cairo_move_to(cr, 0, h - 0.5);
    cairo_line_to(cr, w - 0.5, h - 0.5);
    cairo_line_to(cr, w - 0.5, 0);
    cairo_stroke(cr);

    if (bevel2) {
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha_light * 0.5);
      cairo_move_to(cr, 1.5, h - 2);
      cairo_line_to(cr, 1.5, 1.5);
      cairo_line_to(cr, w - 2, 1.5);
      cairo_stroke(cr);

      cairo_set_source_rgba(cr, 0, 0, 0, alpha_dark * 0.5);
      cairo_move_to(cr, 1, h - 1.5);
      cairo_line_to(cr, w - 1.5, h - 1.5);
      cairo_line_to(cr, w - 1.5, 1);
      cairo_stroke(cr);
    }
  }
  else {
    // Sunken: dark on top and left
    cairo_set_source_rgba(cr, 0, 0, 0, alpha_dark);
    cairo_move_to(cr, 0, h);
    cairo_line_to(cr, 0, 0);
    cairo_line_to(cr, w, 0);
    cairo_stroke(cr);

    // Light on bottom and right
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha_light);
    cairo_move_to(cr, 0, h - 0.5);
    cairo_line_to(cr, w - 0.5, h - 0.5);
    cairo_line_to(cr, w - 0.5, 0);
    cairo_stroke(cr);
  }

  cairo_restore(cr);
}

static void draw_appearance(cairo_t* cr, int w, int h, appearance_t* app) {
  if (app->flags & BG_GRADIENT) {
    cairo_pattern_t* pat;
    if (app->flags & BG_HORIZONTAL) {
      pat = cairo_pattern_create_linear(0, 0, w, 0);
    }
    else if (app->flags & BG_DIAGONAL) {
      pat = cairo_pattern_create_linear(0, 0, w, h);
    }
    else if (app->flags & BG_CROSSDIAGONAL) {
      pat = cairo_pattern_create_linear(w, 0, 0, h);
    }
    else {  // Vertical or default
      pat = cairo_pattern_create_linear(0, 0, 0, h);
    }

    rgba_t c1 = u32_to_rgba(app->color);
    rgba_t c2 = u32_to_rgba(app->color_to);
    cairo_pattern_add_color_stop_rgba(pat, 0, c1.r, c1.g, c1.b, c1.a);
    cairo_pattern_add_color_stop_rgba(pat, 1, c2.r, c2.g, c2.b, c2.a);

    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
  }
  else {
    rgba_t c = u32_to_rgba(app->color);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_paint(cr);
  }

  if (app->flags & BG_RAISED) {
    draw_bevel(cr, w, h, true, (app->flags & BG_BEVEL2));
  }
  else if (app->flags & BG_SUNKEN) {
    draw_bevel(cr, w, h, false, (app->flags & BG_BEVEL2));
  }
}

static void render_reset_surface(render_context_t* ctx) {
  if (ctx->cr) {
    cairo_destroy(ctx->cr);
    ctx->cr = NULL;
  }
  if (ctx->surface) {
    cairo_surface_destroy(ctx->surface);
    ctx->surface = NULL;
  }
  ctx->target = XCB_WINDOW_NONE;
  ctx->visual_id = 0;
  ctx->depth = 0;
  ctx->surface_is_test = false;
  ctx->width = 0;
  ctx->height = 0;
}

static bool render_prepare_surface(xcb_connection_t* conn, xcb_window_t win, xcb_visualtype_t* visual, render_context_t* ctx, int depth, int w, int h, bool is_test) {
  bool recreate = false;

  if (!ctx->surface || !ctx->cr)
    recreate = true;

  if (ctx->surface_is_test != is_test || ctx->target != win || ctx->width != w || ctx->height != h || ctx->depth != depth) {
    recreate = true;
  }

  if (!is_test) {
    if (!visual)
      return false;
    if (ctx->visual_id != visual->visual_id)
      recreate = true;
  }

  if (!recreate)
    return true;

  render_reset_surface(ctx);

  cairo_surface_t* surface = NULL;
  if (is_test) {
    surface = cairo_image_surface_create((depth == 32) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, w, h);
  }
  else {
    surface = cairo_xcb_surface_create(conn, win, visual, w, h);
  }

  if (!cairo_surface_ok(surface)) {
    if (surface)
      cairo_surface_destroy(surface);
    return false;
  }

  cairo_t* cr = cairo_create(surface);
  if (!cairo_ctx_ok(cr)) {
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return false;
  }

  ctx->surface = surface;
  ctx->cr = cr;
  ctx->target = win;
  ctx->visual_id = (!is_test && visual) ? visual->visual_id : 0;
  ctx->depth = depth;
  ctx->surface_is_test = is_test;
  ctx->width = w;
  ctx->height = h;
  return true;
}

static void render_invalidate_title_cache(render_context_t* ctx) {
  if (ctx->title_surface) {
    cairo_surface_destroy(ctx->title_surface);
    ctx->title_surface = NULL;
  }
  ctx->last_title_width = -1;
  ctx->last_title_height = -1;
  ctx->last_title_color = 0xFFFFFFFFu;
}

void render_init(render_context_t* ctx) {
  ctx->surface = NULL;
  ctx->cr = NULL;
  ctx->layout = NULL;
  ctx->title_surface = NULL;
  ctx->target = XCB_WINDOW_NONE;
  ctx->visual_id = 0;
  ctx->depth = 0;
  ctx->surface_is_test = false;
  ctx->width = 0;
  ctx->height = 0;
  ctx->last_title = NULL;
  ctx->last_title_width = -1;
  ctx->last_title_height = -1;
  ctx->last_title_color = 0xFFFFFFFFu;
}

void render_free(render_context_t* ctx) {
  if (ctx->layout) {
    g_object_unref(ctx->layout);
    ctx->layout = NULL;
  }
  render_reset_surface(ctx);
  if (ctx->last_title) {
    free(ctx->last_title);
    ctx->last_title = NULL;
  }
  render_invalidate_title_cache(ctx);
}

static void ensure_layout(render_context_t* ctx) {
  if (ctx->layout)
    return;

  // We need a dummy surface to create a layout if one doesn't exist
  cairo_surface_t* dummy = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t* cr = cairo_create(dummy);
  ctx->layout = pango_cairo_create_layout(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(dummy);

  PangoFontDescription* desc = pango_font_description_from_string("Sans Bold 10");
  pango_layout_set_font_description(ctx->layout, desc);
  pango_font_description_free(desc);
}

static bool render_update_title_cache(render_context_t* ctx, const char* title, int title_text_width, int title_h, rgba_t text, uint32_t text_color_u32) {
  if (!ctx)
    return false;

  if (!title)
    title = "";

  ensure_layout(ctx);

  bool title_changed = (!ctx->last_title || strcmp(ctx->last_title, title) != 0);
  if (title_changed) {
    pango_layout_set_text(ctx->layout, title, -1);
    if (ctx->last_title)
      free(ctx->last_title);
    ctx->last_title = strdup(title);
  }

  if (title[0] == '\0' || title_text_width <= 0 || title_h <= 0) {
    render_invalidate_title_cache(ctx);
    ctx->last_title_width = title_text_width;
    ctx->last_title_height = title_h;
    ctx->last_title_color = text_color_u32;
    return false;
  }

  bool width_changed = (ctx->last_title_width != title_text_width);
  if (width_changed) {
    pango_layout_set_width(ctx->layout, title_text_width * PANGO_SCALE);
    pango_layout_set_ellipsize(ctx->layout, PANGO_ELLIPSIZE_END);
  }

  bool height_changed = (ctx->last_title_height != title_h);
  bool color_changed = (ctx->last_title_color != text_color_u32);
  bool cache_miss = (ctx->title_surface == NULL);

  if (cache_miss || title_changed || width_changed || height_changed || color_changed) {
    if (ctx->title_surface) {
      cairo_surface_destroy(ctx->title_surface);
      ctx->title_surface = NULL;
    }

    cairo_surface_t* title_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, title_text_width, title_h);
    if (!cairo_surface_ok(title_surface)) {
      if (title_surface)
        cairo_surface_destroy(title_surface);
      ctx->last_title_width = title_text_width;
      ctx->last_title_height = title_h;
      ctx->last_title_color = text_color_u32;
      return false;
    }

    cairo_t* title_cr = cairo_create(title_surface);
    if (!cairo_ctx_ok(title_cr)) {
      cairo_destroy(title_cr);
      cairo_surface_destroy(title_surface);
      ctx->last_title_width = title_text_width;
      ctx->last_title_height = title_h;
      ctx->last_title_color = text_color_u32;
      return false;
    }

    cairo_set_operator(title_cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(title_cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(title_cr);
    cairo_set_operator(title_cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(title_cr, text.r, text.g, text.b, text.a);

    int text_h = 0;
    pango_layout_get_pixel_size(ctx->layout, NULL, &text_h);
    double text_y = (title_h - text_h) / 2.0;
    cairo_move_to(title_cr, 0.0, text_y);
    pango_cairo_update_layout(title_cr, ctx->layout);
    pango_cairo_show_layout(title_cr, ctx->layout);

    cairo_destroy(title_cr);
    cairo_surface_flush(title_surface);
    ctx->title_surface = title_surface;
  }

  ctx->last_title_width = title_text_width;
  ctx->last_title_height = title_h;
  ctx->last_title_color = text_color_u32;
  return ctx->title_surface != NULL;
}

static void draw_button(cairo_t* cr, int x, int y, int w, int h, const char* type, rgba_t color) {
  cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
  cairo_set_line_width(cr, 1.0);

  // Pixel-snap offset for crisp lines
  double ox = x + 0.5;
  double oy = y + 0.5;

  // Button Border
  cairo_rectangle(cr, ox, oy, (double)w - 1, (double)h - 1);
  cairo_stroke(cr);

  // Icon Glyphs (High fidelity)
  if (strcmp(type, "close") == 0) {
    cairo_move_to(cr, ox + 4, oy + 4);
    cairo_line_to(cr, ox + w - 4, oy + h - 4);
    cairo_move_to(cr, ox + w - 4, oy + 4);
    cairo_line_to(cr, ox + 4, oy + h - 4);
    cairo_stroke(cr);
  }
  else if (strcmp(type, "max") == 0) {
    cairo_rectangle(cr, ox + 4, oy + 4, (double)w - 8, (double)h - 8);
    cairo_stroke(cr);
  }
  else if (strcmp(type, "min") == 0) {
    cairo_move_to(cr, ox + 4, oy + h - 6);
    cairo_line_to(cr, ox + w - 4, oy + h - 6);
    cairo_stroke(cr);
  }
}

/*
 * render_frame:
 * Paint the window frame.
 *
 * Pipeline:
 * 1. Reuse or (re)create a persistent Cairo surface/context.
 * 2. Apply clip region (if partial redraw).
 * 3. Draw background, title, buttons.
 * 4. Flush Cairo surface.
 * 5. In tests, upload image data via xcb_put_image for assertions.
 *
 * Note: PangoLayout is reused from `ctx` to save font lookup time.
 */
void render_frame(xcb_connection_t* conn,
                  xcb_window_t win,
                  xcb_visualtype_t* visual,
                  render_context_t* ctx,
                  int depth,
                  bool is_test,
                  const char* title,
                  bool active,
                  int w,
                  int h,
                  theme_t* theme,
                  cairo_surface_t* icon,
                  const dirty_region_t* dirty) {
  if (w <= 0 || h <= 0)
    return;
  if (!ctx)
    return;
  if (!is_test && !visual)
    return;
  if (!render_prepare_surface(conn, win, visual, ctx, depth, w, h, is_test))
    return;

  cairo_surface_t* target_surface = ctx->surface;
  cairo_t* cr = ctx->cr;
  cairo_save(cr);
  cairo_identity_matrix(cr);
  cairo_reset_clip(cr);

  bool use_clip = false;
  dirty_region_t clip = {0};
  if (dirty && dirty->valid) {
    clip = *dirty;
    // Validate clip before clamp to avoid pixman asserts if coords are bogus
    if (clip.w > 0 && clip.h > 0) {
      dirty_region_clamp(&clip, 0, 0, (uint16_t)w, (uint16_t)h);
      if (!clip.valid) {
        cairo_restore(cr);
        return;
      }
      cairo_rectangle(cr, (double)clip.x, (double)clip.y, (double)clip.w, (double)clip.h);
      cairo_clip(cr);
      use_clip = true;
    }
  }

  // Map State
  appearance_t* title_bg = active ? &theme->window_active_title : &theme->window_inactive_title;
  uint32_t border_color_u32 = active ? theme->window_active_border_color : theme->window_inactive_border_color;
  uint32_t text_color_u32 = active ? theme->window_active_label_text_color : theme->window_inactive_label_text_color;

  rgba_t border = u32_to_rgba(border_color_u32);
  rgba_t text = u32_to_rgba(text_color_u32);

  int title_h = (int)theme->title_height;
  int border_w = (int)theme->border_width;
  bool clip_hits_title = true;
  if (use_clip) {
    int32_t cy0 = (int32_t)clip.y;
    int32_t cy1 = cy0 + (int32_t)clip.h;
    clip_hits_title = (cy0 < title_h && cy1 > 0);
  }

  // 1. Draw Background (Solid for the whole frame, title_bg for titlebar)
  cairo_set_source_rgba(cr, border.r, border.g, border.b, border.a);
  cairo_paint(cr);

  if (clip_hits_title) {
    // Draw Titlebar background only when clip intersects the title area.
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, w, title_h);
    cairo_clip(cr);
    draw_appearance(cr, w, title_h, title_bg);
    cairo_restore(cr);
  }

  int title_x_offset = border_w + 6;

  // 2. Draw Icon
  if (clip_hits_title && icon) {
    int icon_w, icon_h;
    if (get_image_wh(icon, &icon_w, &icon_h)) {
      double target_size = title_h - 4;
      if (target_size > 16)
        target_size = 16;
      if (target_size < 8)
        target_size = 8;
      double scale = target_size / ((icon_w > icon_h) ? icon_w : icon_h);
      double draw_w = icon_w * scale;
      double draw_h = icon_h * scale;
      double icon_y = (title_h - draw_h) / 2.0;
      cairo_save(cr);
      cairo_translate(cr, title_x_offset, icon_y);
      cairo_scale(cr, scale, scale);
      cairo_set_source_surface(cr, icon, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
      title_x_offset += (int)draw_w + 6;
    }
  }

  // Button dimensions (used for title text clipping)
  int btn_size = 16;
  int btn_pad = 4;
  int total_button_width = 3 * btn_size + 4 * btn_pad;
  int leftmost_button_x = w - border_w - total_button_width;
  // Ensure leftmost button stays within frame
  if (leftmost_button_x < border_w) {
    leftmost_button_x = border_w;
  }
  // Available width for title text: from title_x_offset to leftmost_button_x
  // minus padding
  int title_text_width = leftmost_button_x - title_x_offset - btn_pad;
  if (title_text_width < 0)
    title_text_width = 0;

  // 3. Draw Title Text
  if (clip_hits_title && title && title[0] != '\0') {
    if (render_update_title_cache(ctx, title, title_text_width, title_h, text, text_color_u32)) {
      cairo_set_source_surface(cr, ctx->title_surface, (double)title_x_offset, 0.0);
      cairo_paint(cr);
    }
  }

  // 4. Draw Borders
  cairo_set_source_rgba(cr, border.r, border.g, border.b, border.a);
  if (border_w > 0 && border_w * 2 < w && border_w * 2 < h) {
    cairo_set_line_width(cr, (double)border_w);
    cairo_rectangle(cr, (double)border_w / 2.0, (double)border_w / 2.0, (double)w - border_w, (double)h - border_w);
    cairo_stroke(cr);
  }

  if (clip_hits_title) {
    // 5. Draw titlebar separator
    cairo_set_source_rgba(cr, border.r, border.g, border.b, border.a);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, (double)title_h - 0.5);
    cairo_line_to(cr, (double)w, (double)title_h - 0.5);
    cairo_stroke(cr);

    // 6. Draw Buttons
    // btn_size and btn_pad already defined above
    int btn_y = (title_h - btn_size) / 2;
    int btn_x = w - border_w - btn_pad - btn_size;
    // Ensure buttons stay within frame
    if (btn_x < border_w) {
      btn_x = border_w;
    }
    // Ensure leftmost button doesn't go beyond left border
    int leftmost_x = btn_x - 2 * (btn_size + btn_pad);
    if (leftmost_x < border_w) {
      btn_x += border_w - leftmost_x;
      leftmost_x = border_w;
    }
    draw_button(cr, btn_x, btn_y, btn_size, btn_size, "close", text);
    btn_x -= (btn_size + btn_pad);
    draw_button(cr, btn_x, btn_y, btn_size, btn_size, "max", text);
    btn_x -= (btn_size + btn_pad);
    draw_button(cr, btn_x, btn_y, btn_size, btn_size, "min", text);
  }

  // 6. Present
  cairo_surface_flush(target_surface);
  cairo_restore(cr);

  if (is_test) {
    xcb_gcontext_t gc = xcb_generate_id(conn);
    uint32_t mask = XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values[] = {0};
    xcb_create_gc(conn, gc, win, mask, values);
    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, win, gc, (uint16_t)w, (uint16_t)h, 0, 0, 0, (uint8_t)depth, (uint32_t)(cairo_image_surface_get_stride(target_surface) * h),
                  cairo_image_surface_get_data(target_surface));
    xcb_free_gc(conn, gc);
  }
}
