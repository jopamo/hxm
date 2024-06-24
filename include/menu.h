#ifndef MENU_H
#define MENU_H

#include <xcb/xcb.h>

#include "bbox.h"
#include "containers.h"
#include "handle.h"
#include "render.h"

typedef enum {
    MENU_ACTION_NONE,
    MENU_ACTION_EXEC,
    MENU_ACTION_RESTART,
    MENU_ACTION_EXIT,
    MENU_ACTION_RELOAD,
    MENU_ACTION_RESTORE,
    MENU_ACTION_SEPARATOR
} menu_action_t;

typedef struct {
    char* label;
    menu_action_t action;
    char* cmd;        // For EXEC
    handle_t client;  // For RESTORE
    char* icon_path;
    cairo_surface_t* icon_surface;
} menu_item_t;

typedef struct {
    xcb_window_t window;
    int16_t x, y;
    uint16_t w, h;
    bool visible;
    bool is_client_list;  // Whether it's showing the client list vs root menu

    int32_t selected_index;  // -1 for none
    int32_t item_height;

    // Resources
    render_context_t render_ctx;

    small_vec_t items;  // vector of menu_item_t*
} menu_t;

typedef struct server server_t;

void menu_init(server_t* s);
void menu_destroy(server_t* s);

void menu_show(server_t* s, int16_t x, int16_t y);
void menu_show_client_list(server_t* s, int16_t x, int16_t y);
void menu_hide(server_t* s);

// Event handlers specific to menu when active
void menu_handle_expose(server_t* s);
void menu_handle_pointer_motion(server_t* s, int16_t x, int16_t y);
void menu_handle_button_press(server_t* s, xcb_button_press_event_t* ev);
void menu_handle_button_release(server_t* s, xcb_button_release_event_t* ev);

#endif  // MENU_H
