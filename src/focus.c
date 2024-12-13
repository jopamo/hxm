/* src/focus.c
 * Window focus handling and history
 */

#include <stdlib.h>

#include "client.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"
#include "xcb_utils.h"

#ifdef HXM_DEBUG_TRACE
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

/*
static void install_client_colormap(server_t* s, client_hot_t* hot) {
    if (!hot) return;
    if (hot->colormap != XCB_NONE) {
        xcb_install_colormap(s->conn, hot->colormap);
    }
    if (hot->frame_colormap_owned && hot->frame_colormap != XCB_NONE) {
        xcb_install_colormap(s->conn, hot->frame_colormap);
    }
}
*/

void wm_set_focus(server_t* s, handle_t h) {
    TRACE_LOG("set_focus from=%lx to=%lx", s->focused_client, h);
    if (s->focused_client == h) return;

    client_hot_t* c = NULL;
    client_cold_t* cold = NULL;
    if (h != HANDLE_INVALID) {
        c = server_chot(s, h);
        cold = server_ccold(s, h);
        // Only allow focusing mapped windows
        if (!c || c->state != STATE_MAPPED) return;
    }

    // Unfocus old
    if (s->focused_client != HANDLE_INVALID) {
        client_hot_t* old = server_chot(s, s->focused_client);
        if (old) {
            old->flags &= ~CLIENT_FLAG_FOCUSED;
            old->dirty |= DIRTY_FRAME_STYLE | DIRTY_STATE;
        }
    }

    s->focused_client = h;

    if (c) {
        c->flags |= CLIENT_FLAG_FOCUSED;
        c->dirty |= DIRTY_FRAME_STYLE | DIRTY_STATE;

        // install_client_colormap(s, c);

        // Move to MRU head
        if (c->focus_node.next && c->focus_node.next != &c->focus_node) {
            TRACE_LOG("set_focus remove focus_node h=%lx node=%p prev=%p next=%p", h, (void*)&c->focus_node,
                      (void*)c->focus_node.prev, (void*)c->focus_node.next);
            list_remove(&c->focus_node);
        }
        TRACE_ONLY(debug_dump_focus_history(s, "before focus insert"));
        list_insert(&c->focus_node, &s->focus_history, s->focus_history.next);
        TRACE_ONLY(debug_dump_focus_history(s, "after focus insert"));

        // Set X input focus
        if (cold && cold->can_focus) {
            TRACE_LOG("set_focus set_input_focus h=%lx xid=%u", h, c->xid);
            xcb_set_input_focus(s->conn, XCB_INPUT_FOCUS_POINTER_ROOT, c->xid, XCB_CURRENT_TIME);
        }

        // Send WM_TAKE_FOCUS if supported
        if (cold && (cold->protocols & PROTOCOL_TAKE_FOCUS)) {
            TRACE_LOG("set_focus WM_TAKE_FOCUS h=%lx xid=%u", h, c->xid);
            xcb_client_message_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.response_type = XCB_CLIENT_MESSAGE;
            ev.format = 32;
            ev.window = c->xid;
            ev.type = atoms.WM_PROTOCOLS;
            ev.data.data32[0] = atoms.WM_TAKE_FOCUS;
            ev.data.data32[1] = XCB_CURRENT_TIME;

            xcb_send_event(s->conn, 0, c->xid, XCB_EVENT_MASK_NO_EVENT, (const char*)&ev);
        }

        if (s->config.focus_raise) {
            TRACE_LOG("set_focus raise h=%lx", h);
            stack_raise(s, h);
        }

        // Mark deferred update
        s->root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
    } else {
        // Focus root or None
        TRACE_LOG("set_focus root");
        // xcb_install_colormap(s->conn, s->default_colormap);
        xcb_set_input_focus(s->conn, XCB_INPUT_FOCUS_POINTER_ROOT, s->root, XCB_CURRENT_TIME);
        s->root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
    }
}
