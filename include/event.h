/*
 * event.h - Main event loop and server state definition
 *
 * Defines:
 * - server_t: global WM state ("world" object)
 * - event_buckets_t: coalescing storage for tick-based event processing
 * - pending_config_t: merged ConfigureRequest representation
 * - event loop entry points (init/run/cleanup, ingest/process, cookie draining)
 *
 * Tick model:
 * 1) Ingest   : poll X events + signals + timers, bucket/coalesce events
 * 2) Process  : apply logical updates from buckets to in-memory model
 * 3) Flush    : emit X requests once per tick, then xcb_flush
 *
 * Contracts:
 * - Not thread-safe, server_t is owned by the main thread
 * - No synchronous X replies in hot paths (use cookie_jar)
 * - Bounded work per tick (MAX_EVENTS_PER_TICK, COOKIE_JAR_MAX_REPLIES_PER_TICK)
 * - Memory in tick_arena is valid until the next tick (arena_reset)
 *
 * Notes:
 * - hash_map_t stores void* values; handles are stored via handle_conv.h
 * - Many small_vec_t buckets hold pointers to events allocated in tick_arena
 */

#ifndef EVENT_H
#define EVENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "client.h"
#include "config.h"
#include "cookie_jar.h"
#include "ds.h"
#include "handle.h"
#include "handle_conv.h"
#include "hxm.h"
#include "menu.h"
#include "slotmap.h"

/* Bounded event processing per tick */
#ifndef MAX_EVENTS_PER_TICK
#define MAX_EVENTS_PER_TICK 512u
#endif

/* Merged ConfigureRequest for coalescing */
typedef struct pending_config {
    xcb_window_t window;

    int16_t x, y;
    uint16_t width, height;
    uint16_t border_width;

    xcb_window_t sibling;
    uint8_t stack_mode;
    uint16_t mask; /* value_mask from the request */
} pending_config_t;

/* Event buckets for coalescing */
typedef struct event_buckets {
    /* Ordered queues (ordering matters for correctness) */
    small_vec_t map_requests;     /* xcb_map_request_event_t* */
    small_vec_t unmap_notifies;   /* xcb_unmap_notify_event_t* */
    small_vec_t destroy_notifies; /* xcb_destroy_notify_event_t* */

    small_vec_t key_presses;     /* xcb_key_press_event_t* */
    small_vec_t button_events;   /* xcb_button_press_event_t* or xcb_button_release_event_t* */
    small_vec_t client_messages; /* xcb_client_message_event_t* */

    /* Expose coalesced by window: window -> dirty_region_t* */
    hash_map_t expose_regions;

    /* ConfigureRequest coalesced by window: window -> pending_config_t* */
    hash_map_t configure_requests;

    /* ConfigureNotify coalesced by window: window -> xcb_configure_notify_event_t* */
    hash_map_t configure_notifies;

    /* Destroy tracker for this tick: window -> (void*)1 */
    hash_map_t destroyed_windows;

    /* PropertyNotify coalesced by (window, atom): combined key -> xcb_property_notify_event_t* or small sentinel */
    hash_map_t property_notifies;

    /* MotionNotify latest per window: window -> xcb_motion_notify_event_t* */
    hash_map_t motion_notifies;

    /* Enter/Leave latest (not per-window), used for pointer focus rules */
    struct {
        xcb_enter_notify_event_t enter;
        xcb_leave_notify_event_t leave;
        bool enter_valid;
        bool leave_valid;
    } pointer_notify;

    /* Damage events coalesced by drawable: drawable -> dirty_region_t* */
    hash_map_t damage_regions;

    /* RandR coalescing */
    bool randr_dirty;
    uint16_t randr_width;
    uint16_t randr_height;

    /* Per-tick counters */
    uint64_t ingested;
    uint64_t coalesced;
} event_buckets_t;

/* Root dirty flags
 * Used to defer expensive root property updates to the end of the tick
 */
enum {
    ROOT_DIRTY_CLIENT_LIST = 1u << 0,
    ROOT_DIRTY_ACTIVE_WINDOW = 1u << 1,
    ROOT_DIRTY_CLIENT_LIST_STACKING = 1u << 2,
    ROOT_DIRTY_WORKAREA = 1u << 3,
    ROOT_DIRTY_VISIBILITY = 1u << 4,
    ROOT_DIRTY_CURRENT_DESKTOP = 1u << 5,
    ROOT_DIRTY_SHOWING_DESKTOP = 1u << 6
};

/* Interaction state (Move/Resize/Menu) */
typedef enum interaction_mode {
    INTERACTION_NONE = 0,
    INTERACTION_MOVE,
    INTERACTION_RESIZE,
    INTERACTION_MENU
} interaction_mode_t;

typedef enum resize_dir {
    RESIZE_NONE = 0,
    RESIZE_TOP = 1u << 0,
    RESIZE_BOTTOM = 1u << 1,
    RESIZE_LEFT = 1u << 2,
    RESIZE_RIGHT = 1u << 3
} resize_dir_t;

/* Monitor information */
typedef struct monitor {
    rect_t geom;
    rect_t workarea;
} monitor_t;

/* Main server state */
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
    int signal_fd;
    int timer_fd;

    /* Extension support flags */
    bool damage_supported;
    uint8_t damage_event_base;
    uint8_t damage_error_base;

    bool randr_supported;
    uint8_t randr_event_base;

    /* Root property dirty bits */
    uint32_t root_dirty;

    /* Monitor configuration */
    monitor_t* monitors;
    uint32_t monitor_count;

    /* Workarea (computed minus struts/docks) */
    rect_t workarea;
    bool workarea_dirty;

    /* Key symbols mapping */
    xcb_key_symbols_t* keysyms;

    /* Cursor resources */
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

    /* Interaction state */
    interaction_mode_t interaction_mode;
    resize_dir_t interaction_resize_dir;

    xcb_window_t interaction_window;
    handle_t interaction_handle;

    uint32_t interaction_time;       /* X server timestamp */
    uint64_t last_interaction_flush; /* monotonic ns */

    int16_t interaction_start_x, interaction_start_y;
    int16_t interaction_start_w, interaction_start_h;

    int16_t interaction_pointer_x, interaction_pointer_y;

    /* Per-tick scratch arena */
    arena_t tick_arena;

    /* Async reply tracking */
    cookie_jar_t cookie_jar;

    /* Prefetched X event */
    xcb_generic_event_t* prefetched_event;

    /* Coalescing buckets */
    event_buckets_t buckets;

    /* Client storage */
    slotmap_t clients;          /* owns hot/cold client memory */
    small_vec_t active_clients; /* handles (handle_t) for iteration */

    /* Global maps: XID -> handle */
    hash_map_t window_to_client;         /* xcb_window_t -> handle_t via ptr */
    hash_map_t frame_to_client;          /* frame XID -> handle_t via ptr */
    hash_map_t pending_unmanaged_states; /* xcb_window_t -> small_vec_t* */

    /* Stacking layers (bottom -> top) */
    small_vec_t layers[LAYER_COUNT]; /* each contains handle_t via ptr or direct value depending on ds impl */

    /* Focus */
    handle_t focused_client;
    xcb_window_t initial_focus;
    xcb_window_t committed_focus;
    list_node_t focus_history; /* MRU list head */

    /* Workspaces */
    uint32_t desktop_count;
    uint32_t current_desktop;
    bool showing_desktop;

    /* Root menu */
    menu_t menu;

    /* Control flags */
    bool running;
    bool restarting;
    int exit_code;

    bool x_poll_immediate;
    uint8_t force_poll_ticks;

    uint64_t txn_id; /* monotonic transaction id for cookie ordering */
    bool in_commit_phase;
    bool pending_flush;

    /* Configuration */
    config_t config;
    bool is_test;
} server_t;

/* ---------- Common server helpers ---------- */

static inline client_hot_t* server_chot(server_t* s, handle_t h) {
    if (!s) return NULL;
    if (!slotmap_live(&s->clients, h)) return NULL;
    return (client_hot_t*)slotmap_hot(&s->clients, h);
}

static inline client_cold_t* server_ccold(server_t* s, handle_t h) {
    if (!s) return NULL;
    if (!slotmap_live(&s->clients, h)) return NULL;
    return (client_cold_t*)slotmap_cold(&s->clients, h);
}

static inline handle_t server_get_client_by_window(server_t* s, xcb_window_t win) {
    if (!s || win == XCB_NONE) return HANDLE_INVALID;
    void* ptr = hash_map_get(&s->window_to_client, (uint64_t)win);
    return ptr_to_handle(ptr);
}

static inline handle_t server_get_client_by_frame(server_t* s, xcb_window_t frame) {
    if (!s || frame == XCB_NONE) return HANDLE_INVALID;
    void* ptr = hash_map_get(&s->frame_to_client, (uint64_t)frame);
    return ptr_to_handle(ptr);
}

/* ---------- Server lifecycle ---------- */

void server_init(server_t* s);
void server_run(server_t* s);
void server_cleanup(server_t* s);

/* ---------- Event ingestion and processing ---------- */

/* Ingest X events and coalesce into buckets
 * x_ready indicates the caller already knows the X fd is readable
 */
void event_ingest(server_t* s, bool x_ready);

/* Drain async replies (cookie_jar)
 * Returns true if any reply/timeout work was processed
 */
bool event_drain_cookies(server_t* s);

/* Process buckets, apply updates, flush dirty changes */
void event_process(server_t* s);

/* Schedule a timerfd-based wakeup after ms milliseconds */
void server_schedule_timer(server_t* s, int ms);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_H */
