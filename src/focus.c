#include <stdlib.h>

#include "bbox.h"
#include "client.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

void wm_set_focus(server_t* s, handle_t h) {
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

        // Move to MRU head
        if (c->focus_node.next && c->focus_node.next != &c->focus_node) {
            list_remove(&c->focus_node);
        }
        list_insert(&c->focus_node, &s->focus_history, s->focus_history.next);

        // Set X input focus
        if (cold && cold->can_focus) {
            xcb_set_input_focus(s->conn, XCB_INPUT_FOCUS_POINTER_ROOT, c->xid, XCB_CURRENT_TIME);
        }

        // Send WM_TAKE_FOCUS if supported
        if (cold && (cold->protocols & PROTOCOL_TAKE_FOCUS)) {
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
            stack_raise(s, h);
        }

        // Mark deferred update
        s->root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
    } else {
        // Focus root or None
        xcb_set_input_focus(s->conn, XCB_INPUT_FOCUS_POINTER_ROOT, s->root, XCB_CURRENT_TIME);
        s->root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
    }
}
