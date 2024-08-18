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

#ifdef BBOX_DEBUG_TRACE
static void debug_dump_focus_history(const server_t* s, const char* tag) {
    if (!s) return;
    const list_node_t* head = &s->focus_history;
    LOG_DEBUG("focus_history %s head=%p next=%p prev=%p", tag, (void*)head, (void*)head->next, (void*)head->prev);
    const list_node_t* node = head->next;
    int guard = 0;
    while (node != head && guard < 128) {
        const client_hot_t* c = (const client_hot_t*)((const char*)node - offsetof(client_hot_t, focus_node));
        LOG_DEBUG("  [%d] node=%p prev=%p next=%p h=%lx xid=%u state=%d", guard, (void*)node, (void*)node->prev,
                  (void*)node->next, c->self, c->xid, c->state);
        node = node->next;
        guard++;
    }
    if (node != head) {
        LOG_WARN("focus_history %s: guard hit at %d, possible loop", tag, guard);
    }
}

static void debug_dump_transients(const client_hot_t* hot, const char* tag) {
    if (!hot) return;
    const list_node_t* head = &hot->transients_head;
    LOG_DEBUG("transients %s h=%lx head=%p next=%p prev=%p", tag, hot->self, (void*)head, (void*)head->next,
              (void*)head->prev);
    const list_node_t* node = head->next;
    int guard = 0;
    while (node != head && guard < 64) {
        const client_hot_t* c = (const client_hot_t*)((const char*)node - offsetof(client_hot_t, transient_sibling));
        LOG_DEBUG("  [%d] node=%p prev=%p next=%p h=%lx xid=%u state=%d", guard, (void*)node, (void*)node->prev,
                  (void*)node->next, c->self, c->xid, c->state);
        node = node->next;
        guard++;
    }
    if (node != head) {
        LOG_WARN("transients %s: guard hit at %d, possible loop", tag, guard);
    }
}
#endif

bool should_focus_on_map(const client_hot_t* hot) {
    if (hot->focus_override != -1) return (bool)hot->focus_override;

    // Some window types should never get focus on map
    if (hot->type == WINDOW_TYPE_DOCK || hot->type == WINDOW_TYPE_NOTIFICATION || hot->type == WINDOW_TYPE_DESKTOP ||
        hot->type == WINDOW_TYPE_MENU || hot->type == WINDOW_TYPE_DROPDOWN_MENU ||
        hot->type == WINDOW_TYPE_POPUP_MENU || hot->type == WINDOW_TYPE_TOOLTIP || hot->type == WINDOW_TYPE_COMBO ||
        hot->type == WINDOW_TYPE_DND) {
        return false;
    }

    // Dialogs and transients may steal focus
    if (hot->type == WINDOW_TYPE_DIALOG) return true;
    if (hot->transient_for != HANDLE_INVALID) return true;
    return false;
}

static bool should_hide_for_show_desktop(const client_hot_t* hot) {
    return hot && hot->type != WINDOW_TYPE_DOCK && hot->type != WINDOW_TYPE_DESKTOP;
}

void client_manage_start(server_t* s, xcb_window_t win) {
    TRACE_LOG("manage_start win=%u", win);
    if (server_get_client_by_window(s, win) != HANDLE_INVALID) {
        LOG_DEBUG("Already managing window %u", win);
        return;
    }

    // Allocate slot
    void* hot_ptr;
    void* cold_ptr;
    handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
    if (h == HANDLE_INVALID) {
        LOG_ERROR("Failed to allocate client slot for window %u", win);
        return;
    }
    TRACE_LOG("manage_start allocated handle=%lx for win=%u", h, win);

    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;

    hot->self = h;
    hot->xid = win;
    hot->state = STATE_NEW;
    hot->manage_phase = MANAGE_PHASE1;

    hot->initial_state = XCB_ICCCM_WM_STATE_NORMAL;

    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;

    hot->state_above = false;
    hot->state_below = false;

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
    hot->visual_id = s->root_visual;
    hot->depth = 0;

    hot->damage = XCB_NONE;
    dirty_region_reset(&hot->damage_region);

    hot->ignore_unmap = 1;
    hot->override_redirect = false;
    hot->manage_aborted = false;

    hot->user_time = 0;
    hot->user_time_window = XCB_NONE;
    hot->sync_enabled = false;
    hot->sync_counter = 0;
    hot->icon_geometry_valid = false;

    // Phase 1 cookie budget
    // Attrs, Geom, Class, ClientMachine, Hints, NormalHints, Transient, Type, Protocols,
    // NetName, Name, NetIconName, IconName, NetState, Desktop,
    // Strut, StrutPartial, Icon, PID, UserTime, UserTimeWindow,
    // SyncCounter, IconGeometry, MotifHints, WindowOpacity
    hot->pending_replies = 25;

    cold->can_focus = true;
    arena_init(&cold->string_arena, 512);

    list_init(&hot->stacking_node);
    list_init(&hot->transient_sibling);
    list_init(&hot->transients_head);
    list_init(&hot->focus_node);

    TRACE_LOG("manage_start init nodes h=%lx focus_node=%p", h, (void*)&hot->focus_node);

    // Register mapping so we can find it
    hash_map_insert(&s->window_to_client, win, handle_to_ptr(h));
    TRACE_LOG("manage_start window_to_client[%u]=%lx", win, h);

    // 1. GetWindowAttributes (override_redirect, visual)
    uint32_t c1 = xcb_get_window_attributes(s->conn, win).sequence;
    cookie_jar_push(&s->cookie_jar, c1, COOKIE_GET_WINDOW_ATTRIBUTES, h, win, wm_handle_reply);

    // 2. GetGeometry
    uint32_t c2 = xcb_get_geometry(s->conn, win).sequence;
    cookie_jar_push(&s->cookie_jar, c2, COOKIE_GET_GEOMETRY, h, win, wm_handle_reply);

    // 3. WM_CLASS
    uint32_t c3 = xcb_get_property(s->conn, 0, win, atoms.WM_CLASS, XCB_ATOM_STRING, 0, 1024).sequence;
    cookie_jar_push(&s->cookie_jar, c3, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_CLASS,
                    wm_handle_reply);

    // 4. WM_CLIENT_MACHINE
    uint32_t c4 = xcb_get_property(s->conn, 0, win, atoms.WM_CLIENT_MACHINE, XCB_ATOM_STRING, 0, 1024).sequence;
    cookie_jar_push(&s->cookie_jar, c4, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_CLIENT_MACHINE,
                    wm_handle_reply);

    // 5. WM_HINTS
    uint32_t c5 = xcb_get_property(s->conn, 0, win, atoms.WM_HINTS, atoms.WM_HINTS, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c5, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_HINTS,
                    wm_handle_reply);

    // 6. WM_NORMAL_HINTS
    uint32_t c6 = xcb_get_property(s->conn, 0, win, atoms.WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c6, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_NORMAL_HINTS,
                    wm_handle_reply);

    // 7. WM_TRANSIENT_FOR
    uint32_t c7 = xcb_get_property(s->conn, 0, win, atoms.WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1).sequence;
    cookie_jar_push(&s->cookie_jar, c7, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_TRANSIENT_FOR,
                    wm_handle_reply);

    // 8. _NET_WM_WINDOW_TYPE
    uint32_t c8 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c8, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_WINDOW_TYPE,
                    wm_handle_reply);

    // 9. WM_PROTOCOLS
    uint32_t c9 = xcb_get_property(s->conn, 0, win, atoms.WM_PROTOCOLS, XCB_ATOM_ATOM, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c9, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_PROTOCOLS,
                    wm_handle_reply);

    // 10. _NET_WM_NAME (UTF8)
    uint32_t c10 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_NAME, atoms.UTF8_STRING, 0, 1024).sequence;
    cookie_jar_push(&s->cookie_jar, c10, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_NAME,
                    wm_handle_reply);

    // 11. WM_NAME
    uint32_t c11 = xcb_get_property(s->conn, 0, win, atoms.WM_NAME, XCB_ATOM_STRING, 0, 1024).sequence;
    cookie_jar_push(&s->cookie_jar, c11, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_NAME,
                    wm_handle_reply);

    // 12. _NET_WM_ICON_NAME (UTF8)
    uint32_t c12 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_ICON_NAME, atoms.UTF8_STRING, 0, 1024).sequence;
    cookie_jar_push(&s->cookie_jar, c12, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_ICON_NAME,
                    wm_handle_reply);

    // 13. WM_ICON_NAME
    uint32_t c13 = xcb_get_property(s->conn, 0, win, atoms.WM_ICON_NAME, XCB_ATOM_STRING, 0, 1024).sequence;
    cookie_jar_push(&s->cookie_jar, c13, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms.WM_ICON_NAME,
                    wm_handle_reply);

    // 14. _NET_WM_STATE
    uint32_t c14 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_STATE, XCB_ATOM_ATOM, 0, 32).sequence;
    cookie_jar_push(&s->cookie_jar, c14, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_STATE,
                    wm_handle_reply);

    // 15. _NET_WM_DESKTOP
    uint32_t c15 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 0, 1).sequence;
    cookie_jar_push(&s->cookie_jar, c15, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_DESKTOP,
                    wm_handle_reply);

    // 16. _NET_WM_STRUT
    uint32_t c16 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_STRUT, XCB_ATOM_CARDINAL, 0, 4).sequence;
    cookie_jar_push(&s->cookie_jar, c16, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_STRUT,
                    wm_handle_reply);

    // 17. _NET_WM_STRUT_PARTIAL
    uint32_t c17 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 0, 12).sequence;
    cookie_jar_push(&s->cookie_jar, c17, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_STRUT_PARTIAL,
                    wm_handle_reply);

    // 18. _NET_WM_ICON
    uint32_t c18 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_ICON, XCB_ATOM_CARDINAL, 0, 16384).sequence;
    cookie_jar_push(&s->cookie_jar, c18, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_ICON,
                    wm_handle_reply);

    // 19. _NET_WM_PID
    uint32_t c19 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_PID, XCB_ATOM_CARDINAL, 0, 1).sequence;
    cookie_jar_push(&s->cookie_jar, c19, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_PID,
                    wm_handle_reply);

    // 20. _NET_WM_USER_TIME
    uint32_t c20 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_USER_TIME, XCB_ATOM_CARDINAL, 0, 1).sequence;
    cookie_jar_push(&s->cookie_jar, c20, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_USER_TIME,
                    wm_handle_reply);

    // 21. _NET_WM_USER_TIME_WINDOW
    uint32_t c21 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_USER_TIME_WINDOW, XCB_ATOM_WINDOW, 0, 1).sequence;
    cookie_jar_push(&s->cookie_jar, c21, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_USER_TIME_WINDOW,
                    wm_handle_reply);

    // 22. _NET_WM_SYNC_REQUEST_COUNTER
    uint32_t c22 =
        xcb_get_property(s->conn, 0, win, atoms._NET_WM_SYNC_REQUEST_COUNTER, XCB_ATOM_CARDINAL, 0, 1).sequence;
    cookie_jar_push(&s->cookie_jar, c22, COOKIE_GET_PROPERTY, h,
                    ((uint64_t)win << 32) | atoms._NET_WM_SYNC_REQUEST_COUNTER, wm_handle_reply);

    // 23. _NET_WM_ICON_GEOMETRY
    uint32_t c23 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_ICON_GEOMETRY, XCB_ATOM_CARDINAL, 0, 4).sequence;
    cookie_jar_push(&s->cookie_jar, c23, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_ICON_GEOMETRY,
                    wm_handle_reply);

    // 24. _MOTIF_WM_HINTS
    uint32_t c24 = xcb_get_property(s->conn, 0, win, atoms._MOTIF_WM_HINTS, XCB_ATOM_ANY, 0, 5).sequence;
    cookie_jar_push(&s->cookie_jar, c24, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._MOTIF_WM_HINTS,
                    wm_handle_reply);

    // 25. _NET_WM_WINDOW_OPACITY
    uint32_t c25 = xcb_get_property(s->conn, 0, win, atoms._NET_WM_WINDOW_OPACITY, XCB_ATOM_CARDINAL, 0, 1).sequence;
    cookie_jar_push(&s->cookie_jar, c25, COOKIE_GET_PROPERTY, h, ((uint64_t)win << 32) | atoms._NET_WM_WINDOW_OPACITY,
                    wm_handle_reply);

    LOG_DEBUG("Started management for window %u (handle %lx)", win, h);
    TRACE_ONLY(debug_dump_focus_history(s, "after manage_start"));
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

            if (r->layer != -1) {
                hot->base_layer = (uint8_t)r->layer;
                if (hot->layer != LAYER_FULLSCREEN) {
                    hot->layer = client_layer_from_state(hot);
                }
            }

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
    TRACE_LOG("finish_manage h=%lx xid=%u desktop=%d sticky=%d initial_state=%u", h, hot->xid, hot->desktop,
              hot->sticky, hot->initial_state);

    client_apply_rules(s, h);

    wm_place_window(s, h);

    // 1. Create frame
    rect_t geom = hot->desired;

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[3];

    // Set background to inactive color initially
    values[0] = 0x333333;

    values[1] = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE |
                XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW;

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

    // 2. Add to SaveSet (crash safety)
    xcb_change_save_set(s->conn, XCB_SET_MODE_INSERT, hot->xid);

    // 3. Reparent
    xcb_reparent_window(s->conn, hot->xid, hot->frame, bw, th);

    hot->server = hot->desired;
    wm_send_synthetic_configure(s, h);

    // Set _NET_FRAME_EXTENTS (before mapping)
    uint32_t extents[4] = {bw, bw, th + bw, bw};
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 32, 4,
                        extents);

    if (hot->window_opacity_valid) {
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->frame, atoms._NET_WM_WINDOW_OPACITY, XCB_ATOM_CARDINAL,
                            32, 1, &hot->window_opacity);
    }

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

    // 4. Map if visible on current desktop and not requested to start iconic
    bool visible = (hot->sticky || (hot->desktop == (int32_t)s->current_desktop)) &&
                   (hot->initial_state != XCB_ICCCM_WM_STATE_ICONIC);

    TRACE_LOG("finish_manage visibility h=%lx visible=%d current_desktop=%u", h, visible, s->current_desktop);

    if (visible) {
        xcb_map_window(s->conn, hot->xid);
        xcb_map_window(s->conn, hot->frame);
        hot->state = STATE_MAPPED;

        uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                            state_vals);
    } else {
        hot->state = STATE_UNMAPPED;

        uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                            state_vals);
    }

    bool hidden_by_show_desktop = false;
    if (s->showing_desktop && hot->state == STATE_MAPPED && should_hide_for_show_desktop(hot)) {
        hot->show_desktop_hidden = true;
        TRACE_LOG("finish_manage hide for show_desktop h=%lx xid=%u", h, hot->xid);
        wm_client_iconify(s, h);
        hidden_by_show_desktop = true;
    }

    hot->dirty |= DIRTY_STATE;

    // Subscribe to client events
    uint32_t client_events = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_ENTER_WINDOW |
                             XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(s->conn, hot->xid, XCB_CW_EVENT_MASK, &client_events);

    if (s->damage_supported) {
        hot->damage = xcb_generate_id(s->conn);
        xcb_damage_create(s->conn, hot->damage, hot->xid, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
        dirty_region_reset(&hot->damage_region);
    }

    // Setup passive grabs for click-to-focus and Alt-move/resize
    client_setup_grabs(s, h);

    if (!hidden_by_show_desktop) {
        // Initial stacking
        if (hot->transient_for != HANDLE_INVALID) {
            TRACE_LOG("finish_manage stack above parent h=%lx parent=%lx", h, hot->transient_for);
            stack_place_above(s, h, hot->transient_for);
        } else {
            TRACE_LOG("finish_manage stack raise h=%lx", h);
            stack_raise(s, h);
        }

        // Focus new window if visible and allowed or if nothing is focused
        if (visible && (s->focused_client == HANDLE_INVALID || should_focus_on_map(hot))) {
            TRACE_LOG("finish_manage focus h=%lx", h);
            wm_set_focus(s, h);
        }

        // Draw initial decorations
        frame_redraw(s, h, FRAME_REDRAW_ALL);
    }

    // Add to focus history
    TRACE_ONLY(debug_dump_focus_history(s, "before manage insert"));
    if (hot->focus_node.next && hot->focus_node.next != &hot->focus_node) {
        list_remove(&hot->focus_node);
    }
    list_insert(&hot->focus_node, &s->focus_history, s->focus_history.next);
    TRACE_ONLY(debug_dump_focus_history(s, "after manage insert"));

    // Publish initial desktop
    uint32_t desk_prop = (uint32_t)hot->desktop;
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 32, 1,
                        &desk_prop);

    // Mark root properties dirty
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
    TRACE_LOG("unmanage h=%lx frame=%u state=%d ignore_unmap=%u", h, hot->frame, hot->state, hot->ignore_unmap);
    TRACE_ONLY(debug_dump_focus_history(s, "before unmanage"));
    TRACE_ONLY(debug_dump_transients(hot, "before unmanage"));

    // Remove from stacking
    TRACE_LOG("unmanage stack_remove h=%lx layer=%d", h, hot->layer);
    stack_remove(s, h);

    // Unlink from parent
    if (hot->transient_sibling.next && hot->transient_sibling.next != &hot->transient_sibling) {
        TRACE_LOG("unmanage unlink from parent h=%lx", h);
        list_remove(&hot->transient_sibling);
        list_init(&hot->transient_sibling);
    }

    // Unlink children
    while (!list_empty(&hot->transients_head)) {
        list_node_t* node = hot->transients_head.next;
        client_hot_t* child = (client_hot_t*)((char*)node - offsetof(client_hot_t, transient_sibling));
        TRACE_LOG("unmanage unlink child parent=%lx child=%lx", h, child->self);
        child->transient_for = HANDLE_INVALID;
        list_remove(node);
        list_init(node);
    }

    // Remove from focus history
    if (hot->focus_node.next && hot->focus_node.next != &hot->focus_node) {
        TRACE_LOG("unmanage focus_history remove h=%lx node=%p prev=%p next=%p", h, (void*)&hot->focus_node,
                  (void*)hot->focus_node.prev, (void*)hot->focus_node.next);
        list_remove(&hot->focus_node);
        list_init(&hot->focus_node);
    }
    TRACE_ONLY(debug_dump_focus_history(s, "after focus removal"));

    // If focused, pick next
    if (s->focused_client == h) {
        handle_t next_h = HANDLE_INVALID;

        // Prefer parent if still mapped
        if (hot->transient_for != HANDLE_INVALID) {
            client_hot_t* parent = server_chot(s, hot->transient_for);
            if (parent && parent->state == STATE_MAPPED) {
                next_h = hot->transient_for;
            }
        }

        // Fallback to MRU
        if (next_h == HANDLE_INVALID) {
            list_node_t* node = s->focus_history.next;
#ifdef BBOX_DEBUG_TRACE
            int guard = 0;
#endif
            while (node != &s->focus_history) {
                client_hot_t* cand = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));
#ifdef BBOX_DEBUG_TRACE
                if (guard < 64) {
                    TRACE_LOG("unmanage focus scan[%d] node=%p h=%lx xid=%u state=%d", guard, (void*)node, cand->self,
                              cand->xid, cand->state);
                } else if (guard == 64) {
                    TRACE_WARN("unmanage focus scan exceeded 64 entries for h=%lx", h);
                }
                guard++;
#endif
                if (cand->state == STATE_MAPPED) {
                    next_h = cand->self;
                    break;
                }
                node = node->next;
            }
        }

        wm_set_focus(s, next_h);
    }

    // Remove from SaveSet
    xcb_change_save_set(s->conn, XCB_SET_MODE_DELETE, hot->xid);

    // Reparent back to root if window still exists
    if (!destroyed) {
        TRACE_LOG("unmanage reparent xid=%u -> root", hot->xid);
        xcb_reparent_window(s->conn, hot->xid, s->root, hot->server.x, hot->server.y);
    }

    if (hot->damage != XCB_NONE) {
        xcb_damage_destroy(s->conn, hot->damage);
        hot->damage = XCB_NONE;
        dirty_region_reset(&hot->damage_region);
    }

    // Destroy frame
    if (hot->frame != XCB_NONE) {
        TRACE_LOG("unmanage destroy frame=%u", hot->frame);
        xcb_destroy_window(s->conn, hot->frame);
    }

    // Cleanup maps and properties
    if (hot->xid != XCB_NONE) {
        xcb_delete_property(s->conn, hot->xid, atoms.WM_STATE);
        if (!destroyed) {
            xcb_delete_property(s->conn, hot->xid, atoms._NET_WM_DESKTOP);
            xcb_delete_property(s->conn, hot->xid, atoms._NET_WM_STATE);
        }
        hash_map_remove(&s->window_to_client, hot->xid);
    }
    if (hot->frame != XCB_NONE) hash_map_remove(&s->frame_to_client, hot->frame);

    // Free cold data
    arena_destroy(&cold->string_arena);
    render_free(&hot->render_ctx);
    if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);

    // Free slot
    slotmap_free(&s->clients, h);

    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_WORKAREA;
    TRACE_ONLY(debug_dump_focus_history(s, "after unmanage"));
}

void client_close(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    client_cold_t* cold = server_ccold(s, h);
    if (!hot || !cold || hot->state == STATE_DESTROYED || hot->state == STATE_UNMANAGED) return;

    if (cold->protocols & PROTOCOL_PING) {
        xcb_client_message_event_t ping;
        memset(&ping, 0, sizeof(ping));
        ping.response_type = XCB_CLIENT_MESSAGE;
        ping.format = 32;
        ping.window = hot->xid;
        ping.type = atoms.WM_PROTOCOLS;
        ping.data.data32[0] = atoms._NET_WM_PING;
        ping.data.data32[1] = hot->user_time ? hot->user_time : XCB_CURRENT_TIME;
        ping.data.data32[2] = hot->xid;
        xcb_send_event(s->conn, 0, hot->xid, XCB_EVENT_MASK_NO_EVENT, (const char*)&ping);
    }

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

void client_constrain_size(const size_hints_t* s, uint32_t flags, uint16_t* w, uint16_t* h) {
    // Min/Max size
    if (flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
        if (s->min_w > 0 && *w < (uint16_t)s->min_w) *w = (uint16_t)s->min_w;
        if (s->min_h > 0 && *h < (uint16_t)s->min_h) *h = (uint16_t)s->min_h;
    }

    if (flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
        if (s->max_w > 0 && *w > (uint16_t)s->max_w) *w = (uint16_t)s->max_w;
        if (s->max_h > 0 && *h > (uint16_t)s->max_h) *h = (uint16_t)s->max_h;
    }

    // Aspect ratio
    if (flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) {
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
    }

    // Resize increments
    if (flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
        if (s->inc_w > 1) {
            int32_t base_w = (flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) ? s->base_w : (s->min_w > 0 ? s->min_w : 0);
            if (*w > base_w) {
                *w = (uint16_t)(base_w + ((*w - base_w) / s->inc_w) * s->inc_w);
            }
        }
        if (s->inc_h > 1) {
            int32_t base_h = (flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) ? s->base_h : (s->min_h > 0 ? s->min_h : 0);
            if (*h > base_h) {
                *h = (uint16_t)(base_h + ((*h - base_h) / s->inc_h) * s->inc_h);
            }
        }
    }
}

void client_setup_grabs(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    // Grab buttons 1, 2, 3 for click-to-focus and Alt-move/resize
    // We use SYNC mode for the pointer to allow interception of clicks
    uint16_t buttons[] = {1, 2, 3};
    for (int i = 0; i < 3; i++) {
        xcb_grab_button(s->conn, 0, hot->xid, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_NONE, XCB_NONE, buttons[i], XCB_MOD_MASK_ANY);
    }
}
