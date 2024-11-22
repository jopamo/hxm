/* src/wm_dirty.c
 * Window manager dirty state flushing and property publishing
 */

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "bbox.h"
#include "client.h"
#include "event.h"
#include "frame.h"
#include "wm.h"
#include "wm_internal.h"

static bool wm_client_is_hidden(const server_t* s, const client_hot_t* hot) {
    if (hot->state != STATE_MAPPED) return true;
    if (!hot->sticky && hot->desktop != (int32_t)s->current_desktop) return true;
    return false;
}

void wm_send_synthetic_configure(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
    uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;
    int16_t client_offset_x = (int16_t)bw;
    int16_t client_offset_y = (int16_t)th;
    /*
    if (hot->gtk_frame_extents_set) {
        client_offset_x = 0;
        client_offset_y = 0;
    }
    */

    char buffer[32];
    memset(buffer, 0, sizeof(buffer));
    xcb_configure_notify_event_t* ev = (xcb_configure_notify_event_t*)buffer;

    ev->response_type = XCB_CONFIGURE_NOTIFY;
    ev->event = hot->xid;
    ev->window = hot->xid;
    ev->above_sibling = XCB_NONE;
    ev->x = (int16_t)(hot->server.x + client_offset_x);
    ev->y = (int16_t)(hot->server.y + client_offset_y);
    ev->width = hot->server.w;
    ev->height = hot->server.h;
    ev->border_width = 0;
    ev->override_redirect = hot->override_redirect;

    TRACE_LOG("synthetic_configure xid=%u x=%d y=%d w=%u h=%u", hot->xid, ev->x, ev->y, ev->width, ev->height);
    xcb_send_event(s->conn, 0, hot->xid, XCB_EVENT_MASK_STRUCTURE_NOTIFY, buffer);
}

void wm_publish_workarea(server_t* s, const rect_t* wa) {
    if (!s || !wa) return;

    bool changed = (memcmp(&s->workarea, wa, sizeof(rect_t)) != 0);
    s->workarea = *wa;

    uint32_t n = s->desktop_count ? s->desktop_count : 1;
    uint32_t* wa_vals = malloc(n * 4 * sizeof(uint32_t));
    if (wa_vals) {
        for (uint32_t i = 0; i < n; i++) {
            wa_vals[i * 4 + 0] = (uint32_t)s->workarea.x;
            wa_vals[i * 4 + 1] = (uint32_t)s->workarea.y;
            wa_vals[i * 4 + 2] = (uint32_t)s->workarea.w;
            wa_vals[i * 4 + 3] = (uint32_t)s->workarea.h;
        }
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_WORKAREA, XCB_ATOM_CARDINAL, 32, n * 4,
                            wa_vals);
        free(wa_vals);
    }

    if (!changed) return;

    // Re-apply workarea-dependent geometry for maximized/fullscreen windows
    for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
        if (!slotmap_is_used_idx(&s->clients, i)) continue;
        client_hot_t* hot = (client_hot_t*)slotmap_hot_at(&s->clients, i);
        if (hot->state == STATE_UNMANAGING || hot->state == STATE_DESTROYED) continue;

        if (hot->layer == LAYER_FULLSCREEN && s->config.fullscreen_use_workarea) {
            hot->desired = s->workarea;
            hot->dirty |= DIRTY_GEOM;
        } else if (hot->maximized_horz || hot->maximized_vert) {
            wm_client_set_maximize(s, hot, hot->maximized_horz, hot->maximized_vert);
        }
    }
}

static uint32_t wm_build_client_list_stacking(server_t* s, xcb_window_t* out, uint32_t cap) {
    if (!s || !out || !cap) return 0;

    uint32_t idx = 0;
    for (int l = 0; l < LAYER_COUNT; l++) {
        small_vec_t* v = &s->layers[l];
        for (size_t i = 0; i < v->length; i++) {
            handle_t h = ptr_to_handle(v->items[i]);
            client_hot_t* hot = server_chot(s, h);
            if (!hot) continue;
            if (hot->state == STATE_UNMANAGING || hot->state == STATE_DESTROYED) continue;
            if (idx >= cap) return idx;
            out[idx++] = hot->xid;
        }
    }

    return idx;
}

static uint32_t wm_build_client_list(server_t* s, xcb_window_t* out, uint32_t cap) {
    if (!s || !out || !cap) return 0;

    uint32_t idx = 0;
    for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
        if (!slotmap_is_used_idx(&s->clients, i)) continue;

        client_hot_t* hot = (client_hot_t*)slotmap_hot_at(&s->clients, i);
        if (hot->state == STATE_UNMANAGING || hot->state == STATE_DESTROYED) continue;

        if (idx >= cap) return idx;
        out[idx++] = hot->xid;
    }
    return idx;
}

/*
 * EWMH note
 *  - _NET_CLIENT_LIST is "initial mapping order"
 *  - _NET_CLIENT_LIST_STACKING is bottom-to-top stacking order
 *
 * If you don't maintain a dedicated mapping-order list yet, it's better to publish
 * only the stacking-correct list for _NET_CLIENT_LIST_STACKING and keep _NET_CLIENT_LIST
 * as-is (or omit it) instead of incorrectly reusing stacking order.
 */
void wm_flush_dirty(server_t* s) {
    for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
        if (!slotmap_is_used_idx(&s->clients, i)) continue;

        handle_t h = slotmap_handle_at(&s->clients, i);
        client_hot_t* hot = (client_hot_t*)slotmap_hot_at(&s->clients, i);

        if (hot->dirty == DIRTY_NONE) continue;
        TRACE_LOG("flush_dirty h=%lx xid=%u dirty=0x%x state=%d", h, hot->xid, hot->dirty, hot->state);
        if (hot->state == STATE_UNMANAGING || hot->state == STATE_DESTROYED) continue;

        if (hot->dirty & DIRTY_GEOM) {
            uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
            uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;

            int32_t frame_x = hot->desired.x;
            int32_t frame_y = hot->desired.y;
            uint32_t frame_w = hot->desired.w;
            uint32_t frame_h = hot->desired.h;

            int32_t client_x = bw;
            int32_t client_y = th;
            uint32_t client_w = hot->desired.w;
            uint32_t client_h = hot->desired.h;

            /*
            if (hot->gtk_frame_extents_set) {
                client_x -= (int32_t)hot->gtk_extents.left;
                client_y -= (int32_t)hot->gtk_extents.top;
                client_w += hot->gtk_extents.left + hot->gtk_extents.right;
                client_h += hot->gtk_extents.top + hot->gtk_extents.bottom;
            }
            */

            frame_w += 2 * bw;
            frame_h += th + bw;

            uint32_t frame_values[4];
            frame_values[0] = (uint32_t)frame_x;
            frame_values[1] = (uint32_t)frame_y;
            frame_values[2] = frame_w;
            frame_values[3] = frame_h;

            xcb_configure_window(
                s->conn, hot->frame,
                XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                frame_values);

            uint32_t client_values[4];
            client_values[0] = (uint32_t)client_x;
            client_values[1] = (uint32_t)client_y;
            client_values[2] = client_w;
            client_values[3] = client_h;

            xcb_configure_window(
                s->conn, hot->xid,
                XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                client_values);

            // Set _NET_FRAME_EXTENTS
            // if (hot->flags & CLIENT_FLAG_UNDECORATED) {
            //     xcb_delete_property(s->conn, hot->xid, atoms._NET_FRAME_EXTENTS);
            // } else {
            //     uint32_t extents[4] = {bw, bw, th + bw, bw};
            //     xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_FRAME_EXTENTS,
            //                         XCB_ATOM_CARDINAL, 32, 4, extents);
            // }
            // Original logic above, modified:
            if ((hot->flags & CLIENT_FLAG_UNDECORATED)) {
                xcb_delete_property(s->conn, hot->xid, atoms._NET_FRAME_EXTENTS);
            } else {
                uint32_t extents[4] = {bw, bw, th + bw, bw};
                xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_FRAME_EXTENTS,
                                    XCB_ATOM_CARDINAL, 32, 4, extents);
            }

            // Update server state immediately to ensure redraw uses correct geometry
            hot->server.x = (int16_t)frame_x;
            hot->server.y = (int16_t)frame_y;
            hot->server.w = (uint16_t)client_w;
            hot->server.h = (uint16_t)client_h;

            wm_send_synthetic_configure(s, h);

            frame_redraw(s, h, FRAME_REDRAW_ALL);

            hot->pending = hot->desired;
            hot->pending_epoch++;

            hot->dirty &= ~DIRTY_GEOM;

            LOG_DEBUG("Flushed DIRTY_GEOM for %lx: %d,%d %dx%d", h, hot->pending.x, hot->pending.y, hot->pending.w,
                      hot->pending.h);
        }

        if (hot->dirty & DIRTY_TITLE) {
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_NAME, atoms.UTF8_STRING, 0, 1024).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME,
                            wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_NAME, XCB_ATOM_STRING, 0, 1024).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms.WM_NAME,
                            wm_handle_reply);

            hot->dirty &= ~DIRTY_TITLE;
        }

        if (hot->dirty & DIRTY_HINTS) {
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms.WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 32).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms.WM_NORMAL_HINTS, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_HINTS, atoms.WM_HINTS, 0, 32).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms.WM_HINTS,
                            wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms._MOTIF_WM_HINTS, XCB_ATOM_ANY, 0, 5).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._MOTIF_WM_HINTS, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms._GTK_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 0, 4).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._GTK_FRAME_EXTENTS, wm_handle_reply);

            hot->dirty &= ~DIRTY_HINTS;
        }

        if (hot->dirty & DIRTY_STRUT) {
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 0, 12).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._NET_WM_STRUT_PARTIAL, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_STRUT, XCB_ATOM_CARDINAL, 0, 4).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_STRUT,
                            wm_handle_reply);

            hot->dirty &= ~DIRTY_STRUT;
        }

        if (hot->dirty & DIRTY_OPACITY) {
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_WINDOW_OPACITY, XCB_ATOM_CARDINAL, 0, 1).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._NET_WM_WINDOW_OPACITY, wm_handle_reply);

            hot->dirty &= ~DIRTY_OPACITY;
        }

        if (hot->dirty & DIRTY_FRAME_STYLE) {
            frame_redraw(s, h, FRAME_REDRAW_ALL);
            hot->dirty &= ~DIRTY_FRAME_STYLE;
        }

        if (hot->dirty & DIRTY_STACK) {
            TRACE_LOG("flush_dirty stack_move h=%lx layer=%d", h, hot->layer);
            stack_move_to_layer(s, h);
            hot->dirty &= ~DIRTY_STACK;
        }

        if (hot->dirty & DIRTY_STATE) {
            TRACE_LOG("flush_dirty state h=%lx layer=%d above=%d below=%d sticky=%d max=%d/%d focused=%d", h,
                      hot->layer, hot->state_above, hot->state_below, hot->sticky, hot->maximized_horz,
                      hot->maximized_vert, (hot->flags & CLIENT_FLAG_FOCUSED) != 0);

            xcb_atom_t state_atoms[12];
            uint32_t count = 0;

            if (hot->layer == LAYER_FULLSCREEN) {
                state_atoms[count++] = atoms._NET_WM_STATE_FULLSCREEN;
            }
            if (hot->state_above) {
                state_atoms[count++] = atoms._NET_WM_STATE_ABOVE;
            }
            if (hot->state_below) {
                state_atoms[count++] = atoms._NET_WM_STATE_BELOW;
            }

            if (hot->flags & CLIENT_FLAG_URGENT) state_atoms[count++] = atoms._NET_WM_STATE_DEMANDS_ATTENTION;
            if (hot->sticky) state_atoms[count++] = atoms._NET_WM_STATE_STICKY;
            if (hot->maximized_horz) state_atoms[count++] = atoms._NET_WM_STATE_MAXIMIZED_HORZ;
            if (hot->maximized_vert) state_atoms[count++] = atoms._NET_WM_STATE_MAXIMIZED_VERT;
            if (wm_client_is_hidden(s, hot)) state_atoms[count++] = atoms._NET_WM_STATE_HIDDEN;
            if (hot->flags & CLIENT_FLAG_FOCUSED) state_atoms[count++] = atoms._NET_WM_STATE_FOCUSED;

            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_STATE, XCB_ATOM_ATOM, 32, count,
                                state_atoms);

            // Set _NET_WM_ALLOWED_ACTIONS
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

            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_ALLOWED_ACTIONS, XCB_ATOM_ATOM,
                                32, num_actions, actions);

            hot->dirty &= ~DIRTY_STATE;
        }
    }

    // Root properties

    if (s->root_dirty & ROOT_DIRTY_ACTIVE_WINDOW) {
        if (s->focused_client != HANDLE_INVALID) {
            client_hot_t* c = server_chot(s, s->focused_client);
            if (c) {
                xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW,
                                    32, 1, &c->xid);
            }
        } else {
            xcb_delete_property(s->conn, s->root, atoms._NET_ACTIVE_WINDOW);
        }
        s->root_dirty &= ~ROOT_DIRTY_ACTIVE_WINDOW;
    }

    if (s->root_dirty & (ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING)) {
        size_t cap = 0;
        for (int l = 0; l < LAYER_COUNT; l++) cap += s->layers[l].length;

        xcb_window_t* wins_stacking =
            cap ? (xcb_window_t*)arena_alloc(&s->tick_arena, cap * sizeof(xcb_window_t)) : NULL;

        uint32_t idx_stacking = 0;
        if (wins_stacking) {
            idx_stacking = wm_build_client_list_stacking(s, wins_stacking, (uint32_t)cap);
        }

        // _NET_CLIENT_LIST: mapping order (slotmap order)
        // Must include all managed windows (including iconified ones), so we use slotmap capacity.
        uint32_t cap_list = slotmap_capacity(&s->clients);
        xcb_window_t* wins_list =
            cap_list ? (xcb_window_t*)arena_alloc(&s->tick_arena, cap_list * sizeof(xcb_window_t)) : NULL;

        uint32_t idx_list = 0;
        if (wins_list) {
            idx_list = wm_build_client_list(s, wins_list, cap_list);
        }

        if (s->root_dirty & ROOT_DIRTY_CLIENT_LIST) {
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_CLIENT_LIST, XCB_ATOM_WINDOW, 32,
                                idx_list, wins_list);
        }

        if (s->root_dirty & ROOT_DIRTY_CLIENT_LIST_STACKING) {
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_CLIENT_LIST_STACKING,
                                XCB_ATOM_WINDOW, 32, idx_stacking, wins_stacking);
        }

        s->root_dirty &= ~(ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING);
    }

    if (s->root_dirty & ROOT_DIRTY_WORKAREA) {
        rect_t wa;
        wm_compute_workarea(s, &wa);
        wm_publish_workarea(s, &wa);

        s->root_dirty &= ~ROOT_DIRTY_WORKAREA;
    }

    xcb_flush(s->conn);
}
