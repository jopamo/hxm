/* src/render.c
 * Rendering primitives and color management
 *
 * This module uses Cairo/Pango to draw window decorations.
 * It implements a double-buffered pipeline:
 * 1. Draw to an offscreen XCB Pixmap (via cairo_xcb_surface).
 * 2. Copy the Pixmap to the Window (xcb_copy_area).
 *
 * This prevents flicker during redraws.
 */

#include "render.h"

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// Rendering is expected to run on the WM thread
// XCB/cairo_xcb are not safely usable from multiple threads on one connection

// Helper to convert hex uint32 to RGBA
static rgba_t u32_to_rgba(uint32_t c) {
    return (rgba_t){((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, 1.0};
}

static inline bool cairo_surface_ok(cairo_surface_t* s) { return s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS; }

static inline bool cairo_ctx_ok(cairo_t* cr) { return cr && cairo_status(cr) == CAIRO_STATUS_SUCCESS; }

static bool get_image_wh(cairo_surface_t* s, int* out_w, int* out_h) {
    if (!s) return false;
    if (cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE) return false;
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
    } else {
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
        } else if (app->flags & BG_DIAGONAL) {
            pat = cairo_pattern_create_linear(0, 0, w, h);
        } else if (app->flags & BG_CROSSDIAGONAL) {
            pat = cairo_pattern_create_linear(w, 0, 0, h);
        } else {  // Vertical or default
            pat = cairo_pattern_create_linear(0, 0, 0, h);
        }

        rgba_t c1 = u32_to_rgba(app->color);
        rgba_t c2 = u32_to_rgba(app->color_to);
        cairo_pattern_add_color_stop_rgba(pat, 0, c1.r, c1.g, c1.b, c1.a);
        cairo_pattern_add_color_stop_rgba(pat, 1, c2.r, c2.g, c2.b, c2.a);

        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
    } else {
        rgba_t c = u32_to_rgba(app->color);
        cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
        cairo_paint(cr);
    }

    if (app->flags & BG_RAISED) {
        draw_bevel(cr, w, h, true, (app->flags & BG_BEVEL2));
    } else if (app->flags & BG_SUNKEN) {
        draw_bevel(cr, w, h, false, (app->flags & BG_BEVEL2));
    }
}

void render_init(render_context_t* ctx) {
    ctx->surface = NULL;
    ctx->cr = NULL;
    ctx->layout = NULL;
    ctx->width = 0;
    ctx->height = 0;
    ctx->last_title = NULL;
    ctx->last_title_width = -1;
}

void render_free(render_context_t* ctx) {
    if (ctx->layout) {
        g_object_unref(ctx->layout);
        ctx->layout = NULL;
    }
    if (ctx->cr) {
        cairo_destroy(ctx->cr);
        ctx->cr = NULL;
    }
    if (ctx->surface) {
        cairo_surface_destroy(ctx->surface);
        ctx->surface = NULL;
    }
    if (ctx->last_title) {
        free(ctx->last_title);
        ctx->last_title = NULL;
    }
    ctx->width = 0;
    ctx->height = 0;
}

static void ensure_layout(render_context_t* ctx) {
    if (ctx->layout) return;

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
    } else if (strcmp(type, "max") == 0) {
        cairo_rectangle(cr, ox + 4, oy + 4, (double)w - 8, (double)h - 8);
        cairo_stroke(cr);
    } else if (strcmp(type, "min") == 0) {
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
 * 1. Create XCB Pixmap (double buffer).
 * 2. Wrap it in a Cairo surface.
 * 3. Apply clip region (if partial redraw).
 * 4. Draw background, title, buttons.
 * 5. Blit Pixmap -> Window.
 * 6. Destroy temporary resources.
 *
 * Note: PangoLayout is reused from `ctx` to save font lookup time.
 */
void render_frame(xcb_connection_t* conn, xcb_window_t win, xcb_visualtype_t* visual, render_context_t* ctx, int depth,
                  bool is_test, const char* title, bool active, int w, int h, theme_t* theme, cairo_surface_t* icon,
                  const dirty_region_t* dirty) {
    if (w <= 0 || h <= 0) return;

    ensure_layout(ctx);

    cairo_surface_t* target_surface = NULL;
    xcb_pixmap_t pixmap = XCB_NONE;

    if (is_test) {
        // Use image surface for tests to avoid XCB-Cairo dependency on dummy
        // connection
        target_surface = cairo_image_surface_create((depth == 32) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, w, h);
    } else {
        if (!visual) return;  // Safety check

        // Use XCB surface for production
        pixmap = xcb_generate_id(conn);
        xcb_create_pixmap(conn, (uint8_t)depth, pixmap, win, (uint16_t)w, (uint16_t)h);
        target_surface = cairo_xcb_surface_create(conn, pixmap, visual, w, h);
    }

    if (!cairo_surface_ok(target_surface)) {
        if (!is_test && pixmap != XCB_NONE) xcb_free_pixmap(conn, pixmap);
        if (target_surface) cairo_surface_destroy(target_surface);
        return;
    }

    cairo_t* cr = cairo_create(target_surface);
    if (!cairo_ctx_ok(cr)) {
        cairo_destroy(cr);
        cairo_surface_destroy(target_surface);
        if (!is_test && pixmap != XCB_NONE) xcb_free_pixmap(conn, pixmap);
        return;
    }

    if (dirty && dirty->valid) {
        dirty_region_t clip = *dirty;
        // Validate clip before clamp to avoid pixman asserts if coords are bogus
        if (clip.w > 0 && clip.h > 0) {
            dirty_region_clamp(&clip, 0, 0, (uint16_t)w, (uint16_t)h);
            if (!clip.valid) {
                cairo_destroy(cr);
                cairo_surface_destroy(target_surface);
                if (!is_test) {
                    xcb_free_pixmap(conn, pixmap);
                }
                return;
            }
            cairo_rectangle(cr, (double)clip.x, (double)clip.y, (double)clip.w, (double)clip.h);
            cairo_clip(cr);
        }
    }

    // Map State
    appearance_t* title_bg = active ? &theme->window_active_title : &theme->window_inactive_title;
    uint32_t border_color_u32 = active ? theme->window_active_border_color : theme->window_inactive_border_color;
    uint32_t text_color_u32 = active ? theme->window_active_label_text_color : theme->window_inactive_label_text_color;
    appearance_t* handle_bg = active ? &theme->window_active_handle : &theme->window_inactive_handle;
    appearance_t* grip_bg = active ? &theme->window_active_grip : &theme->window_inactive_grip;

    rgba_t border = u32_to_rgba(border_color_u32);
    rgba_t text = u32_to_rgba(text_color_u32);

    int title_h = (int)theme->title_height;
    int border_w = (int)theme->border_width;
    int handle_h = (int)theme->handle_height;

    // 1. Draw Background (Solid for the whole frame, title_bg for titlebar)
    cairo_set_source_rgba(cr, border.r, border.g, border.b, border.a);
    cairo_paint(cr);

    // Draw Titlebar background
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, w, title_h);
    cairo_clip(cr);
    draw_appearance(cr, w, title_h, title_bg);
    cairo_restore(cr);

    // Draw Handle and Grips if handle_h > 0
    if (handle_h > 0) {
        int handle_y = h - handle_h;
        int grip_w = handle_h * 2;  // Grips are usually wider than handle is high

        // Draw Handle background
        cairo_save(cr);
        cairo_rectangle(cr, 0, handle_y, w, handle_h);
        cairo_clip(cr);
        draw_appearance(cr, w, handle_h, handle_bg);
        cairo_restore(cr);

        // Draw Grips
        cairo_save(cr);
        cairo_rectangle(cr, 0, handle_y, grip_w, handle_h);
        cairo_clip(cr);
        draw_appearance(cr, grip_w, handle_h, grip_bg);
        cairo_restore(cr);

        cairo_save(cr);
        cairo_rectangle(cr, w - grip_w, handle_y, grip_w, handle_h);
        cairo_clip(cr);
        draw_appearance(cr, grip_w, handle_h, grip_bg);
        cairo_restore(cr);
    }

    int title_x_offset = border_w + 6;

    // 2. Draw Icon
    if (icon) {
        int icon_w, icon_h;
        if (get_image_wh(icon, &icon_w, &icon_h)) {
            double target_size = title_h - 4;
            if (target_size > 16) target_size = 16;
            if (target_size < 8) target_size = 8;
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
    // Available width for title text: from title_x_offset to leftmost_button_x minus padding
    int title_text_width = leftmost_button_x - title_x_offset - btn_pad;
    if (title_text_width < 0) title_text_width = 0;

    // 3. Draw Title Text
    if (title && title[0] != '\0') {
        cairo_set_source_rgba(cr, text.r, text.g, text.b, text.a);

        // Update layout text only if changed
        if (!ctx->last_title || strcmp(ctx->last_title, title) != 0) {
            pango_layout_set_text(ctx->layout, title, -1);
            if (ctx->last_title) free(ctx->last_title);
            ctx->last_title = strdup(title);
        }

        // Update layout width only if changed
        if (ctx->last_title_width != title_text_width) {
            pango_layout_set_width(ctx->layout, title_text_width * PANGO_SCALE);
            pango_layout_set_ellipsize(ctx->layout, PANGO_ELLIPSIZE_END);
            ctx->last_title_width = title_text_width;
        }

        int text_h;
        pango_layout_get_pixel_size(ctx->layout, NULL, &text_h);
        double text_y = (title_h - text_h) / 2.0;
        cairo_move_to(cr, title_x_offset, text_y);
        pango_cairo_update_layout(cr, ctx->layout);
        pango_cairo_show_layout(cr, ctx->layout);
    }

    // 4. Draw Borders
    cairo_set_source_rgba(cr, border.r, border.g, border.b, border.a);
    if (border_w > 0 && border_w * 2 < w && border_w * 2 < h) {
        cairo_set_line_width(cr, (double)border_w);
        cairo_rectangle(cr, (double)border_w / 2.0, (double)border_w / 2.0, (double)w - border_w, (double)h - border_w);
        cairo_stroke(cr);
    }

    // 5. Draw Handle (Bottom bar)
    // We need handle_height and handle_bg. For now we use some defaults or pass
    // them. Let's assume title_bg is passed for now, but we should pass handle_bg
    // too. Actually, I should probably update render_frame signature again to be
    // more complete.

    // For now, let's just draw the separator again with better precision
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

    // 6. Blit to Window
    cairo_destroy(cr);
    cairo_surface_flush(target_surface);

    if (is_test) {
        xcb_gcontext_t gc = xcb_generate_id(conn);
        uint32_t mask = XCB_GC_GRAPHICS_EXPOSURES;
        uint32_t values[] = {0};
        xcb_create_gc(conn, gc, win, mask, values);
        xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, win, gc, (uint16_t)w, (uint16_t)h, 0, 0, 0, (uint8_t)depth,
                      (uint32_t)(cairo_image_surface_get_stride(target_surface) * h),
                      cairo_image_surface_get_data(target_surface));
        xcb_free_gc(conn, gc);
    } else {
        cairo_surface_destroy(target_surface);
        xcb_gcontext_t gc = xcb_generate_id(conn);
        uint32_t mask = XCB_GC_GRAPHICS_EXPOSURES;
        uint32_t values[] = {0};
        xcb_create_gc(conn, gc, win, mask, values);
        xcb_copy_area(conn, pixmap, win, gc, 0, 0, 0, 0, (uint16_t)w, (uint16_t)h);
        xcb_free_gc(conn, gc);
        xcb_free_pixmap(conn, pixmap);
    }

    if (is_test) {
        cairo_surface_destroy(target_surface);
    }
}
