/* src/focus.c
 * Window focus handling and history
 *
 * Model:
 * - MRU History: We maintain a global Most Recently Used list (`s->focus_history`)
 * - Logical vs Physical: `wm_set_focus` updates the logical state (`s->focused_client`)
 *   The actual X11 `SetInputFocus` request is deferred to the flush phase via `s->root_dirty`
 *   This prevents focus stealing races and flickering
 */

#include <stdlib.h>

#include "client.h"
#include "event.h"
#include "hxm.h"
#include "hxm_diag.h"
#include "wm.h"
#include "xcb_utils.h"

static inline bool list_node_linked(const list_node_t* n) {
    if (!n) return false;
    if (!n->next || !n->prev) return false;
    return !(n->next == n && n->prev == n);
}

void wm_install_client_colormap(server_t* s, client_hot_t* hot) {
    if (!s || !hot) return;

    client_cold_t* cold = server_ccold(s, hot->self);
    bool use_list = cold && cold->colormap_windows && cold->colormap_windows_len > 0;

    if (use_list) {
        for (uint32_t i = 0; i < cold->colormap_windows_len; i++) {
            xcb_window_t win = cold->colormap_windows[i];
            if (win == hot->xid && hot->colormap != XCB_NONE) {
                xcb_install_colormap(s->conn, hot->colormap);
            } else if (win == hot->frame && hot->frame_colormap_owned && hot->frame_colormap != XCB_NONE) {
                xcb_install_colormap(s->conn, hot->frame_colormap);
            }
        }
        return;
    }

    if (hot->colormap != XCB_NONE) {
        xcb_install_colormap(s->conn, hot->colormap);
    }
    if (hot->frame_colormap_owned && hot->frame_colormap != XCB_NONE) {
        xcb_install_colormap(s->conn, hot->frame_colormap);
    }
}

/*
 * wm_set_focus:
 * Update the focused client.
 *
 * Actions:
 * - Update `s->focused_client`
 * - Mark the old and new clients as dirty (for frame redraws)
 * - Move the new client to the head of the MRU focus list
 * - Mark `ROOT_DIRTY_ACTIVE_WINDOW` to trigger the X11 focus update in the flush phase
 */
void wm_set_focus(server_t* s, handle_t h) {
    if (!s) return;
    TRACE_LOG("set_focus from=%lx to=%lx", s->focused_client, h);

    client_hot_t* c = NULL;
    if (h != HANDLE_INVALID) {
        c = server_chot(s, h);
        // Only allow focusing mapped windows
        if (!c || c->state != STATE_MAPPED) return;
    }

    if (s->focused_client == h) return;

    // Unfocus old
    if (s->focused_client != HANDLE_INVALID) {
        client_hot_t* old = server_chot(s, s->focused_client);
        if (old) {
            old->flags &= ~CLIENT_FLAG_FOCUSED;
            old->dirty |= DIRTY_FRAME_STYLE | DIRTY_STATE;
        }
    }
    wm_cancel_interaction(s);
    s->focused_client = h;

    if (c) {
        c->flags |= CLIENT_FLAG_FOCUSED;
        c->dirty |= DIRTY_FRAME_STYLE | DIRTY_STATE;

        // Move to MRU head
        if (list_node_linked(&c->focus_node)) {
            TRACE_LOG("set_focus remove focus_node h=%lx node=%p prev=%p next=%p", h, (void*)&c->focus_node,
                      (void*)c->focus_node.prev, (void*)c->focus_node.next);
            list_remove(&c->focus_node);
        }
        TRACE_ONLY(diag_dump_focus_history(s, "before focus insert"));
        list_insert(&c->focus_node, &s->focus_history, s->focus_history.next);
        TRACE_ONLY(diag_dump_focus_history(s, "after focus insert"));

        if (s->config.focus_raise) {
            TRACE_LOG("set_focus raise h=%lx", h);
            stack_raise(s, h);
        }

        // Mark deferred update
        s->root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
    } else {
        // Focus root or None
        TRACE_LOG("set_focus root");
        s->root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
    }
}
