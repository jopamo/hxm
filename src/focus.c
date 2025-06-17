/* src/focus.c
 * Window focus handling and history.
 *
 * Model:
 * - MRU History: We maintain a global Most Recently Used list (`s->focus_history`).
 * - Logical vs Physical: `wm_set_focus` updates the logical state (`s->focused_client`).
 *   The actual X11 `SetInputFocus` request is deferred to the flush phase via `s->root_dirty`.
 *   This prevents focus stealing races and flickering.
 */

#include <stdlib.h>

#include "client.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"
#include "xcb_utils.h"

#ifdef HXM_ENABLE_DEBUG_LOGGING
static void debug_dump_focus_history(const server_t* s, const char* tag) {
    if (!s) return;
    const list_node_t* head = &s->focus_history;
    LOG_DEBUG("focus_history %s head=%p next=%p prev=%p", tag, (void*)head, (void*)head->next, (void*)head->prev);
    const list_node_t* node = head->next;
    int guard = 0;
    while (node != head && guard < 64) {
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
#endif

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
 * 1. Update `s->focused_client`.
 * 2. Mark the old and new clients as dirty (for frame redraws).
 * 3. Move the new client to the head of the MRU focus list.
 * 4. Mark `ROOT_DIRTY_ACTIVE_WINDOW` to trigger the X11 focus update in the flush phase.
 */
void wm_set_focus(server_t* s, handle_t h) {
    TRACE_LOG("set_focus from=%lx to=%lx", s->focused_client, h);
    if (s->focused_client == h) return;

    bool same_focus = (s->focused_client == h);
    if (same_focus && h == HANDLE_INVALID) return;

    client_hot_t* c = NULL;
    if (h != HANDLE_INVALID) {
        c = server_chot(s, h);
        // Only allow focusing mapped windows
        if (!c || c->state != STATE_MAPPED) return;
    }

    if (!same_focus) {
        // Unfocus old
        if (s->focused_client != HANDLE_INVALID) {
            client_hot_t* old = server_chot(s, s->focused_client);
            if (old) {
                old->flags &= ~CLIENT_FLAG_FOCUSED;
                old->dirty |= DIRTY_FRAME_STYLE | DIRTY_STATE;
            }
            wm_cancel_interaction(s);
        }

        s->focused_client = h;
    }

    if (c) {
        c->flags |= CLIENT_FLAG_FOCUSED;
        c->dirty |= DIRTY_FRAME_STYLE | DIRTY_STATE;

        if (!same_focus) {
            // Move to MRU head
            if (c->focus_node.next && c->focus_node.next != &c->focus_node) {
                TRACE_LOG("set_focus remove focus_node h=%lx node=%p prev=%p next=%p", h, (void*)&c->focus_node,
                          (void*)c->focus_node.prev, (void*)c->focus_node.next);
                list_remove(&c->focus_node);
            }
            TRACE_ONLY(debug_dump_focus_history(s, "before focus insert"));
            list_insert(&c->focus_node, &s->focus_history, s->focus_history.next);
            TRACE_ONLY(debug_dump_focus_history(s, "after focus insert"));
        }

        if (s->config.focus_raise && !same_focus) {
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
