/* src/wm_dirty.c
 * Window manager dirty state flushing and property publishing
 *
 * The "Commit Phase":
 * -------------------
 * hxm uses a deferred rendering model. Logic code (event handlers) does not
 * send updates to X11 immediately. Instead, it modifies the internal model
 * (client_hot_t) and sets dirty flags (DIRTY_GEOM, DIRTY_STATE, etc.).
 *
 * At the end of every tick, `wm_flush_dirty` is called. This function:
 *  1. Iterates over all active clients.
 *  2. Resolves conflicting dirty states.
 *  3. Batches XCB requests (ConfigureWindow, ChangeProperty).
 *  4. Updates global properties (ClientList, Workarea).
 *
 * This ensures:
 *  - Visual consistency (no half-applied states).
 *  - Reduced X11 traffic (coalescing multiple geometry changes).
 *  - Correct ordering (stacking changes applied before geometry).
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "event.h"
#include "frame.h"
#include "hxm.h"
#include "wm.h"
#include "wm_internal.h"

static bool wm_client_is_hidden(const server_t* s, const client_hot_t* hot) {
    if (hot->state != STATE_MAPPED) return true;
    if (!hot->sticky && hot->desktop != (int32_t)s->current_desktop) return true;
    return false;
}

static void wm_send_sync_request(server_t* s, const client_hot_t* hot, uint64_t value, uint32_t time) {
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = hot->xid;
    ev.type = atoms.WM_PROTOCOLS;
    ev.data.data32[0] = atoms._NET_WM_SYNC_REQUEST;
    if (time == 0) time = hot->user_time ? hot->user_time : XCB_CURRENT_TIME;
    ev.data.data32[1] = time;
    ev.data.data32[2] = (uint32_t)(value & 0xFFFFFFFFu);
    ev.data.data32[3] = (uint32_t)(value >> 32);
    ev.data.data32[4] = 0;
    xcb_send_event(s->conn, 0, hot->xid, XCB_EVENT_MASK_NO_EVENT, (const char*)&ev);
}

void wm_send_synthetic_configure(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
    uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;
    int16_t client_offset_x = (int16_t)bw;
    int16_t client_offset_y = (int16_t)th;

    if (hot->gtk_frame_extents_set) {
        client_offset_x = 0;
        client_offset_y = 0;
    }

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

    if (hot->last_synthetic_geom.x == ev->x && hot->last_synthetic_geom.y == ev->y &&
        hot->last_synthetic_geom.w == ev->width && hot->last_synthetic_geom.h == ev->height) {
        return;
    }
    hot->last_synthetic_geom.x = ev->x;
    hot->last_synthetic_geom.y = ev->y;
    hot->last_synthetic_geom.w = ev->width;
    hot->last_synthetic_geom.h = ev->height;

    TRACE_LOG("synthetic_configure xid=%u x=%d y=%d w=%u h=%u", hot->xid, ev->x, ev->y, ev->width, ev->height);
    xcb_send_event(s->conn, 0, hot->xid, XCB_EVENT_MASK_STRUCTURE_NOTIFY, buffer);
}

void wm_publish_workarea(server_t* s, const rect_t* wa) {
    if (!s || !wa) return;

    bool changed = (memcmp(&s->workarea, wa, sizeof(rect_t)) != 0);
    s->workarea = *wa;

    if (changed) {
        uint32_t n = s->desktop_count ? s->desktop_count : 1;
        uint32_t* wa_vals = malloc(n * 4 * sizeof(uint32_t));
        if (wa_vals) {
            for (uint32_t i = 0; i < n; i++) {
                wa_vals[i * 4 + 0] = (uint32_t)s->workarea.x;
                wa_vals[i * 4 + 1] = (uint32_t)s->workarea.y;
                wa_vals[i * 4 + 2] = (uint32_t)s->workarea.w;
                wa_vals[i * 4 + 3] = (uint32_t)s->workarea.h;
            }
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_WORKAREA, XCB_ATOM_CARDINAL, 32,
                                n * 4, wa_vals);
            free(wa_vals);
        }
    }

    if (!changed) return;

    // Re-apply workarea-dependent geometry for maximized/fullscreen windows
    for (size_t i = 0; i < s->active_clients.length; i++) {
        handle_t h = ptr_to_handle(s->active_clients.items[i]);
        client_hot_t* hot = server_chot(s, h);
        if (!hot) continue;

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
#ifndef NDEBUG
            for (uint32_t j = 0; j < idx; j++) {
                assert(out[j] != hot->xid);
            }
#endif
            out[idx++] = hot->xid;
        }
    }

    return idx;
}

static uint32_t wm_build_client_list(server_t* s, xcb_window_t* out, uint32_t cap) {
    if (!s || !out || !cap) return 0;

    uint32_t idx = 0;
    for (size_t i = 0; i < s->active_clients.length; i++) {
        handle_t h = ptr_to_handle(s->active_clients.items[i]);
        client_hot_t* hot = server_chot(s, h);
        if (!hot) continue;
        if (hot->state == STATE_UNMANAGING || hot->state == STATE_DESTROYED) continue;

        if (idx >= cap) return idx;
#ifndef NDEBUG
        for (uint32_t j = 0; j < idx; j++) {
            assert(out[j] != hot->xid);
        }
#endif
        out[idx++] = hot->xid;
    }
    return idx;
}

/*
 * wm_flush_dirty:
 * Commit all pending state changes to the X server.
 *
 * Phases:
 * 1. Visibility: Map/Unmap windows based on desktop state.
 * 2. Per-Client Updates: Flush geometry, title, hints, and stacking.
 * 3. Focus Commit: Apply deferred focus changes (SetInputFocus).
 * 4. Root Properties: Update _NET_CLIENT_LIST, WORKAREA, etc.
 *
 * Returns true if any X requests were issued (triggering a flush).
 */
bool wm_flush_dirty(server_t* s, uint64_t now) {
    bool flushed = false;
    s->in_commit_phase = true;

    // 0. Handle new clients ready to be managed
    for (size_t i = 0; i < s->active_clients.length;) {
        void* ptr = s->active_clients.items[i];
        handle_t h = ptr_to_handle(ptr);
        client_hot_t* hot = server_chot(s, h);
        if (hot && hot->state == STATE_READY) {
            client_finish_manage(s, h);
            flushed = true;
        }

        // Safe advance: only increment if the element at 'i' hasn't changed (wasn't swapped out)
        if (i < s->active_clients.length && s->active_clients.items[i] == ptr) {
            i++;
        }
    }

    // 1. Visibility (Map/Unmap) - Must happen before focus
    if (s->root_dirty & ROOT_DIRTY_VISIBILITY) {
        flushed = true;
        for (size_t i = 0; i < s->active_clients.length;) {
            void* ptr = s->active_clients.items[i];
            handle_t h = ptr_to_handle(ptr);
            client_hot_t* c = server_chot(s, h);
            if (!c) {
                if (i < s->active_clients.length && s->active_clients.items[i] == ptr) i++;
                continue;
            }

            if (c->state != STATE_MAPPED) {
                if (i < s->active_clients.length && s->active_clients.items[i] == ptr) i++;
                continue;
            }

            bool visible = c->sticky || (c->desktop == (int32_t)s->current_desktop);
            if (visible) {
                // Ensure mapped
                // We don't track "actually mapped" bit perfectly besides state=MAPPED.
                // But since we use deferred visibility for workspace switch, we assume
                // if it's MAPPED state, it SHOULD be mapped if visible.
                // Ideally we'd check if it's already mapped to avoid redundant
                // requests, but XCB map on mapped window is no-op. However, we want to
                // update WM_STATE property too.

                // Optimization: track visibility in client or just blindly apply?
                // Blindly applying is safer for "commit" style.
                xcb_map_window(s->conn, c->frame);
                uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
                xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                    state_vals);
            } else {
                if (c->ignore_unmap < UINT8_MAX) c->ignore_unmap++;
                xcb_unmap_window(s->conn, c->frame);
                uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};
                xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                    state_vals);
            }

            if (i < s->active_clients.length && s->active_clients.items[i] == ptr) {
                i++;
            }
        }
        s->root_dirty &= ~ROOT_DIRTY_VISIBILITY;
    }

    for (size_t i = 0; i < s->active_clients.length;) {
        void* ptr = s->active_clients.items[i];
        handle_t h = ptr_to_handle(ptr);
        client_hot_t* hot = server_chot(s, h);
        if (!hot) {
            if (i < s->active_clients.length && s->active_clients.items[i] == ptr) i++;
            continue;
        }

        if (hot->dirty == DIRTY_NONE) {
            if (i < s->active_clients.length && s->active_clients.items[i] == ptr) i++;
            continue;
        }

        bool dirty_changed = (hot->dirty != hot->last_log_dirty);
        bool dirty_interesting = (hot->dirty & (DIRTY_GEOM | DIRTY_STACK | DIRTY_STATE));
        if (dirty_changed || dirty_interesting) {
            TRACE_LOG("flush_dirty h=%lx xid=%u dirty=0x%x state=%d", h, hot->xid, hot->dirty, hot->state);
            hot->last_log_dirty = hot->dirty;
        }

        if (hot->state == STATE_UNMANAGING || hot->state == STATE_DESTROYED || hot->state == STATE_NEW) {
            if (i < s->active_clients.length && s->active_clients.items[i] == ptr) i++;
            continue;
        }

        if (hot->dirty & DIRTY_GEOM) {
            bool interactive =
                ((s->interaction_mode == INTERACTION_RESIZE || s->interaction_mode == INTERACTION_MOVE) &&
                 s->interaction_window == hot->frame);

            if (interactive) {
                uint64_t interval = 16666666;  // ~16ms for 60Hz
                if (s->last_interaction_flush > 0 && (now - s->last_interaction_flush) < interval) {
                    uint64_t remaining = interval - (now - s->last_interaction_flush);
                    int ms = (int)(remaining / 1000000) + 1;
                    server_schedule_timer(s, ms);
                    if (i < s->active_clients.length && s->active_clients.items[i] == ptr) i++;
                    continue;
                }
                s->last_interaction_flush = now;
            }

            bool interactive_resize =
                (s->interaction_mode == INTERACTION_RESIZE && s->interaction_window == hot->frame);
            if (interactive_resize && hot->sync_enabled && hot->sync_counter != XCB_NONE) {
                uint64_t sync_value = ++hot->sync_value;
                wm_send_sync_request(s, hot, sync_value, s->interaction_time);
            }

            uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
            uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;

            // Safety: Do not configure if we have no geometry yet.
            if (hot->desired.w == 0 || hot->desired.h == 0) {
                hot->dirty &= ~DIRTY_GEOM;  // Clear dirty flag to prevent spin
                goto end_dirty_geom;
            }

            // Defer configuration until management is complete
            if (hot->state == STATE_NEW) {
                goto end_dirty_geom;
            }

            // Robust clamping: Ensure frame is at least large enough for
            // decorations/buttons and client is never <= 0.
            if (hot->desired.w < MIN_FRAME_SIZE) hot->desired.w = (uint16_t)MIN_FRAME_SIZE;
            if (hot->desired.h < MIN_FRAME_SIZE) hot->desired.h = (uint16_t)MIN_FRAME_SIZE;

            // Apply size hints (increments, aspect ratio, min/max) to ensure we send a valid
            // geometry that the client won't immediately reject.
            client_constrain_size(&hot->hints, hot->hints_flags, &hot->desired.w, &hot->desired.h);

            int32_t frame_x = hot->desired.x;
            int32_t frame_y = hot->desired.y;
            uint32_t frame_w = hot->desired.w;
            uint32_t frame_h = hot->desired.h;

            int32_t client_w_calc = (int32_t)hot->desired.w;
            int32_t client_h_calc = (int32_t)hot->desired.h;

            if (hot->gtk_frame_extents_set) {
                frame_x -= (int32_t)hot->gtk_extents.left;
                frame_y -= (int32_t)hot->gtk_extents.top;

                client_w_calc = frame_w;
                client_h_calc = frame_h;
            } else {
                frame_w += 2 * bw;
                frame_h += th + bw;
                // client_w/h remain equal to desired (content size)
            }

            // Final clamp for client dimensions
            if (client_w_calc < 1) client_w_calc = 1;
            if (client_h_calc < 1) client_h_calc = 1;

            uint32_t client_w = (uint32_t)client_w_calc;
            uint32_t client_h = (uint32_t)client_h_calc;

            TRACE_LOG("apply_geom: frame(%dx%d+%d+%d) extents_set=%d -> client(%dx%d)", frame_w, frame_h, frame_x,
                      frame_y, hot->gtk_frame_extents_set, client_w, client_h);

            bool geom_changed = (hot->server.x != (int16_t)frame_x || hot->server.y != (int16_t)frame_y ||
                                 hot->server.w != (uint16_t)client_w || hot->server.h != (uint16_t)client_h);

            if (geom_changed) {
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
                int32_t local_x = bw;
                int32_t local_y = th;

                if (hot->gtk_frame_extents_set) {
                    local_x = 0;
                    local_y = 0;
                }

                client_values[0] = (uint32_t)local_x;
                client_values[1] = (uint32_t)local_y;
                client_values[2] = client_w;
                client_values[3] = client_h;

                xcb_configure_window(
                    s->conn, hot->xid,
                    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                    client_values);

                // Set _NET_FRAME_EXTENTS
                uint32_t extents[4] = {bw, bw, th + bw, bw};
                if ((hot->flags & CLIENT_FLAG_UNDECORATED) || hot->gtk_frame_extents_set) {
                    extents[0] = 0;
                    extents[1] = 0;
                    extents[2] = 0;
                    extents[3] = 0;
                }
                xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_FRAME_EXTENTS,
                                    XCB_ATOM_CARDINAL, 32, 4, extents);

                // Update server state immediately to ensure redraw uses correct geometry
                hot->server.x = (int16_t)frame_x;
                hot->server.y = (int16_t)frame_y;
                hot->server.w = (uint16_t)client_w;
                hot->server.h = (uint16_t)client_h;

                frame_redraw(s, h, FRAME_REDRAW_ALL);

                LOG_DEBUG("Flushed DIRTY_GEOM for %lx: Frame Global(%d,%d) Client Local(%d,%d) %dx%d", h, frame_x,
                          frame_y, local_x, local_y, client_w, client_h);
                flushed = true;
            } else {
                TRACE_LOG("Skipping DIRTY_GEOM for %lx (unchanged)", h);
            }

            wm_send_synthetic_configure(s, h);
            flushed = true;

            hot->pending = hot->desired;
            hot->pending_epoch++;

            hot->dirty &= ~DIRTY_GEOM;
        }
    end_dirty_geom:

        if (hot->dirty & DIRTY_TITLE) {
            flushed = true;
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_NAME, atoms.UTF8_STRING, 0, 1024).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME,
                            s->txn_id, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_NAME, XCB_ATOM_STRING, 0, 1024).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms.WM_NAME,
                            s->txn_id, wm_handle_reply);

            hot->dirty &= ~DIRTY_TITLE;
        }

        if (hot->dirty & DIRTY_HINTS) {
            flushed = true;
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms.WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 32).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms.WM_NORMAL_HINTS, s->txn_id, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_HINTS, atoms.WM_HINTS, 0, 32).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms.WM_HINTS,
                            s->txn_id, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_COLORMAP_WINDOWS, XCB_ATOM_WINDOW, 0, 64).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms.WM_COLORMAP_WINDOWS, s->txn_id, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms._MOTIF_WM_HINTS, XCB_ATOM_ANY, 0, 5).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._MOTIF_WM_HINTS, s->txn_id, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms._GTK_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 0, 4).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._GTK_FRAME_EXTENTS, s->txn_id, wm_handle_reply);

            hot->dirty &= ~DIRTY_HINTS;
        }

        if (hot->dirty & DIRTY_STRUT) {
            flushed = true;
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 0, 12).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._NET_WM_STRUT_PARTIAL, s->txn_id, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_STRUT, XCB_ATOM_CARDINAL, 0, 4).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_STRUT,
                            s->txn_id, wm_handle_reply);

            hot->dirty &= ~DIRTY_STRUT;
        }

        if (hot->dirty & DIRTY_OPACITY) {
            flushed = true;
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_WINDOW_OPACITY, XCB_ATOM_CARDINAL, 0, 1).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._NET_WM_WINDOW_OPACITY, s->txn_id, wm_handle_reply);

            hot->dirty &= ~DIRTY_OPACITY;
        }

        if (hot->dirty & DIRTY_DESKTOP) {
            flushed = true;
            uint32_t desktop = hot->sticky ? 0xFFFFFFFFu : (uint32_t)hot->desktop;
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 32,
                                1, &desktop);
            hot->dirty &= ~DIRTY_DESKTOP;
        }

        frame_flush(s, h);

        if (hot->dirty & DIRTY_STACK) {
            flushed = true;
            TRACE_LOG("flush_dirty stack h=%lx layer=%d stack_layer=%d", h, hot->layer, hot->stacking_layer);

            // If the client is not in the correct layer in the list, move it.
            // This happens if layer changed but stack_move_to_layer wasn't called
            // (which we shouldn't rely on anymore for X sync). Actually, we modified
            // stack_move_to_layer to NOT sync. But we need to call it if the list is
            // wrong. The list is wrong if hot->layer != hot->stacking_layer
            if (hot->layer != hot->stacking_layer) {
                stack_move_to_layer(s, h);
            }

            stack_sync_to_xcb(s, h);
            hot->dirty &= ~DIRTY_STACK;
        }

        if (hot->dirty & DIRTY_STATE) {
            flushed = true;
            TRACE_LOG(
                "flush_dirty state h=%lx layer=%d above=%d below=%d sticky=%d "
                "max=%d/%d focused=%d",
                h, hot->layer, hot->state_above, hot->state_below, hot->sticky, hot->maximized_horz,
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
            if (hot->skip_taskbar) state_atoms[count++] = atoms._NET_WM_STATE_SKIP_TASKBAR;
            if (hot->skip_pager) state_atoms[count++] = atoms._NET_WM_STATE_SKIP_PAGER;
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

        if (i < s->active_clients.length && s->active_clients.items[i] == ptr) {
            i++;
        }
    }

    // Commit Focus
    if (s->committed_focus != s->initial_focus) {
        // Handle initial condition where they might be different?
        // Just use s->focused_client
    }

    // Check if focus changed from committed state
    // We need to resolve the handle to window
    xcb_window_t desired_focus = XCB_NONE;
    client_hot_t* focus_hot = NULL;
    client_cold_t* focus_cold = NULL;

    if (s->focused_client != HANDLE_INVALID) {
        focus_hot = server_chot(s, s->focused_client);
        focus_cold = server_ccold(s, s->focused_client);
        if (focus_hot && focus_hot->state == STATE_MAPPED) {
            desired_focus = focus_hot->xid;
        } else {
            // Fallback to root if focused client invalid/unmapped
            desired_focus = s->root;
        }
    } else {
        desired_focus = s->root;
    }

    if (desired_focus != s->committed_focus) {
        flushed = true;
        TRACE_LOG("flush_dirty commit focus %u -> %u", s->committed_focus, desired_focus);

        if (desired_focus == s->root) {
            if (s->default_colormap != XCB_NONE) {
                xcb_install_colormap(s->conn, s->default_colormap);
            }
            xcb_set_input_focus(s->conn, XCB_INPUT_FOCUS_POINTER_ROOT, s->root, XCB_CURRENT_TIME);
        } else if (focus_hot) {
            wm_install_client_colormap(s, focus_hot);

            if (focus_cold && focus_cold->can_focus) {
                xcb_set_input_focus(s->conn, XCB_INPUT_FOCUS_POINTER_ROOT, focus_hot->xid, XCB_CURRENT_TIME);
            }

            if (focus_cold && (focus_cold->protocols & PROTOCOL_TAKE_FOCUS)) {
                xcb_client_message_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.response_type = XCB_CLIENT_MESSAGE;
                ev.format = 32;
                ev.window = focus_hot->xid;
                ev.type = atoms.WM_PROTOCOLS;
                ev.data.data32[0] = atoms.WM_TAKE_FOCUS;
                ev.data.data32[1] = focus_hot->user_time ? focus_hot->user_time : XCB_CURRENT_TIME;
                xcb_send_event(s->conn, 0, focus_hot->xid, XCB_EVENT_MASK_NO_EVENT, (const char*)&ev);
            }
        }

        s->committed_focus = desired_focus;
    }

    // Root properties

    if (s->root_dirty & ROOT_DIRTY_ACTIVE_WINDOW) {
        flushed = true;
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
        flushed = true;
        size_t cap = 0;
        for (int l = 0; l < LAYER_COUNT; l++) cap += s->layers[l].length;

        xcb_window_t* wins_stacking =
            cap ? (xcb_window_t*)arena_alloc(&s->tick_arena, cap * sizeof(xcb_window_t)) : NULL;

        uint32_t idx_stacking = 0;
        if (wins_stacking) {
            idx_stacking = wm_build_client_list_stacking(s, wins_stacking, (uint32_t)cap);
        }

        // _NET_CLIENT_LIST: mapping order (slotmap order), minus docks and
        // skip-taskbar/pager clients.
        uint32_t cap_list = (uint32_t)s->active_clients.length;
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
        flushed = true;
        rect_t wa;
        wm_compute_workarea(s, &wa);
        static rl_t rl_wa = {0};
        if (rl_allow(&rl_wa, now, 1000000000)) {
            TRACE_LOG("publish_workarea x=%d y=%d w=%u h=%u", wa.x, wa.y, wa.w, wa.h);
        }
        wm_publish_workarea(s, &wa);

        s->root_dirty &= ~ROOT_DIRTY_WORKAREA;
    }

    if (s->root_dirty & ROOT_DIRTY_CURRENT_DESKTOP) {
        flushed = true;
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, 32,
                            1, &s->current_desktop);
        s->root_dirty &= ~ROOT_DIRTY_CURRENT_DESKTOP;
    }

    if (s->root_dirty & ROOT_DIRTY_SHOWING_DESKTOP) {
        flushed = true;
        uint32_t val = s->showing_desktop ? 1u : 0u;
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_SHOWING_DESKTOP, XCB_ATOM_CARDINAL, 32,
                            1, &val);
        s->root_dirty &= ~ROOT_DIRTY_SHOWING_DESKTOP;
    }

    s->in_commit_phase = false;
    return flushed;
}
