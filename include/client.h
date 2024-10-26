#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/damage.h>
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
    DIRTY_STRUT = 1u << 7,
    DIRTY_OPACITY = 1u << 8
} client_dirty_t;

typedef enum client_state {
    STATE_UNMANAGED = 0,
    STATE_NEW,        // Allocated, waiting for initial properties
    STATE_MAPPED,     // Fully managed and mapped
    STATE_UNMAPPED,   // Managed but unmapped (iconified/withdrawn)
    STATE_DESTROYED,  // Window destroyed
    STATE_UNMANAGING  // Currently being unmanaged
} client_state_t;

typedef enum manage_phase { MANAGE_PHASE1 = 1, MANAGE_PHASE2 = 2, MANAGE_DONE = 3 } manage_phase_t;

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

typedef enum protocol_flags {
    PROTOCOL_DELETE_WINDOW = 1u << 0,
    PROTOCOL_TAKE_FOCUS = 1u << 1,
    PROTOCOL_SYNC_REQUEST = 1u << 2,
    PROTOCOL_PING = 1u << 3
} protocol_flags_t;

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
    WINDOW_TYPE_COMBO,
    WINDOW_TYPE_DND,
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
    uint16_t saved_state_mask;
    bool saved_maximize_valid;
    bool saved_maximized_horz;
    bool saved_maximized_vert;
    int32_t stacking_index;
    int8_t stacking_layer;

    uint32_t dirty;
    uint8_t state;            // client_state_t
    uint8_t initial_state;    // From WM_HINTS
    uint8_t pending_replies;  // Count of pending startup cookies
    uint8_t ignore_unmap;

    uint8_t layer;
    uint8_t base_layer;
    bool state_above;
    bool state_below;
    uint8_t type;  // window_type_t
    bool type_from_net;
    int32_t desktop;  // -1 for ALL_DESKTOPS
    bool sticky;
    bool maximized_horz;
    bool maximized_vert;
    int8_t focus_override;  // -1: default, 0: no, 1: yes
    uint8_t placement;      // placement_policy_t
    uint16_t flags;
    bool show_desktop_hidden;
    bool motif_decorations_set;
    bool motif_undecorated;
    bool gtk_frame_extents_set;
    struct {
        uint32_t left, right, top, bottom;
    } gtk_extents;

    bool override_redirect;
    bool manage_aborted;

    handle_t transient_for;

    list_node_t transient_sibling;  // Node in parent's transients_list
    list_node_t transients_head;    // List of clients transient for this one

    list_node_t focus_node;  // Node in MRU focus history

    int last_cursor_dir;  // Cache to avoid redundant cursor updates
    render_context_t render_ctx;
    cairo_surface_t* icon_surface;

    xcb_visualid_t visual_id;
    xcb_visualtype_t* visual_type;
    uint8_t depth;
    xcb_colormap_t colormap;
    xcb_colormap_t frame_colormap;
    bool frame_colormap_owned;

    xcb_damage_damage_t damage;
    dirty_region_t damage_region;

    manage_phase_t manage_phase;

    // EWMH/ICCCM extras that some clients use for focus/sync heuristics
    uint32_t user_time;
    xcb_window_t user_time_window;
    bool sync_enabled;
    uint32_t sync_counter;

    bool icon_geometry_valid;
    rect_t icon_geometry;

    bool window_opacity_valid;
    uint32_t window_opacity;

    bool fullscreen_monitors_valid;
    uint32_t fullscreen_monitors[4];
} client_hot_t;

static inline uint8_t client_layer_from_state(const client_hot_t* hot) {
    if (hot->state_above) return LAYER_ABOVE;
    if (hot->state_below) return LAYER_BELOW;
    return hot->base_layer;
}

typedef struct client_cold {
    // Effective/composed strings used for UI
    char* title;

    // Base strings tracked from properties
    char* base_title;
    char* base_icon_name;

    char* wm_instance;
    char* wm_class;
    char* wm_client_machine;
    arena_t string_arena;

    bool has_net_wm_name;
    bool has_net_wm_icon_name;

    uint32_t protocols;
    xcb_window_t transient_for_xid;
    bool can_focus;

    strut_t strut;
    strut_t strut_partial;
    strut_t strut_full;
    bool strut_partial_active;
    bool strut_full_active;

    uint32_t pid;
} client_cold_t;

typedef struct server server_t;

void client_manage_start(server_t* s, xcb_window_t win);
void client_finish_manage(server_t* s, handle_t h);
void client_unmanage(server_t* s, handle_t h);
void client_close(server_t* s, handle_t h);
void client_constrain_size(const size_hints_t* hints, uint32_t flags, uint16_t* w, uint16_t* h);
bool should_focus_on_map(const client_hot_t* hot);
void client_setup_grabs(server_t* s, handle_t h);

#endif  // CLIENT_H
