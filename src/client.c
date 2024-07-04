#include "client.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "bbox.h"
#include "event.h"
#include "frame.h"
#include "slotmap.h"
#include "wm.h"
#include "xcb_utils.h"

bool should_focus_on_map(const client_hot_t* hot) {
    if (hot->focus_override != -1) return (bool)hot->focus_override;

    // Some window types should never get focus on map
    if (hot->type == WINDOW_TYPE_DOCK || hot->type == WINDOW_TYPE_NOTIFICATION || hot->type == WINDOW_TYPE_DESKTOP ||
        hot->type == WINDOW_TYPE_MENU || hot->type == WINDOW_TYPE_DROPDOWN_MENU ||
        hot->type == WINDOW_TYPE_POPUP_MENU || hot->type == WINDOW_TYPE_TOOLTIP) {
        return false;
    }
    // Dialogs and transients may steal focus
    if (hot->type == WINDOW_TYPE_DIALOG) return true;
    if (hot->transient_for != HANDLE_INVALID) return true;
    return false;
}

void client_manage_start(server_t* s, xcb_window_t win) {
    if (server_get_client_by_window(s, win) != HANDLE_INVALID) {
        LOG_DEBUG("Already managing window %u", win);
        return;
    }

    // Allocate slot
    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
    if (h == HANDLE_INVALID) {
        LOG_ERROR("Failed to allocate client slot for window %u", win);
        return;
    }

    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;

    hot->self = h;
    hot->xid = win;
    hot->state = STATE_NEW;
    hot->layer = LAYER_NORMAL;
    hot->focus_override = -1;
    hot->maximized_horz = false;
    hot->maximized_vert = false;
    hot->saved_maximize_valid = false;
    hot->saved_maximized_horz = false;
    hot->saved_maximized_vert = false;
    hot->last_cursor_dir = -1;
    render_init(&hot->render_ctx);
    hot->icon_surface = NULL;
    hot->visual_type = NULL;
    hot->depth = 0;
    hot->pending_replies = 15;  // Attrs, Geom, Class, Hints, NormalHints, Transient, Type, Protocols, Name, NetName,
                                // NetState, Desktop, Strut, StrutPartial, Icon
    hot->override_redirect = false;
    hot->manage_aborted = false;

    cold->can_focus = true;
    arena_init(&cold->string_arena, 512);

    list_init(&hot->stacking_node);
    list_init(&hot->transient_sibling);
    list_init(&hot->transients_head);
    list_init(&hot->focus_node);

    // Register mapping so we can find it
    hash_map_insert(&s->window_to_client, win, handle_to_ptr(h));

    // 1. GetWindowAttributes (to check override_redirect)
    uint32_t c1 = xcb_get_window_attributes(s->conn, win).sequence;
    cookie_jar_push(&s->cookie_jar, c1, COOKIE_GET_WINDOW_ATTRIBUTES, h, win);

    // 2. GetGeometry
    uint32_t c2 = xcb_get_geometry(s->conn, win).sequence;
    cookie_jar_push(&s->cookie_jar, c2, COOKIE_GET_GEOMETRY, h, win);

    // 3. GetProperty (WM_CLASS)
    uint32_t c3 = xcb_get_property(s->conn, 0, win, atoms.WM_CLASS, XCB_ATOM_STRING, 0, 1024).sequence;
    cookie_jar_push(&s->cookie_jar, c3, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_CLASS);

    // 4. GetProperty (WM_HINTS)
    uint32_t c4 = xcb_get_property(s->conn, 0, win, atoms.WM_HINTS, atoms.WM_HINTS, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c4, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_HINTS);

    // 5. GetProperty (WM_NORMAL_HINTS)
    uint32_t c5 = xcb_get_property(s->conn, 0, win, atoms.WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c5, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_NORMAL_HINTS);

    // 6. GetProperty (WM_TRANSIENT_FOR)
    uint32_t c6 = xcb_get_property(s->conn, 0, win, atoms.WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1).sequence;
    cookie_jar_push(&s->cookie_jar, c6, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_TRANSIENT_FOR);

    // 7. GetProperty (_NET_WM_WINDOW_TYPE)
    uint32_t c7 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c7, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_WINDOW_TYPE);

    // 8. GetProperty (WM_PROTOCOLS)
    uint32_t c8 = xcb_get_property(s->conn, 0, win, atoms.WM_PROTOCOLS, XCB_ATOM_ATOM, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c8, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_PROTOCOLS);

    // 9. GetProperty (_NET_WM_NAME)
    uint32_t c9 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_NAME, atoms.UTF8_STRING, 0, 1024).sequence;
    cookie_jar_push(&s->cookie_jar, c9, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_NAME);

    // 10. GetProperty (WM_NAME)
    uint32_t c10 = xcb_get_property(s->conn, 0, win, atoms.WM_NAME, XCB_ATOM_STRING, 0, 1024).sequence;
    cookie_jar_push(&s->cookie_jar, c10, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_NAME);

    // 11. GetProperty (_NET_WM_STATE)
    uint32_t c11 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_STATE, XCB_ATOM_ATOM, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c11, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_STATE);

    // 12. GetProperty (_NET_WM_DESKTOP)
    uint32_t c12 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 0, 1).sequence;
    cookie_jar_push(&s->cookie_jar, c12, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_DESKTOP);

    // 13. GetProperty (_NET_WM_STRUT)
    uint32_t c13 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_STRUT, XCB_ATOM_CARDINAL, 0, 4).sequence;
    cookie_jar_push(&s->cookie_jar, c13, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_STRUT);

    // 14. GetProperty (_NET_WM_STRUT_PARTIAL)
    uint32_t c14 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 0, 12).sequence;
    cookie_jar_push(&s->cookie_jar, c14, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_STRUT_PARTIAL);

    // 15. GetProperty (_NET_WM_ICON)
    uint32_t c15 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_ICON, XCB_ATOM_CARDINAL, 0, 16384).sequence;
    cookie_jar_push(&s->cookie_jar, c15, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_ICON);

    LOG_DEBUG("Started management for window %u (handle %lx)", win, h);
}

static void client_apply_rules(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    client_cold_t* cold = server_ccold(s, h);
    if (!hot || !cold) return;

    for (size_t i = 0; i < s->config.rules.length; i++) {
        app_rule_t* r = s->config.rules.items[i];
        bool matched = true;

        if (r->class_match && (!cold->wm_class || strcmp(cold->wm_class, r->class_match) != 0)) matched = false;
        if (matched && r->instance_match && (!cold->wm_instance || strcmp(cold->wm_instance, r->instance_match) != 0))
            matched = false;
        if (matched && r->title_match && (!cold->title || !strstr(cold->title, r->title_match))) matched = false;
        if (matched && r->type_match != -1 && hot->type != (uint8_t)r->type_match) matched = false;
        if (matched && r->transient_match != -1) {
            bool is_transient = (hot->transient_for != HANDLE_INVALID);
            if (is_transient != (bool)r->transient_match) matched = false;
        }

        if (matched) {
            LOG_INFO("Rule matched for window %u", hot->xid);
            if (r->desktop != -2) {
                if (r->desktop == -1) {
                    hot->desktop = -1;
                    hot->sticky = true;
                } else {
                    hot->desktop = r->desktop;
                    hot->sticky = false;
                }
            }
            if (r->layer != -1) hot->layer = (uint8_t)r->layer;
            if (r->focus != -1) hot->focus_override = r->focus;
            if (r->placement != PLACEMENT_DEFAULT) hot->placement = (uint8_t)r->placement;
        }
    }

    if (!hot->sticky && hot->desktop >= (int32_t)s->desktop_count) {
        hot->desktop = (int32_t)s->current_desktop;
    }
}

void client_finish_manage(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    client_apply_rules(s, h);

    wm_place_window(s, h);

    // 1. Create frame
    // Openbox-like simple frame
    rect_t geom = hot->desired;
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    // Set background to inactive color initially
    uint32_t values[3];
    values[0] = 0x333333;
    values[1] = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_POINTER_MOTION |
                XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW;

    uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
    uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;

    // Use client visual/depth for the frame to avoid reparenting errors
    if (hot->visual_id != s->root_visual) {
        mask |= XCB_CW_COLORMAP;
        xcb_colormap_t cmap = xcb_generate_id(s->conn);
        xcb_create_colormap(s->conn, XCB_COLORMAP_ALLOC_NONE, cmap, s->root, hot->visual_id);
        values[2] = cmap;
    }

    hot->frame = xcb_generate_id(s->conn);
    xcb_create_window(s->conn, hot->depth, hot->frame, s->root, geom.x, geom.y, geom.w + 2 * bw, geom.h + th + bw, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, hot->visual_id, mask, values);

    // Register frame mapping
    hash_map_insert(&s->frame_to_client, hot->frame, handle_to_ptr(h));

    // 2. Add to SaveSet (Crash safety)
    xcb_change_save_set(s->conn, XCB_SET_MODE_INSERT, hot->xid);

    // 3. Reparent
    xcb_reparent_window(s->conn, hot->xid, hot->frame, bw, th);

    // Set _NET_FRAME_EXTENTS (before mapping)
    uint32_t extents[4] = {bw, bw, th + bw, bw};
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 32, 4,
                        extents);

    // Set _NET_WM_ALLOWED_ACTIONS (before mapping)
    xcb_atom_t actions[16];
    uint32_t num_actions = 0;
    actions[num_actions++] = atoms._NET_WM_ACTION_MOVE;
    actions[num_actions++] = atoms._NET_WM_ACTION_MINIMIZE;
    actions[num_actions++] = atoms._NET_WM_ACTION_STICK;
    actions[num_actions++] = atoms._NET_WM_ACTION_CHANGE_DESKTOP;
    actions[num_actions++] = atoms._NET_WM_ACTION_CLOSE;
    actions[num_actions++] = atoms._NET_WM_ACTION_ABOVE;
    actions[num_actions++] = atoms._NET_WM_ACTION_BELOW;

    bool fixed = (hot->hints.max_w > 0 && hot->hints.min_w == hot->hints.max_w && hot->hints.max_h > 0 &&
                  hot->hints.min_h == hot->hints.max_h);

    if (!fixed) {
        actions[num_actions++] = atoms._NET_WM_ACTION_RESIZE;
        actions[num_actions++] = atoms._NET_WM_ACTION_MAXIMIZE_HORZ;
        actions[num_actions++] = atoms._NET_WM_ACTION_MAXIMIZE_VERT;
        actions[num_actions++] = atoms._NET_WM_ACTION_FULLSCREEN;
    }

    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_ALLOWED_ACTIONS, XCB_ATOM_ATOM, 32,
                        num_actions, actions);

    // 4. Map if visible on current desktop
    bool visible = hot->sticky || (hot->desktop == (int32_t)s->current_desktop);
    if (visible) {
        xcb_map_window(s->conn, hot->xid);
        xcb_map_window(s->conn, hot->frame);
        hot->state = STATE_MAPPED;

        // Set WM_STATE to NormalState (required by some toolkits like Qt)
        uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                            state_vals);
    } else {
        hot->state = STATE_UNMAPPED;
        // Set WM_STATE to IconicState
        uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                            state_vals);
    }
    hot->dirty |= DIRTY_STATE;

    // Subscribe to client events
    uint32_t client_events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE;
    xcb_change_window_attributes(s->conn, hot->xid, XCB_CW_EVENT_MASK, &client_events);

    // Setup passive grabs for click-to-focus and Alt-move/resize
    client_setup_grabs(s, h);

    // Initial stacking
    if (hot->transient_for != HANDLE_INVALID) {
        stack_place_above(s, h, hot->transient_for);
    } else {
        stack_raise(s, h);
    }

    // Add to focus history
    list_insert(&hot->focus_node, &s->focus_history, s->focus_history.next);

    // Focus new window if visible and allowed (dialogs/transients) or if nothing is focused
    if (visible && (s->focused_client == HANDLE_INVALID || should_focus_on_map(hot))) {
        wm_set_focus(s, h);
    }

    // Draw initial decorations
    frame_redraw(s, h, FRAME_REDRAW_ALL);

    // Publish initial desktop if not already set by client
    uint32_t desk_prop = (uint32_t)hot->desktop;
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 32, 1,
                        &desk_prop);

    // Mark client list as dirty
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_WORKAREA;

    LOG_INFO("Managed window %u as client %lx (frame %u)", hot->xid, h, hot->frame);
}

void client_unmanage(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    client_cold_t* cold = server_ccold(s, h);
    if (!hot || hot->state == STATE_UNMANAGING || hot->state == STATE_UNMANAGED) return;

    bool destroyed = (hot->state == STATE_DESTROYED);
    hot->state = STATE_UNMANAGING;

    LOG_INFO("Unmanaging client %lx (window %u, destroyed=%d)", h, hot->xid, destroyed);

    // 0. Remove from stacking
    stack_remove(s, h);

    // Unlink from parent
    if (hot->transient_sibling.next && hot->transient_sibling.next != &hot->transient_sibling) {
        list_remove(&hot->transient_sibling);
        list_init(&hot->transient_sibling);
    }

    // Unlink children
    while (!list_empty(&hot->transients_head)) {
        list_node_t* node = hot->transients_head.next;
        // offsetof trick to get child
        client_hot_t* child = (client_hot_t*)((char*)node - offsetof(client_hot_t, transient_sibling));
        child->transient_for = HANDLE_INVALID;
        list_remove(node);
        list_init(node);
        // Note: we don't need to update child's state other than transient_for handle
    }

    // Remove from focus history
    if (hot->focus_node.next && hot->focus_node.next != &hot->focus_node) {
        list_remove(&hot->focus_node);
        list_init(&hot->focus_node);
    }

    // If focused, pick next
    if (s->focused_client == h) {
        handle_t next_h = HANDLE_INVALID;

        // 1. Prefer parent if still mapped
        if (hot->transient_for != HANDLE_INVALID) {
            client_hot_t* parent = server_chot(s, hot->transient_for);
            if (parent && parent->state == STATE_MAPPED) {
                next_h = hot->transient_for;
            }
        }

        // 2. Fallback to MRU
        if (next_h == HANDLE_INVALID) {
            list_node_t* node = s->focus_history.next;
            while (node != &s->focus_history) {
                client_hot_t* cand = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));
                if (cand->state == STATE_MAPPED) {
                    next_h = cand->self;
                    break;
                }
                node = node->next;
            }
        }

        wm_set_focus(s, next_h);
    }

    // 1. Remove from SaveSet
    xcb_change_save_set(s->conn, XCB_SET_MODE_DELETE, hot->xid);

    // 2. Reparent back to root (if window still exists)
    if (!destroyed) {
        // Calculate root coordinates? For now just 0,0 or keep frame position.
        xcb_reparent_window(s->conn, hot->xid, s->root, hot->server.x, hot->server.y);
    }

    // 3. Destroy frame
    if (hot->frame != XCB_NONE) {
        xcb_destroy_window(s->conn, hot->frame);
    }

    // 4. Cleanup maps
    if (hot->xid != XCB_NONE) hash_map_remove(&s->window_to_client, hot->xid);
    if (hot->frame != XCB_NONE) hash_map_remove(&s->frame_to_client, hot->frame);

    // 5. Free cold data
    arena_destroy(&cold->string_arena);
    render_free(&hot->render_ctx);
    if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);

    // 6. Free slot
    slotmap_free(&s->clients, h);

    // Mark client list as dirty
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_WORKAREA;
}

void client_close(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    client_cold_t* cold = server_ccold(s, h);
    if (!hot || !cold || hot->state == STATE_DESTROYED || hot->state == STATE_UNMANAGED) return;

    if (cold->protocols & PROTOCOL_DELETE_WINDOW) {
        LOG_DEBUG("Sending WM_DELETE_WINDOW to client %lx", h);
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = hot->xid;
        ev.type = atoms.WM_PROTOCOLS;
        ev.data.data32[0] = atoms.WM_DELETE_WINDOW;
        ev.data.data32[1] = XCB_CURRENT_TIME;

        xcb_send_event(s->conn, 0, hot->xid, XCB_EVENT_MASK_NO_EVENT, (const char*)&ev);
    } else {
        LOG_DEBUG("Killing client %lx", h);
        xcb_kill_client(s->conn, hot->xid);
    }
}

void client_constrain_size(const size_hints_t* s, uint16_t* w, uint16_t* h) {
    // 1. Min/Max size
    if (s->min_w > 0 && *w < (uint16_t)s->min_w) *w = (uint16_t)s->min_w;
    if (s->min_h > 0 && *h < (uint16_t)s->min_h) *h = (uint16_t)s->min_h;

    if (s->max_w > 0 && *w > (uint16_t)s->max_w) *w = (uint16_t)s->max_w;
    if (s->max_h > 0 && *h > (uint16_t)s->max_h) *h = (uint16_t)s->max_h;

    // 2. Aspect ratio
    if (s->min_aspect_den > 0 && s->min_aspect_num > 0) {
        if ((int32_t)(*w) * s->min_aspect_den < (int32_t)(*h) * s->min_aspect_num) {
            *w = (uint16_t)((int32_t)(*h) * s->min_aspect_num / s->min_aspect_den);
        }
    }
    if (s->max_aspect_den > 0 && s->max_aspect_num > 0) {
        if ((int32_t)(*w) * s->max_aspect_den > (int32_t)(*h) * s->max_aspect_num) {
            *h = (uint16_t)((int32_t)(*w) * s->max_aspect_den / s->max_aspect_num);
        }
    }

    // 3. Resize increments
    if (s->inc_w > 1) {
        int32_t base_w = (s->base_w > 0) ? s->base_w : (s->min_w > 0 ? s->min_w : 0);
        if (*w > base_w) {
            *w = (uint16_t)(base_w + ((*w - base_w) / s->inc_w) * s->inc_w);
        }
    }
    if (s->inc_h > 1) {
        int32_t base_h = (s->base_h > 0) ? s->base_h : (s->min_h > 0 ? s->min_h : 0);
        if (*h > base_h) {
            *h = (uint16_t)(base_h + ((*h - base_h) / s->inc_h) * s->inc_h);
        }
    }
}

void client_setup_grabs(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    // Grab buttons 1, 2, 3 for click-to-focus and Alt-move/resize
    // We use SYNC mode for the pointer to allow interception of clicks.
    uint16_t buttons[] = {1, 2, 3};
    for (int i = 0; i < 3; i++) {
        xcb_grab_button(s->conn, 0, hot->xid, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_NONE, XCB_NONE, buttons[i], XCB_MOD_MASK_ANY);
    }
}
