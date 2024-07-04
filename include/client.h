#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

#include "bbox.h"
#include "containers.h"
#include "handle.h"
#include "render.h"

typedef struct rect {
    int16_t x, y;
    uint16_t w, h;
} rect_t;

typedef enum layer {
    LAYER_DESKTOP = 0,
    LAYER_BELOW,
    LAYER_NORMAL,
    LAYER_ABOVE,
    LAYER_FULLSCREEN,
    LAYER_OVERLAY,
    LAYER_COUNT
} layer_t;

typedef enum client_dirty {
    DIRTY_NONE = 0,
    DIRTY_GEOM = 1u << 0,
    DIRTY_STACK = 1u << 1,
    DIRTY_FOCUS = 1u << 2,
    DIRTY_TITLE = 1u << 3,
    DIRTY_HINTS = 1u << 4,
    DIRTY_STATE = 1u << 5,
    DIRTY_FRAME_STYLE = 1u << 6,
    DIRTY_STRUT = 1u << 7
} client_dirty_t;

typedef enum client_state {
    STATE_UNMANAGED = 0,
    STATE_NEW,        // Allocated, waiting for initial properties
    STATE_MAPPED,     // Fully managed and mapped
    STATE_UNMAPPED,   // Managed but unmapped (iconified/withdrawn)
    STATE_DESTROYED,  // Window destroyed
    STATE_UNMANAGING  // Currently being unmanaged
} client_state_t;

typedef struct size_hints {
    int32_t min_w, min_h;
    int32_t max_w, max_h;
    int32_t inc_w, inc_h;
    int32_t base_w, base_h;
    int32_t min_aspect_num, min_aspect_den;
    int32_t max_aspect_num, max_aspect_den;
} size_hints_t;

typedef struct strut {
    uint32_t left, right, top, bottom;
    uint32_t left_start_y, left_end_y;
    uint32_t right_start_y, right_end_y;
    uint32_t top_start_x, top_end_x;
    uint32_t bottom_start_x, bottom_end_x;
} strut_t;

typedef enum client_flags {
    CLIENT_FLAG_NONE = 0,
    CLIENT_FLAG_URGENT = 1u << 0,
    CLIENT_FLAG_FOCUSED = 1u << 1,
    CLIENT_FLAG_UNDECORATED = 1u << 2
} client_flags_t;

typedef enum protocol_flags { PROTOCOL_DELETE_WINDOW = 1u << 0, PROTOCOL_TAKE_FOCUS = 1u << 1 } protocol_flags_t;

typedef enum window_type {
    WINDOW_TYPE_NORMAL = 0,
    WINDOW_TYPE_DIALOG,
    WINDOW_TYPE_DOCK,
    WINDOW_TYPE_NOTIFICATION,
    WINDOW_TYPE_DESKTOP,
    WINDOW_TYPE_SPLASH,
    WINDOW_TYPE_TOOLBAR,
    WINDOW_TYPE_UTILITY,
    WINDOW_TYPE_MENU,
    WINDOW_TYPE_DROPDOWN_MENU,
    WINDOW_TYPE_POPUP_MENU,
    WINDOW_TYPE_TOOLTIP,
    WINDOW_TYPE_COUNT
} window_type_t;

typedef struct client_hot {
    handle_t self;
    xcb_window_t xid;
    xcb_window_t frame;

    rect_t server;
    rect_t desired;
    rect_t pending;
    size_hints_t hints;
    uint32_t hints_flags;
    uint32_t pending_epoch;

    rect_t saved_geom;
    rect_t saved_maximize_geom;
    uint8_t saved_layer;
    uint16_t saved_flags;
    bool saved_maximize_valid;
    bool saved_maximized_horz;
    bool saved_maximized_vert;

    strut_t strut;

    uint32_t dirty;
    uint8_t state;            // client_state_t
    uint8_t pending_replies;  // Count of pending startup cookies

    uint8_t layer;
    uint8_t type;     // window_type_t
    int32_t desktop;  // -1 for ALL_DESKTOPS
    bool sticky;
    bool maximized_horz;
    bool maximized_vert;
    int8_t focus_override;  // -1: default, 0: no, 1: yes
    uint8_t placement;      // placement_policy_t
    uint16_t flags;

    bool override_redirect;
    bool manage_aborted;

    handle_t transient_for;
    list_node_t stacking_node;

    list_node_t transient_sibling;  // Node in parent's transients_list
    list_node_t transients_head;    // List of clients transient for this one

    list_node_t focus_node;  // Node in MRU focus history

    int last_cursor_dir;  // Cache to avoid redundant cursor updates
    render_context_t render_ctx;
    cairo_surface_t* icon_surface;

    xcb_visualid_t visual_id;
    xcb_visualtype_t* visual_type;
    uint8_t depth;
} client_hot_t;

typedef struct client_cold {
    char* title;
    char* wm_instance;
    char* wm_class;
    arena_t string_arena;
    bool has_net_wm_name;

    uint32_t protocols;
    xcb_window_t transient_for_xid;
    bool can_focus;
} client_cold_t;

typedef struct server server_t;

void client_manage_start(server_t* s, xcb_window_t win);

void client_finish_manage(server_t* s, handle_t h);

void client_unmanage(server_t* s, handle_t h);

void client_close(server_t* s, handle_t h);

void client_constrain_size(const size_hints_t* hints, uint16_t* w, uint16_t* h);

bool should_focus_on_map(const client_hot_t* hot);

void client_setup_grabs(server_t* s, handle_t h);

#endif  // CLIENT_H
