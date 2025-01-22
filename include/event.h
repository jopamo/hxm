#ifndef EVENT_H
#define EVENT_H

#include <sys/epoll.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "client.h"
#include "config.h"
#include "containers.h"
#include "cookie_jar.h"
#include "handle_conv.h"
#include "menu.h"
#include "slotmap.h"

#define MAX_EVENTS_PER_TICK 512

// Merged ConfigureRequest for coalescing
typedef struct pending_config {
    xcb_window_t window;
    int16_t x, y;
    uint16_t width, height;
    uint16_t border_width;
    xcb_window_t sibling;
    uint8_t stack_mode;
    uint16_t mask;
} pending_config_t;

// Event buckets for coalescing
typedef struct event_buckets {
    // MapRequest/UnmapNotify/DestroyNotify queue (order matters)
    small_vec_t map_requests;
    small_vec_t unmap_notifies;
    small_vec_t destroy_notifies;
    small_vec_t key_presses;
    small_vec_t button_events;  // Stores both press and release
    small_vec_t client_messages;

    // Expose coalesced by window with dirty region union
    hash_map_t expose_regions;  // window -> dirty_region_t

    // ConfigureRequest coalesced by window
    hash_map_t configure_requests;  // window -> pending config

    // ConfigureNotify coalesced by window
    hash_map_t configure_notifies;  // window -> xcb_configure_notify_event_t

    // DestroyNotify tracker for the tick
    hash_map_t destroyed_windows;  // window -> (void*)1

    // PropertyNotify coalesced by (window, atom)
    // We'll use a combined key: (window << 32) | atom
    hash_map_t property_notifies;

    // MotionNotify latest per window
    hash_map_t motion_notifies;  // window -> xcb_motion_notify_event_t

    // EnterNotify/LeaveNotify latest per pointer
    struct {
        xcb_enter_notify_event_t enter;
        xcb_leave_notify_event_t leave;
        bool enter_valid;
        bool leave_valid;
    } pointer_notify;

    // Damage events coalesced by drawable with dirty region union
    hash_map_t damage_regions;  // drawable -> dirty_region_t

    // Counters for this tick
    uint64_t ingested;
    uint64_t coalesced;
} event_buckets_t;

// Root dirty flags
enum {
    ROOT_DIRTY_CLIENT_LIST = 1u << 0,
    ROOT_DIRTY_ACTIVE_WINDOW = 1u << 1,
    ROOT_DIRTY_CLIENT_LIST_STACKING = 1u << 2,
    ROOT_DIRTY_WORKAREA = 1u << 3
};

extern volatile sig_atomic_t g_shutdown_pending;
extern volatile sig_atomic_t g_restart_pending;
extern volatile sig_atomic_t g_reload_pending;

// Main server state
typedef struct server {
    xcb_connection_t* conn;
    xcb_window_t root;
    xcb_visualid_t root_visual;
    xcb_visualtype_t* root_visual_type;
    uint8_t root_depth;
    xcb_colormap_t default_colormap;
    xcb_window_t supporting_wm_check;
    int xcb_fd;
    int epoll_fd;

    bool damage_supported;
    uint8_t damage_event_base;
    uint8_t damage_error_base;
    bool randr_supported;
    uint8_t randr_event_base;

    // Root dirty bits
    uint32_t root_dirty;

    // Workarea
    rect_t workarea;

    // Key symbols
    xcb_key_symbols_t* keysyms;

    // Cursors
    xcb_cursor_t cursor_left_ptr;
    xcb_cursor_t cursor_move;
    xcb_cursor_t cursor_resize_top;
    xcb_cursor_t cursor_resize_bottom;
    xcb_cursor_t cursor_resize_left;
    xcb_cursor_t cursor_resize_right;
    xcb_cursor_t cursor_resize_top_left;
    xcb_cursor_t cursor_resize_top_right;
    xcb_cursor_t cursor_resize_bottom_left;
    xcb_cursor_t cursor_resize_bottom_right;

    // Interaction state (Move/Resize)
    enum { INTERACTION_NONE, INTERACTION_MOVE, INTERACTION_RESIZE } interaction_mode;
    enum {
        RESIZE_NONE = 0,
        RESIZE_TOP = 1 << 0,
        RESIZE_BOTTOM = 1 << 1,
        RESIZE_LEFT = 1 << 2,
        RESIZE_RIGHT = 1 << 3
    } interaction_resize_dir;
    xcb_window_t interaction_window;
    int16_t interaction_start_x, interaction_start_y;
    int16_t interaction_start_w, interaction_start_h;
    int16_t interaction_pointer_x, interaction_pointer_y;

    // Per-tick arena
    struct arena tick_arena;

    // Async replies
    cookie_jar_t cookie_jar;

    // Prefetched event when checking the queue
    xcb_generic_event_t* prefetched_event;

    // Event buckets
    event_buckets_t buckets;

    // Client storage (hot/cold)
    slotmap_t clients;

    // Global maps: xid/frame -> handle
    hash_map_t window_to_client;  // xcb_window_t -> handle_t (stored via uintptr_t)
    hash_map_t frame_to_client;   // frame xid -> handle_t

    // Stacking layers (bottom -> top)
    small_vec_t layers[LAYER_COUNT];

    // Focus
    handle_t focused_client;
    xcb_window_t initial_focus;
    list_node_t focus_history;  // Global MRU for now

    // Workspaces
    uint32_t desktop_count;
    uint32_t current_desktop;
    bool showing_desktop;

    // Root Menu
    menu_t menu;

    // State
    bool running;
    bool restarting;
    int exit_code;
    bool x_poll_immediate;

    // Config
    config_t config;
    bool is_test;
} server_t;

static inline client_hot_t* server_chot(server_t* s, handle_t h) {
    if (!slotmap_live(&s->clients, h)) return NULL;
    return (client_hot_t*)slotmap_hot(&s->clients, h);
}

static inline client_cold_t* server_ccold(server_t* s, handle_t h) {
    if (!slotmap_live(&s->clients, h)) return NULL;
    return (client_cold_t*)slotmap_cold(&s->clients, h);
}

static inline handle_t server_get_client_by_window(server_t* s, xcb_window_t win) {
    void* ptr = hash_map_get(&s->window_to_client, win);
    return ptr_to_handle(ptr);
}

static inline handle_t server_get_client_by_frame(server_t* s, xcb_window_t frame) {
    void* ptr = hash_map_get(&s->frame_to_client, frame);
    return ptr_to_handle(ptr);
}

void server_init(server_t* s);
void server_run(server_t* s);
void server_cleanup(server_t* s);

// Event ingestion
void event_ingest(server_t* s, bool x_ready);
void event_process(server_t* s);

#endif  // EVENT_H
