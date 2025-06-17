/*
 * menu.h - Root menu and client list UI
 *
 * This module implements an override-redirect menu window used for:
 * - Root menu (typically right-click on the desktop)
 * - Client list (typically middle-click on the desktop)
 *
 * Responsibilities:
 * - Create/destroy the menu X11 window (override-redirect)
 * - Populate menu items (from config and/or current clients)
 * - Render menu using Cairo via render_context_t
 * - Handle input (pointer motion, button press/release) while menu is active
 *
 * Threading:
 * - Not thread-safe
 * - Expected to be used on the server's main thread
 *
 * Lifetime:
 * - menu_init must be called during server startup
 * - menu_destroy must be called during server shutdown
 *
 * Conventions:
 * - menu items are stored as pointers (menu_item_t*) in small_vec_t
 * - selected_index is -1 when nothing is selected
 *
 * Contracts:
 * - Callers should route relevant X events to menu_handle_* while menu.visible is true
 * - Implementations should treat invalid handles as safe no-ops
 */

#ifndef MENU_H
#define MENU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

#include "ds.h"
#include "handle.h"
#include "hxm.h"
#include "render.h"

/* Action type for menu items */
typedef enum menu_action {
    MENU_ACTION_NONE = 0,
    MENU_ACTION_EXEC,
    MENU_ACTION_RESTART,
    MENU_ACTION_EXIT,
    MENU_ACTION_RELOAD,
    MENU_ACTION_RESTORE,
    MENU_ACTION_SEPARATOR
} menu_action_t;

/* A single menu item */
typedef struct menu_item {
    char* label;
    menu_action_t action;

    /* Used by MENU_ACTION_EXEC */
    char* cmd;

    /* Used by MENU_ACTION_RESTORE */
    handle_t client;

    /* Icon support */
    char* icon_path;
    cairo_surface_t* icon_surface; /* cached, owned by the menu item */
} menu_item_t;

/* Menu instance state */
typedef struct menu {
    xcb_window_t window;

    int16_t x, y;
    uint16_t w, h;

    bool visible;
    bool is_client_list;

    int32_t selected_index; /* -1 for none */
    int32_t item_height;

    render_context_t render_ctx;

    /* Vector of menu_item_t* */
    small_vec_t items;
} menu_t;

typedef struct server server_t;

/* Resource lifecycle */
void menu_init(server_t* s);
void menu_destroy(server_t* s);

/* Show/hide */
void menu_show(server_t* s, int16_t x, int16_t y);
void menu_show_client_list(server_t* s, int16_t x, int16_t y);
void menu_hide(server_t* s);

/* Event handlers while menu is active */
void menu_handle_expose(server_t* s);
void menu_handle_expose_region(server_t* s, const dirty_region_t* dirty);
void menu_handle_pointer_motion(server_t* s, int16_t x, int16_t y);
void menu_handle_button_press(server_t* s, xcb_button_press_event_t* ev);
void menu_handle_button_release(server_t* s, xcb_button_release_event_t* ev);

/* Optional helpers for callers */
static inline bool menu_is_visible(const menu_t* m) { return m && m->visible; }

#ifdef __cplusplus
}
#endif

#endif /* MENU_H */
