#include <stdlib.h>

#include "bbox.h"
#include "client.h"
#include "event.h"
#include "wm.h"

static void stack_restack(server_t* s, handle_t h);

#ifdef BBOX_DEBUG_TRACE
static void debug_dump_layer(const server_t* s, int layer, const char* tag) {
    if (!s || layer < 0 || layer >= LAYER_COUNT) return;
    const list_node_t* head = &s->layers[layer];
    LOG_DEBUG("stack %s layer=%d head=%p next=%p prev=%p", tag, layer, (void*)head, (void*)head->next,
              (void*)head->prev);
    const list_node_t* node = head->next;
    int guard = 0;
    while (node != head && guard < 64) {
        const client_hot_t* c = (const client_hot_t*)((const char*)node - offsetof(client_hot_t, stacking_node));
        LOG_DEBUG("  [%d] node=%p prev=%p next=%p h=%lx xid=%u frame=%u", guard, (void*)node, (void*)node->prev,
                  (void*)node->next, c->self, c->xid, c->frame);
        node = node->next;
        guard++;
    }
    if (node != head) {
        LOG_WARN("stack %s layer=%d: guard hit at %d, possible loop", tag, layer, guard);
    }
}
#endif

void stack_remove(server_t* s, handle_t h) {
    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    // Only remove if it's actually in a list (next != NULL)
    // We initialize list nodes to point to themselves?
    // list_remove checks? No, our list_remove assumes valid list.
    // client_manage calls stack_raise which inserts it.
    // client_unmanage calls stack_remove.
    // We should check if it's linked.
    if (c->stacking_node.next && c->stacking_node.next != &c->stacking_node) {
        TRACE_LOG("stack_remove h=%lx layer=%d node=%p", h, c->layer, (void*)&c->stacking_node);
        TRACE_ONLY(debug_dump_layer(s, c->layer, "before remove"));
        list_remove(&c->stacking_node);
        list_init(&c->stacking_node);  // Reset to safe state
        s->root_dirty |= ROOT_DIRTY_CLIENT_LIST_STACKING;
        TRACE_ONLY(debug_dump_layer(s, c->layer, "after remove"));
    }
}

void stack_raise(server_t* s, handle_t h) {
    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    TRACE_LOG("stack_raise h=%lx layer=%d", h, c->layer);
    stack_remove(s, h);

    // Insert at tail of layer (top)
    list_node_t* head = &s->layers[c->layer];
    list_insert(&c->stacking_node, head->prev, head);
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST_STACKING;
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after raise"));

    stack_restack(s, h);

    // Raise transients
    // We want to raise them on top of us.
    // Iterate list.
    // Caution: stack_raise modifies the global stacking list, but not the transients list.
    // So we can iterate safely.
    list_node_t* node = c->transients_head.next;
    while (node != &c->transients_head) {
        client_hot_t* child = (client_hot_t*)((char*)node - offsetof(client_hot_t, transient_sibling));
        stack_raise(s, child->self);
        node = node->next;
    }
}

void stack_move_to_layer(server_t* s, handle_t h) {
    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    TRACE_LOG("stack_move_to_layer h=%lx layer=%d", h, c->layer);
    stack_remove(s, h);

    list_node_t* head = &s->layers[c->layer];
    list_insert(&c->stacking_node, head->prev, head);
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST_STACKING;
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after move"));

    stack_restack(s, h);
}

void stack_lower(server_t* s, handle_t h) {
    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    TRACE_LOG("stack_lower h=%lx layer=%d", h, c->layer);
    // Lower transients first so they end up above us?
    // Wait. stack_lower puts at HEAD (absolute bottom).
    // If we lower T1, T1 is bottom.
    // If we lower A, A is bottom (under T1).
    // So A < T1. Correct.

    list_node_t* node = c->transients_head.next;
    while (node != &c->transients_head) {
        client_hot_t* child = (client_hot_t*)((char*)node - offsetof(client_hot_t, transient_sibling));
        stack_lower(s, child->self);
        node = node->next;
    }

    stack_remove(s, h);

    // Insert at head of layer (bottom)
    list_node_t* head = &s->layers[c->layer];
    list_insert(&c->stacking_node, head, head->next);
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST_STACKING;
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after lower"));

    stack_restack(s, h);
}

void stack_place_above(server_t* s, handle_t h, handle_t sibling_h) {
    client_hot_t* c = server_chot(s, h);
    client_hot_t* sib = server_chot(s, sibling_h);
    if (!c || !sib) return;

    TRACE_LOG("stack_place_above h=%lx sib=%lx layer=%d", h, sibling_h, c->layer);
    // If layers differ, we can't place "immediately above" in the list sense if they are in different lists.
    // We just raise 'c' in its own layer?
    // Or keep default behavior.
    if (c->layer != sib->layer) {
        stack_raise(s, h);
        return;
    }

    stack_remove(s, h);

    // Insert after sibling (above sibling)
    // list_insert(node, prev, next)
    // prev = sibling, next = sibling->next
    list_insert(&c->stacking_node, &sib->stacking_node, sib->stacking_node.next);
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST_STACKING;
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after place_above"));

    stack_restack(s, h);
}

void stack_place_below(server_t* s, handle_t h, handle_t sibling_h) {
    client_hot_t* c = server_chot(s, h);
    client_hot_t* sib = server_chot(s, sibling_h);
    if (!c || !sib) return;

    TRACE_LOG("stack_place_below h=%lx sib=%lx layer=%d", h, sibling_h, c->layer);
    if (c->layer != sib->layer) {
        stack_lower(s, h);
        return;
    }

    stack_remove(s, h);

    list_insert(&c->stacking_node, sib->stacking_node.prev, &sib->stacking_node);
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST_STACKING;
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after place_below"));

    stack_restack(s, h);
}

// Find the XID of the window immediately below 'h' in the stacking order
static xcb_window_t find_window_below(server_t* s, client_hot_t* c) {
    // 1. Check same layer, previous node
    if (c->stacking_node.prev != &s->layers[c->layer]) {
        // There is a window below in the same layer
        // We need to get the client from the list_node
        // offsetof trick
        client_hot_t* below = (client_hot_t*)((char*)c->stacking_node.prev - offsetof(client_hot_t, stacking_node));
        return below->frame;
    }

    // 2. Check lower layers (topmost of lower layer)
    for (int l = c->layer - 1; l >= 0; l--) {
        if (!list_empty(&s->layers[l])) {
            list_node_t* tail = s->layers[l].prev;
            client_hot_t* below = (client_hot_t*)((char*)tail - offsetof(client_hot_t, stacking_node));
            return below->frame;
        }
    }

    return XCB_NONE;
}

// Find the XID of the window immediately above 'h'
static xcb_window_t find_window_above(server_t* s, client_hot_t* c) {
    // 1. Check same layer, next node
    if (c->stacking_node.next != &s->layers[c->layer]) {
        client_hot_t* above = (client_hot_t*)((char*)c->stacking_node.next - offsetof(client_hot_t, stacking_node));
        return above->frame;
    }

    // 2. Check higher layers (bottommost of higher layer)
    for (int l = c->layer + 1; l < LAYER_COUNT; l++) {
        if (!list_empty(&s->layers[l])) {
            list_node_t* head = s->layers[l].next;
            client_hot_t* above = (client_hot_t*)((char*)head - offsetof(client_hot_t, stacking_node));
            return above->frame;
        }
    }

    return XCB_NONE;
}

static void stack_restack(server_t* s, handle_t h) {
    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    // We prefer to stack ABOVE the window below us.
    xcb_window_t sibling = find_window_below(s, c);

    uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
    uint32_t values[2];

    if (sibling != XCB_NONE) {
        mask |= XCB_CONFIG_WINDOW_SIBLING;
        values[0] = sibling;
        values[1] = XCB_STACK_MODE_ABOVE;
    } else {
        // No window below?
        // Check if there is a window above.
        sibling = find_window_above(s, c);
        if (sibling != XCB_NONE) {
            mask |= XCB_CONFIG_WINDOW_SIBLING;
            values[0] = sibling;
            values[1] = XCB_STACK_MODE_BELOW;
        } else {
            // No windows above or below? We are the only window?
            // Or just stack mode BOTTOM (if layer 0) or ...
            // If we are the only window, XCB_STACK_MODE_ABOVE/BELOW without sibling means relative to root's stack?
            // "If sibling is None, the stack_mode is relative to the stack of all windows."
            // So STACK_MODE_BELOW -> Bottom of stack.
            // STACK_MODE_ABOVE -> Top of stack.

            // If we are in a low layer but no other windows exist, we can be at top.
            // But if we add a window later in a higher layer, it will go on top.
            // So just ABOVE is fine if empty.
            values[0] = XCB_STACK_MODE_ABOVE;
        }
    }

    TRACE_ONLY({
        uint32_t stack_mode_val = (mask & XCB_CONFIG_WINDOW_SIBLING) ? values[1] : values[0];
        TRACE_LOG("stack_restack h=%lx frame=%u sibling=%u mode=%u", h, c->frame, sibling, stack_mode_val);
    });
    xcb_configure_window(s->conn, c->frame, mask, values);
    counters.restacks_applied++;
}
