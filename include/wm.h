#ifndef WM_H
#define WM_H

#include <stdbool.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "client.h"
#include "config.h"
#include "cookie_jar.h"
#include "xcb_utils.h"

typedef struct server server_t;
typedef struct pending_config pending_config_t;

void wm_become(server_t* s);
void wm_publish_desktop_props(server_t* s);
void wm_compute_workarea(server_t* s, rect_t* out);

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

void wm_client_apply_state_set(server_t* s, handle_t h, const client_state_set_t* set);
void wm_setup_keys(server_t* s);

void wm_cycle_focus(server_t* s, bool forward);

void wm_adopt_children(server_t* s);

void wm_place_window(server_t* s, handle_t h);

void wm_handle_map_request(server_t* s, xcb_map_request_event_t* ev);

void wm_handle_unmap_notify(server_t* s, xcb_unmap_notify_event_t* ev);

void wm_handle_destroy_notify(server_t* s, xcb_destroy_notify_event_t* ev);

void wm_handle_key_press(server_t* s, xcb_key_press_event_t* ev);
void wm_handle_button_press(server_t* s, xcb_button_press_event_t* ev);
void wm_handle_button_release(server_t* s, xcb_button_release_event_t* ev);
void wm_handle_motion_notify(server_t* s, xcb_motion_notify_event_t* ev);
void wm_handle_client_message(server_t* s, xcb_client_message_event_t* ev);

void wm_switch_workspace(server_t* s, uint32_t new_desktop);
void wm_switch_workspace_relative(server_t* s, int delta);
void wm_client_move_to_workspace(server_t* s, handle_t h, uint32_t desktop, bool follow);
void wm_client_toggle_sticky(server_t* s, handle_t h);
void wm_client_toggle_maximize(server_t* s, handle_t h);
void wm_client_iconify(server_t* s, handle_t h);
void wm_client_restore(server_t* s, handle_t h);

// From src/focus.c
void wm_set_focus(server_t* s, handle_t h);

// From src/stack.c
void stack_raise(server_t* s, handle_t h);
void stack_lower(server_t* s, handle_t h);
void stack_place_above(server_t* s, handle_t h, handle_t sibling_h);
void stack_place_below(server_t* s, handle_t h, handle_t sibling_h);
void stack_remove(server_t* s, handle_t h);
void stack_move_to_layer(server_t* s, handle_t h);

void wm_handle_configure_request(server_t* s, handle_t h, pending_config_t* ev);
void wm_handle_configure_notify(server_t* s, handle_t h, xcb_configure_notify_event_t* ev);
void wm_handle_property_notify(server_t* s, handle_t h, xcb_property_notify_event_t* ev);
void wm_flush_dirty(server_t* s);
void wm_handle_reply(server_t* s, const cookie_slot_t* slot, void* reply, xcb_generic_error_t* err);
void wm_client_update_state(server_t* s, handle_t h, uint32_t action, xcb_atom_t prop);
void wm_send_synthetic_configure(server_t* s, handle_t h);
void wm_client_refresh_title(server_t* s, handle_t h);

#endif  // WM_H
