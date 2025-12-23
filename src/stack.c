/* src/stack.c
 * Window stacking order management
 *
 * Model
 *  - Each client lives in exactly one layer stack (s->layers[layer])
 *  - Within a layer: index 0 is bottom, last index is top
 *  - We keep the in-memory order authoritative and push minimal X restacks
 */
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"

/* Forward */
static void stack_restack(server_t* s, handle_t h);

static inline small_vec_t* layer_vec(server_t* s, int layer) {
    if (!s || layer < 0 || layer >= LAYER_COUNT) return NULL;
    return &s->layers[layer];
}

static inline void mark_stacking_dirty(server_t* s) { s->root_dirty |= ROOT_DIRTY_CLIENT_LIST_STACKING; }

static inline int stack_current_layer(const client_hot_t* c) {
    return (c->stacking_layer >= 0) ? (int)c->stacking_layer : (int)c->layer;
}

static int32_t stack_find_index(const small_vec_t* v, handle_t h) {
    if (!v) return -1;
    for (size_t i = 0; i < v->length; i++) {
        if (ptr_to_handle(v->items[i]) == h) return (int32_t)i;
    }
    return -1;
}

static void stack_update_indices(server_t* s, const small_vec_t* v, size_t start) {
    for (size_t i = start; i < v->length; i++) {
        handle_t h = ptr_to_handle(v->items[i]);
        client_hot_t* hot = server_chot(s, h);
        if (hot) hot->stacking_index = (int32_t)i;
    }
}

static void stack_vec_insert(server_t* s, small_vec_t* v, size_t idx, handle_t h) {
    if (!v) return;
    if (idx > v->length) idx = v->length;
    small_vec_push(v, NULL);
    if (idx + 1 < v->length) {
        memmove(&v->items[idx + 1], &v->items[idx], (v->length - 1 - idx) * sizeof(void*));
    }
    v->items[idx] = handle_to_ptr(h);
    stack_update_indices(s, v, idx);
}

static bool stack_vec_remove(server_t* s, small_vec_t* v, size_t idx) {
    if (!v || idx >= v->length) return false;
    if (idx + 1 < v->length) {
        memmove(&v->items[idx], &v->items[idx + 1], (v->length - idx - 1) * sizeof(void*));
    }
    v->length--;
    if (idx < v->length) {
        stack_update_indices(s, v, idx);
    }
    return true;
}

#ifdef HXM_DEBUG_TRACE
static void debug_dump_layer(const server_t* s, int layer, const char* tag) {
    if (!s || layer < 0 || layer >= LAYER_COUNT) return;

    const small_vec_t* v = &s->layers[layer];
    LOG_DEBUG("stack %s layer=%d count=%zu", tag, layer, v->length);

    for (size_t i = 0; i < v->length && i < 64; i++) {
        handle_t h = ptr_to_handle(v->items[i]);
        const client_hot_t* c = server_chot((server_t*)s, h);
        if (!c) continue;
        LOG_DEBUG("  [%zu] h=%lx xid=%u frame=%u", i, c->self, c->xid, c->frame);
    }

    if (v->length > 64) {
        LOG_WARN("stack %s layer=%d guard hit at %d, possible loop", tag, layer, 64);
    }
}
#endif

void stack_remove(server_t* s, handle_t h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    /* Safety check: if client thinks it is not in any layer, don't try to remove. */
    if (c->stacking_layer < 0 && c->stacking_index < 0) return;

    int layer = stack_current_layer(c);
    small_vec_t* v = layer_vec(s, layer);
    if (!v || v->length == 0) return;

    int32_t idx = c->stacking_index;
    if (idx < 0 || (size_t)idx >= v->length || ptr_to_handle(v->items[idx]) != h) {
        idx = stack_find_index(v, h);
        if (idx < 0) return;
    }

    TRACE_LOG("stack_remove h=%lx layer=%d index=%d", h, layer, idx);
    TRACE_ONLY(debug_dump_layer(s, layer, "before remove"));

    stack_vec_remove(s, v, (size_t)idx);
    c->stacking_index = -1;
    c->stacking_layer = -1;

    mark_stacking_dirty(s);
    TRACE_ONLY(debug_dump_layer(s, layer, "after remove"));
}

static void stack_insert_top(server_t* s, client_hot_t* c, int layer) {
    small_vec_t* v = layer_vec(s, layer);
    if (!v) return;
    stack_vec_insert(s, v, v->length, c->self);
    c->stacking_layer = (int8_t)layer;
    c->stacking_index = (int32_t)(v->length - 1);
    mark_stacking_dirty(s);
}

static void stack_insert_bottom(server_t* s, client_hot_t* c, int layer) {
    small_vec_t* v = layer_vec(s, layer);
    if (!v) return;
    stack_vec_insert(s, v, 0, c->self);
    c->stacking_layer = (int8_t)layer;
    c->stacking_index = 0;
    mark_stacking_dirty(s);
}

void stack_raise(server_t* s, handle_t h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    int layer = stack_current_layer(c);
    TRACE_LOG("stack_raise h=%lx layer=%d", h, layer);

    stack_remove(s, h);
    stack_insert_top(s, c, layer);
    TRACE_ONLY(debug_dump_layer(s, layer, "after raise"));

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
    stack_insert_top(s, c, c->layer);
    TRACE_ONLY(debug_dump_layer(s, c->layer, "after move"));
}

void stack_lower(server_t* s, handle_t h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    int layer = stack_current_layer(c);
    TRACE_LOG("stack_lower h=%lx layer=%d", h, layer);

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
    stack_insert_bottom(s, c, layer);
    TRACE_ONLY(debug_dump_layer(s, layer, "after lower"));

    stack_restack(s, h);
}

void stack_place_above(server_t* s, handle_t h, handle_t sibling_h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    client_hot_t* sib = server_chot(s, sibling_h);
    if (!c || !sib) return;

    int layer = stack_current_layer(c);
    int sib_layer = stack_current_layer(sib);
    TRACE_LOG("stack_place_above h=%lx sib=%lx layer=%d", h, sibling_h, layer);

    /* Different lists means no meaningful "immediately above" */
    if (layer != sib_layer) {
        stack_raise(s, h);
        return;
    }

    stack_remove(s, h);

    small_vec_t* v = layer_vec(s, layer);
    if (!v) return; /* Should be covered by stack_remove or earlier logic, but for safety */

    int32_t sib_idx = sib->stacking_index;
    if (sib_idx < 0 || (size_t)sib_idx >= v->length || ptr_to_handle(v->items[sib_idx]) != sibling_h) {
        sib_idx = stack_find_index(v, sibling_h);
        if (sib_idx < 0) {
            stack_raise(s, h);
            return;
        }
    }

    /* Insert after sibling */
    size_t new_idx = (size_t)sib_idx + 1;
    stack_vec_insert(s, v, new_idx, h);
    c->stacking_layer = (int8_t)layer;
    c->stacking_index = (int32_t)new_idx;
    mark_stacking_dirty(s);
    TRACE_ONLY(debug_dump_layer(s, layer, "after place_above"));

    stack_restack(s, h);
}

void stack_place_below(server_t* s, handle_t h, handle_t sibling_h) {
    if (!s) return;

    client_hot_t* c = server_chot(s, h);
    client_hot_t* sib = server_chot(s, sibling_h);
    if (!c || !sib) return;

    int layer = stack_current_layer(c);
    int sib_layer = stack_current_layer(sib);
    TRACE_LOG("stack_place_below h=%lx sib=%lx layer=%d", h, sibling_h, layer);

    if (layer != sib_layer) {
        stack_lower(s, h);
        return;
    }

    stack_remove(s, h);

    small_vec_t* v = layer_vec(s, layer);
    if (!v) return;

    int32_t sib_idx = sib->stacking_index;
    if (sib_idx < 0 || (size_t)sib_idx >= v->length || ptr_to_handle(v->items[sib_idx]) != sibling_h) {
        sib_idx = stack_find_index(v, sibling_h);
        if (sib_idx < 0) {
            stack_lower(s, h);
            return;
        }
    }

    /* Insert before sibling */
    size_t new_idx = (size_t)sib_idx;
    stack_vec_insert(s, v, new_idx, h);
    c->stacking_layer = (int8_t)layer;
    c->stacking_index = (int32_t)new_idx;
    mark_stacking_dirty(s);
    TRACE_ONLY(debug_dump_layer(s, layer, "after place_below"));

    stack_restack(s, h);
}

static xcb_window_t find_window_below(server_t* s, client_hot_t* c) {
    /* Same layer: previous node is immediately below */
    int layer = stack_current_layer(c);
    small_vec_t* v = layer_vec(s, layer);
    if (v) {
        int32_t idx = c->stacking_index;
        if (idx < 0 || (size_t)idx >= v->length || ptr_to_handle(v->items[idx]) != c->self) {
            idx = stack_find_index(v, c->self);
        }
        if (idx > 0) {
            handle_t below_h = ptr_to_handle(v->items[idx - 1]);
            client_hot_t* below = server_chot(s, below_h);
            if (below) return below->frame;
        }
    }

    /* Lower layers: topmost of the nearest non-empty lower layer */
    for (int l = layer - 1; l >= 0; l--) {
        small_vec_t* lv = layer_vec(s, l);
        if (lv && lv->length > 0) {
            handle_t below_h = ptr_to_handle(lv->items[lv->length - 1]);
            client_hot_t* below = server_chot(s, below_h);
            if (below) return below->frame;
        }
    }

    return XCB_NONE;
}

static xcb_window_t find_window_above(server_t* s, client_hot_t* c) {
    /* Same layer: next node is immediately above */
    int layer = stack_current_layer(c);
    small_vec_t* v = layer_vec(s, layer);
    if (v) {
        int32_t idx = c->stacking_index;
        if (idx < 0 || (size_t)idx >= v->length || ptr_to_handle(v->items[idx]) != c->self) {
            idx = stack_find_index(v, c->self);
        }
        if (idx >= 0 && (size_t)(idx + 1) < v->length) {
            handle_t above_h = ptr_to_handle(v->items[idx + 1]);
            client_hot_t* above = server_chot(s, above_h);
            if (above) return above->frame;
        }
    }

    /* Higher layers: bottommost of the nearest non-empty higher layer */
    for (int l = layer + 1; l < LAYER_COUNT; l++) {
        small_vec_t* lv = layer_vec(s, l);
        if (lv && lv->length > 0) {
            handle_t above_h = ptr_to_handle(lv->items[0]);
            client_hot_t* above = server_chot(s, above_h);
            if (above) return above->frame;
        }
    }

    return XCB_NONE;
}

void stack_sync_to_xcb(server_t* s, handle_t h) {
    if (!s) return;
    assert(s->in_commit_phase);

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
        TRACE_LOG("stack_sync_to_xcb h=%lx frame=%u sibling=%u mode=%u", h, c->frame, sibling, mode);
    });

    xcb_configure_window(s->conn, c->frame, mask, values);
    counters.restacks_applied++;
}

static void stack_restack(server_t* s, handle_t h) {
    client_hot_t* c = server_chot(s, h);
    if (c) c->dirty |= DIRTY_STACK;
}