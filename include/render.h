/*
 * render.h - Cairo/XCB rendering backend
 *
 * Responsibilities:
 * - Maintain a persistent Cairo + Pango rendering context for frame decoration drawing
 * - Render window frame decorations (border/title/buttons) onto an XCB window using cairo-xcb
 * - Support partial redraw via dirty regions (damage)
 *
 * Threading:
 * - Not thread-safe
 * - Expected to be called from the server main thread with an active XCB connection
 *
 * Lifetime:
 * - Call render_init once before first use of render_context_t
 * - Call render_free once when done
 * - Call render_context_resize as needed when target dimensions change
 *
 * Notes:
 * - The returned cairo_surface_t/cairo_t/PangoLayout are owned by render_context_t
 * - render_frame may recreate the backing surface if target/visual/depth changes
 */

#pragma once

#include <cairo/cairo-xcb.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xcb/xcb.h>

#include "hxm.h"
#include "theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent rendering state to avoid reallocation churn */
typedef struct render_context {
    cairo_surface_t* surface;
    cairo_t* cr;
    PangoLayout* layout;

    /* Cached target parameters */
    xcb_visualid_t visual_id;
    int depth;

    int width;
    int height;

    /* Caching to avoid re-layout churn */
    char* last_title;
    int last_title_width;
} render_context_t;

/* Simple color struct for interfaces and theme conversions */
typedef struct rgba {
    double r, g, b, a;
} rgba_t;

/* Initialize / Free */
void render_init(render_context_t* ctx);
void render_free(render_context_t* ctx);

/* Ensure ctx matches the target window/visual/depth/size
 * Returns false on allocation or backend failure
 */
bool render_context_ensure(xcb_connection_t* conn, xcb_window_t win, xcb_visualtype_t* visual, render_context_t* ctx,
                           int depth, int width, int height);

/* Reset common cairo state for a new paint pass
 * This does not clear the surface by itself
 */
void render_context_begin(render_context_t* ctx);

/* Optional: clear full surface to transparent */
void render_clear(render_context_t* ctx);

/* Main paint function
 * - conn/win/visual/depth define the X11 target
 * - ctx is persistent and owned by caller
 * - is_test may alter behavior (deterministic output, disable X11 flushes, etc)
 * - title/active/theme/icon define appearance
 * - dirty may be NULL to redraw entire frame
 */
void render_frame(xcb_connection_t* conn, xcb_window_t win, xcb_visualtype_t* visual, render_context_t* ctx, int depth,
                  bool is_test, const char* title, bool active, int width, int height, theme_t* theme,
                  cairo_surface_t* icon, const dirty_region_t* dirty);

/* Convenience: convert theme color (or other integer formats) to rgba_t
 * If you already store doubles, you can ignore this helper
 */
static inline rgba_t rgba_make(double r, double g, double b, double a) {
    rgba_t c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}

#ifdef __cplusplus
}
#endif
