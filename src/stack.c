/* src/stack.c
 * Window stacking order management
 *
 * Model
 *  - Each client lives in exactly one layer list (s->layers[layer])
 *  - Within a layer: head->next is bottom, head->prev is top
 *  - We keep the in-memory order authoritative and push minimal X restacks
 */
#include <stddef.h>
#include <stdlib.h>

#include "bbox.h"
#include "client.h"
#include "event.h"
#include "wm.h"

/* Forward */
static void stack_restack(server_t* s, handle_t h);

static inline list_node_t* layer_head(server_t* s, int layer) { return &s->layers[layer]; }

static inline bool node_is_linked(const list_node_t* n) {
    /* Our list nodes are initialized to self-linked sentinels */
    return n && n->next && n->prev && (n->next != n) && (n->prev != n);
}

static inline void mark_stacking_dirty(server_t* s) { s->root_dirty |= ROOT_DIRTY_CLIENT_LIST_STACKING; }

static inline client_hot_t* node_to_client(list_node_t* n) {
    return (client_hot_t*)((char*)n - offsetof(client_hot_t, stacking_node));
}

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
        LOG_WARN("stack %s layer=%d guard hit at %d, possible loop", tag, layer, guard);
    }
}
#endif

void stack_remove(server_t* s, handle_t h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    if (!node_is_linked(&c->stacking_node)) return;

    TRACE_LOG("stack_remove h=%lx layer=%d node=%p", h, c->layer, (void*)&c->stacking_node);
    TRACE_ONLY(debug_dump_layer(s, c->layer, "before remove"));

    list_remove(&c->stacking_node);
    list_init(&c->stacking_node);

    mark_stacking_dirty(s);
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after remove"));
}

static void stack_insert_top(server_t* s, client_hot_t* c) {
    list_node_t* head = layer_head(s, c->layer);
    list_insert(&c->stacking_node, head->prev, head);
    mark_stacking_dirty(s);
}

static void stack_insert_bottom(server_t* s, client_hot_t* c) {
    list_node_t* head = layer_head(s, c->layer);
    list_insert(&c->stacking_node, head, head->next);
    mark_stacking_dirty(s);
}

void stack_raise(server_t* s, handle_t h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    TRACE_LOG("stack_raise h=%lx layer=%d", h, c->layer);

    stack_remove(s, h);
    stack_insert_top(s, c);
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after raise"));

    stack_restack(s, h);

    /* Raise transients on top of parent
     * transients list is independent from stacking list, so it's safe to iterate
     */
    list_node_t* node = c->transients_head.next;
    int guard = 0;
    while (node != &c->transients_head && guard++ < 256) {
        client_hot_t* child = (client_hot_t*)((char*)node - offsetof(client_hot_t, transient_sibling));
        node = node->next;
        stack_raise(s, child->self);
    }

    if (guard >= 256) {
        LOG_WARN("stack_raise h=%lx transient guard hit, possible loop", h);
    }
}

void stack_move_to_layer(server_t* s, handle_t h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    TRACE_LOG("stack_move_to_layer h=%lx layer=%d", h, c->layer);

    stack_remove(s, h);
    stack_insert_top(s, c);
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after move"));

    stack_restack(s, h);
}

void stack_lower(server_t* s, handle_t h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    TRACE_LOG("stack_lower h=%lx layer=%d", h, c->layer);

    /* Lower transients first so they remain above the parent after the parent is lowered */
    list_node_t* node = c->transients_head.next;
    int guard = 0;
    while (node != &c->transients_head && guard++ < 256) {
        client_hot_t* child = (client_hot_t*)((char*)node - offsetof(client_hot_t, transient_sibling));
        node = node->next;
        stack_lower(s, child->self);
    }

    if (guard >= 256) {
        LOG_WARN("stack_lower h=%lx transient guard hit, possible loop", h);
    }

    stack_remove(s, h);
    stack_insert_bottom(s, c);
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after lower"));

    stack_restack(s, h);
}

void stack_place_above(server_t* s, handle_t h, handle_t sibling_h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    client_hot_t* sib = server_chot(s, sibling_h);
    if (!c || !sib) return;

    TRACE_LOG("stack_place_above h=%lx sib=%lx layer=%d", h, sibling_h, c->layer);

    /* Different lists means no meaningful "immediately above" */
    if (c->layer != sib->layer) {
        stack_raise(s, h);
        return;
    }

    stack_remove(s, h);

    /* Insert after sibling */
    list_insert(&c->stacking_node, &sib->stacking_node, sib->stacking_node.next);
    mark_stacking_dirty(s);
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after place_above"));

    stack_restack(s, h);
}

void stack_place_below(server_t* s, handle_t h, handle_t sibling_h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    client_hot_t* sib = server_chot(s, sibling_h);
    if (!c || !sib) return;

    TRACE_LOG("stack_place_below h=%lx sib=%lx layer=%d", h, sibling_h, c->layer);

    if (c->layer != sib->layer) {
        stack_lower(s, h);
        return;
    }

    stack_remove(s, h);

    /* Insert before sibling */
    list_insert(&c->stacking_node, sib->stacking_node.prev, &sib->stacking_node);
    mark_stacking_dirty(s);
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after place_below"));

    stack_restack(s, h);
}

static xcb_window_t find_window_below(server_t* s, client_hot_t* c) {
    /* Same layer: previous node is immediately below */
    const list_node_t* head = layer_head(s, c->layer);
    if (c->stacking_node.prev != head) {
        client_hot_t* below = node_to_client(c->stacking_node.prev);
        return below->frame;
    }

    /* Lower layers: topmost of the nearest non-empty lower layer */
    for (int l = c->layer - 1; l >= 0; l--) {
        if (!list_empty(layer_head(s, l))) {
            list_node_t* tail = s->layers[l].prev;
            client_hot_t* below = node_to_client(tail);
            return below->frame;
        }
    }

    return XCB_NONE;
}

static xcb_window_t find_window_above(server_t* s, client_hot_t* c) {
    /* Same layer: next node is immediately above */
    const list_node_t* head = layer_head(s, c->layer);
    if (c->stacking_node.next != head) {
        client_hot_t* above = node_to_client(c->stacking_node.next);
        return above->frame;
    }

    /* Higher layers: bottommost of the nearest non-empty higher layer */
    for (int l = c->layer + 1; l < LAYER_COUNT; l++) {
        if (!list_empty(layer_head(s, l))) {
            list_node_t* first = s->layers[l].next;
            client_hot_t* above = node_to_client(first);
            return above->frame;
        }
    }

    return XCB_NONE;
}

static void stack_restack(server_t* s, handle_t h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    /* Prefer expressing "above the window below me"
     * This keeps stable ordering when inserting into the middle
     */
    xcb_window_t sibling = find_window_below(s, c);

    uint16_t mask = 0;
    uint32_t values[2] = {0, 0};

    if (sibling != XCB_NONE) {
        mask = XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
        values[0] = sibling;
        values[1] = XCB_STACK_MODE_ABOVE;
    } else {
        /* If there's no below sibling, try anchoring relative to an above sibling */
        sibling = find_window_above(s, c);
        if (sibling != XCB_NONE) {
            mask = XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
            values[0] = sibling;
            values[1] = XCB_STACK_MODE_BELOW;
        } else {
            /* Only window we know about */
            mask = XCB_CONFIG_WINDOW_STACK_MODE;
            values[0] = XCB_STACK_MODE_ABOVE;
        }
    }

    TRACE_ONLY({
        uint32_t mode = (mask & XCB_CONFIG_WINDOW_SIBLING) ? values[1] : values[0];
        TRACE_LOG("stack_restack h=%lx frame=%u sibling=%u mode=%u", h, c->frame, sibling, mode);
    });

    xcb_configure_window(s->conn, c->frame, mask, values);
    counters.restacks_applied++;
}
