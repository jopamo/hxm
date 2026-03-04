/* src/diag.c
 *
 * Diagnostic helpers used for inspecting internal WM structures
 *
 * These functions are compiled only when HXM_DIAG=1
 *
 * They exist to debug corruption and consistency failures that are otherwise
 * difficult to reproduce in a running WM, including:
 * - broken intrusive lists
 * - invalid slotmap handles
 * - stacking order inconsistencies
 * - transient relationship corruption
 * - focus history loops
 *
 * Diagnostics are intentionally defensive because they are often called after
 * corruption has already happened
 *
 * Defensive principles:
 * - validate pointer links before dereferencing
 * - cap traversal lengths to avoid infinite loops and log floods
 * - emit raw pointers, handles, and XIDs for quick cross-correlation
 * - never mutate WM state
 *
 * These routines are safe observers
 * If structures are corrupted they should stop traversal and emit enough data
 * to identify the break without making the situation worse
 */

#include "event.h"
#include "hxm_diag.h"

/*
 * Dump one stacking layer vector
 *
 * Layers are small_vec arrays of client handles, one array per layer
 * This view is useful for debugging stacking anomalies such as unexpected
 * raise/lower behavior, wrong layer assignment, or disappearing windows
 * Common layers include DESKTOP, NORMAL, ABOVE, and FULLSCREEN
 *
 * Safety notes:
 * - vectors are flat arrays and should not loop by themselves
 * - corrupted length fields can still cause absurd traversal counts
 * - traversal is capped to contain damage and surface likely corruption
 */
void diag_dump_layer(const server_t* s, layer_t l, const char* tag) {
  if (!s)
    return;
  const small_vec_t* v = &s->layers[l];

  // Layer vectors should remain bounded, cap traversal to limit log flood
  LOG_DEBUG("stack %s layer=%d count=%zu", tag, l, v->length);

  for (size_t i = 0; i < v->length && i < 64; i++) {
    handle_t h = ptr_to_handle(v->items[i]);
    const client_hot_t* c = server_chot((server_t*)s, h);
    if (!c)
      continue;
    // Print both handle and X identifiers for event-log correlation
    LOG_DEBUG("  [%zu] h=%lx xid=%u frame=%u", i, c->self, c->xid, c->frame);
  }

  if (v->length > 64) {
    // Guard hit usually means suspicious vector length metadata
    LOG_WARN("stack %s layer=%d guard hit at %d, possible loop", tag, l, 64);
  }
}

/*
 * Dump the focus history intrusive list
 *
 * focus_history stores most-recently-focused clients and is used as fallback
 * focus selection when the current focus disappears
 *
 * Data structure:
 * - intrusive circular doubly linked list
 * - each client embeds client_hot_t.focus_node
 * - empty list invariant is head->next == head and head->prev == head
 *
 * Corruption patterns this reveals:
 * - NULL next/prev links
 * - broken circular links
 * - loops that never return to head
 *
 * Traversal is guarded to avoid infinite loops on corrupted link graphs
 */
void diag_dump_focus_history(const server_t* s, const char* tag) {
  if (!s)
    return;
  const list_node_t* head = &s->focus_history;

  // focus_history is circular and must always have non-NULL head links
  if (!head->next || !head->prev) {
    LOG_WARN("focus_history %s: list not initialized", tag);
    return;
  }

  LOG_DEBUG("focus_history %s head=%p next=%p prev=%p", tag, (void*)head, (void*)head->next, (void*)head->prev);
  const list_node_t* node = head->next;
  int guard = 0;
  while (node != head && guard < 128) {
    // Stop early on partially corrupted nodes
    if (!node->next || !node->prev) {
      LOG_WARN("focus_history %s: null link at node=%p", tag, (void*)node);
      break;
    }
    /*
     * Convert intrusive node address back to owning client_hot_t
     * client = node - offsetof(client_hot_t, focus_node)
     */
    const client_hot_t* c = (const client_hot_t*)((const char*)node - offsetof(client_hot_t, focus_node));
    // c used only for debug logging, keep variable to simplify inspection
    (void)c;
    LOG_DEBUG("  [%d] node=%p prev=%p next=%p h=%lx xid=%u state=%d", guard, (void*)node, (void*)node->prev, (void*)node->next, c->self, c->xid, c->state);
    node = node->next;
    guard++;
  }
  if (node != head) {
    // Guard protects against infinite traversal on corrupted circular links
    LOG_WARN("focus_history %s: guard hit at %d, possible loop", tag, guard);
  }
}

/*
 * Dump transient children for a parent client
 *
 * Transient relationships model dialog/popup/modal parent linkage and influence
 * stacking and focus behavior
 *
 * Data structure:
 * - parent owns intrusive list client_hot_t.transients_head
 * - each child links through client_hot_t.transient_sibling
 * - children should agree with transient_for parent references
 *
 * Corruption patterns this reveals:
 * - orphaned transient nodes
 * - broken sibling links
 * - loops that do not return to the list head
 *
 * Traversal is guarded to avoid infinite loops on corrupted link graphs
 */
void diag_dump_transients(const client_hot_t* hot, const char* tag) {
  if (!hot)
    return;
  const list_node_t* head = &hot->transients_head;

  // transients list is circular and must always have non-NULL head links
  if (!head->next || !head->prev) {
    LOG_WARN("transients %s h=%lx: list not initialized", tag, hot->self);
    return;
  }

  LOG_DEBUG("transients %s h=%lx head=%p next=%p prev=%p", tag, hot->self, (void*)head, (void*)head->next, (void*)head->prev);
  const list_node_t* node = head->next;
  int guard = 0;
  while (node != head && guard < 64) {
    // Stop early on partially corrupted nodes
    if (!node->next || !node->prev) {
      LOG_WARN("transients %s: null link at node=%p", tag, (void*)node);
      break;
    }
    /*
     * Convert intrusive node address back to owning client_hot_t
     * client = node - offsetof(client_hot_t, transient_sibling)
     */
    const client_hot_t* c = (const client_hot_t*)((const char*)node - offsetof(client_hot_t, transient_sibling));
    // c used only for debug logging, keep variable to simplify inspection
    (void)c;
    LOG_DEBUG("  [%d] node=%p prev=%p next=%p h=%lx xid=%u state=%d", guard, (void*)node, (void*)node->prev, (void*)node->next, c->self, c->xid, c->state);
    node = node->next;
    guard++;
  }
  if (node != head) {
    // Guard protects against infinite traversal on corrupted circular links
    LOG_WARN("transients %s: guard hit at %d, possible loop", tag, guard);
  }
}
