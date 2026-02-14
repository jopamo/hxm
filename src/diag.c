/* src/diag.c
 * Diagnostics helpers, compiled only when HXM_DIAG=1
 *
 * These routines are intentionally defensive:
 * - validate list head/link pointers
 * - cap traversal to guard against loops/corruption
 * - log enough context (ptrs, handles, xids) to debug quickly
 */

#include "event.h"
#include "hxm_diag.h"

void diag_dump_layer(const server_t* s, layer_t l, const char* tag) {
  if (!s)
    return;
  const small_vec_t* v = &s->layers[l];

  // Layers should be bounded, but cap log spam and detect loops in callers
  LOG_DEBUG("stack %s layer=%d count=%zu", tag, l, v->length);

  for (size_t i = 0; i < v->length && i < 64; i++) {
    handle_t h = ptr_to_handle(v->items[i]);
    const client_hot_t* c = server_chot((server_t*)s, h);
    if (!c)
      continue;
    LOG_DEBUG("  [%zu] h=%lx xid=%u frame=%u", i, c->self, c->xid, c->frame);
  }

  if (v->length > 64) {
    LOG_WARN("stack %s layer=%d guard hit at %d, possible loop", tag, l, 64);
  }
}

void diag_dump_focus_history(const server_t* s, const char* tag) {
  if (!s)
    return;
  const list_node_t* head = &s->focus_history;

  // focus_history is an intrusive circular doubly-linked list
  // head points to itself when empty
  if (!head->next || !head->prev) {
    LOG_WARN("focus_history %s: list not initialized", tag);
    return;
  }

  LOG_DEBUG("focus_history %s head=%p next=%p prev=%p", tag, (void*)head, (void*)head->next, (void*)head->prev);
  const list_node_t* node = head->next;
  int guard = 0;
  while (node != head && guard < 128) {
    // Defensive against partial corruption
    if (!node->next || !node->prev) {
      LOG_WARN("focus_history %s: null link at node=%p", tag, (void*)node);
      break;
    }
    const client_hot_t* c = (const client_hot_t*)((const char*)node - offsetof(client_hot_t, focus_node));
    (void)c;
    LOG_DEBUG("  [%d] node=%p prev=%p next=%p h=%lx xid=%u state=%d", guard, (void*)node, (void*)node->prev, (void*)node->next, c->self, c->xid, c->state);
    node = node->next;
    guard++;
  }
  if (node != head) {
    LOG_WARN("focus_history %s: guard hit at %d, possible loop", tag, guard);
  }
}

void diag_dump_transients(const client_hot_t* hot, const char* tag) {
  if (!hot)
    return;
  const list_node_t* head = &hot->transients_head;

  // transients_head is an intrusive circular list of children linked via
  // transient_sibling
  if (!head->next || !head->prev) {
    LOG_WARN("transients %s h=%lx: list not initialized", tag, hot->self);
    return;
  }

  LOG_DEBUG("transients %s h=%lx head=%p next=%p prev=%p", tag, hot->self, (void*)head, (void*)head->next, (void*)head->prev);
  const list_node_t* node = head->next;
  int guard = 0;
  while (node != head && guard < 64) {
    // Defensive against partial corruption
    if (!node->next || !node->prev) {
      LOG_WARN("transients %s: null link at node=%p", tag, (void*)node);
      break;
    }
    const client_hot_t* c = (const client_hot_t*)((const char*)node - offsetof(client_hot_t, transient_sibling));
    (void)c;
    LOG_DEBUG("  [%d] node=%p prev=%p next=%p h=%lx xid=%u state=%d", guard, (void*)node, (void*)node->prev, (void*)node->next, c->self, c->xid, c->state);
    node = node->next;
    guard++;
  }
  if (node != head) {
    LOG_WARN("transients %s: guard hit at %d, possible loop", tag, guard);
  }
}
