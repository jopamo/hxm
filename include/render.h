#pragma once
#include <cairo/cairo-xcb.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <xcb/xcb.h>

#include "hxm.h"
#include "theme.h"

// Holds persistent resources to prevent allocation churn
typedef struct {
    cairo_surface_t* surface;
    cairo_t* cr;
    PangoLayout* layout;
    int width;
    int height;
} render_context_t;

// Simple color struct for the interface
typedef struct {
    double r, g, b, a;
} rgba_t;

// Initialize/Free
void render_init(render_context_t* ctx);
void render_free(render_context_t* ctx);

// The Main Paint Function
void render_frame(xcb_connection_t* conn, xcb_window_t win, xcb_visualtype_t* visual, render_context_t* ctx, int depth,
                  bool is_test, const char* title, bool active,
                  int width,   // Total frame width
                  int height,  // Total frame height
                  theme_t* theme, cairo_surface_t* icon, const dirty_region_t* dirty);
