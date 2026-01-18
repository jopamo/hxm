/*
 * wm.h - Window manager core definitions
 *
 * Declares core window management entry points and event handlers
 *
 * Responsibilities:
 * - Ownership of WM selection (become/release)
 * - Publishing EWMH/ICCCM desktop properties
 * - Client lifecycle: manage/unmanage, map/unmap/destroy flows
 * - Focus policy and cycling
 * - Workspace (desktop) switching and client migration
 * - User interactions (move/resize) and cancellation
 * - Stacking operations and synchronization to X11
 * - Handling async replies via cookie_jar
 *
 * Threading:
 * - Not thread-safe
 * - Intended to be called from the server's main thread only
 *
 * Conventions:
 * - handle_t identifies a managed client (see client.h + slotmap/handles)
 * - Event handlers accept raw XCB events and may ignore unrelated events safely
 */

#ifndef WM_H
#define WM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "client.h"
#include "config.h"
#include "cookie_jar.h"
#include "hxm.h"
#include "xcb_utils.h"

typedef struct server server_t;
typedef struct pending_config pending_config_t;

/* WM ownership lifecycle */
void wm_become(server_t* s);
void wm_release(server_t* s);

/* EWMH/desktop properties */
void wm_publish_desktop_props(server_t* s);

/* Compute current workarea in root coordinates */
void wm_compute_workarea(server_t* s, rect_t* out);
void wm_get_monitor_geometry(server_t* s, client_hot_t* hot, rect_t* out_geom);

/* Client state set for _NET_WM_STATE style updates */
typedef struct client_state_set {
    bool fullscreen;
    bool above;
    bool below;
    bool sticky;
    bool urgent;
    bool max_horz;
    bool max_vert;
    bool modal;
    bool shaded;
    bool skip_taskbar;
    bool skip_pager;
} client_state_set_t;

/* Apply a full state-set to a client (authoritative desired state) */
void wm_client_apply_state_set(server_t* s, handle_t h, const client_state_set_t* set);

/* Key grab setup from config */
void wm_setup_keys(server_t* s);

/* Focus helpers */
void wm_cycle_focus(server_t* s, bool forward);

/* Adopt existing children on startup (reparent/scan) */
void wm_adopt_children(server_t* s);

/* Place a newly-managed window per placement policy and rules */
void wm_place_window(server_t* s, handle_t h);

/* X event handlers */
void wm_handle_map_request(server_t* s, xcb_map_request_event_t* ev);
void wm_handle_unmap_notify(server_t* s, xcb_unmap_notify_event_t* ev);
void wm_handle_destroy_notify(server_t* s, xcb_destroy_notify_event_t* ev);

void wm_handle_key_press(server_t* s, xcb_key_press_event_t* ev);
void wm_handle_key_release(server_t* s, xcb_key_release_event_t* ev);
void wm_handle_button_press(server_t* s, xcb_button_press_event_t* ev);
void wm_handle_button_release(server_t* s, xcb_button_release_event_t* ev);
void wm_handle_motion_notify(server_t* s, xcb_motion_notify_event_t* ev);
void wm_handle_client_message(server_t* s, xcb_client_message_event_t* ev);
void wm_handle_configure_request(server_t* s, handle_t h, pending_config_t* ev);
void wm_handle_configure_notify(server_t* s, handle_t h, xcb_configure_notify_event_t* ev);
void wm_handle_property_notify(server_t* s, handle_t h, xcb_property_notify_event_t* ev);
void wm_handle_colormap_notify(server_t* s, xcb_colormap_notify_event_t* ev);

/* Cancel current move/resize or other grab-based interaction */
void wm_cancel_interaction(server_t* s);

/* Workspace management */
void wm_switch_workspace(server_t* s, uint32_t new_desktop);
void wm_switch_workspace_relative(server_t* s, int delta);

void wm_client_move_to_workspace(server_t* s, handle_t h, uint32_t desktop, bool follow);
void wm_client_toggle_sticky(server_t* s, handle_t h);
void wm_client_toggle_maximize(server_t* s, handle_t h);
void wm_client_iconify(server_t* s, handle_t h);
void wm_client_restore(server_t* s, handle_t h);

/* Focus implementation (src/focus.c) */
void wm_set_focus(server_t* s, handle_t h);

/* Alt-tab switcher */
void wm_switcher_start(server_t* s, int dir);
void wm_switcher_step(server_t* s, int dir);
void wm_switcher_commit(server_t* s);
void wm_switcher_cancel(server_t* s);

/* Stacking (src/stack.c) */
void stack_raise(server_t* s, handle_t h);
void stack_lower(server_t* s, handle_t h);
void stack_place_above(server_t* s, handle_t h, handle_t sibling_h);
void stack_place_below(server_t* s, handle_t h, handle_t sibling_h);
void stack_remove(server_t* s, handle_t h);

/* Move client to its configured layer (based on state/rules) */
void stack_move_to_layer(server_t* s, handle_t h);

/* Sync stacking changes to X11 for a client */
void stack_sync_to_xcb(server_t* s, handle_t h);

/* Dirty flush:
 * Returns true if more work remains or if flush did work (implementation-specific)
 */
bool wm_flush_dirty(server_t* s, uint64_t now);

/* Async reply dispatch from cookie_jar */
void wm_handle_reply(server_t* s, const cookie_slot_t* slot, void* reply, xcb_generic_error_t* err);

/* State/metadata updates */
void wm_client_update_state(server_t* s, handle_t h, uint32_t action, xcb_atom_t prop);
void wm_send_synthetic_configure(server_t* s, handle_t h);
void wm_client_refresh_title(server_t* s, handle_t h);

#ifdef __cplusplus
}
#endif

#endif /* WM_H */
