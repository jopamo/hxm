/* src/cookie_jar.c
 *
 * Asynchronous XCB reply tracking and dispatch.
 *
 * XCB requests that expect replies return a 32-bit sequence number.
 * The reply arrives later and must be matched against that sequence.
 *
 * The cookie jar maintains a mapping:
 *
 *      sequence -> handler + context
 *
 * allowing the event loop to poll for replies without blocking.
 *
 * ---------------------------------------------------------------------
 * Why this exists
 * ---------------------------------------------------------------------
 *
 * XCB provides two ways to retrieve replies:
 *
 *      synchronous: xcb_*_reply()
 *      asynchronous: xcb_poll_for_reply()
 *
 * Synchronous calls block the thread until the X server responds.
 * In a window manager this is unacceptable because it would stall
 * event processing and freeze the UI.
 *
 * Instead hxm issues requests asynchronously:
 *
 *      seq = xcb_get_property(...)
 *      cookie_jar_push(seq -> handler)
 *
 * Later the event loop calls cookie_jar_drain() to check whether
 * replies are available.
 *
 * ---------------------------------------------------------------------
 * Table design
 * ---------------------------------------------------------------------
 *
 * The cookie jar is implemented as an open-addressed hash table.
 *
 * Properties:
 *
 *      key:        XCB sequence number
 *      probing:    linear probing
 *      deletion:   backshift deletion (no tombstones)
 *      capacity:   power of two
 *
 * Using a power-of-two capacity allows fast indexing:
 *
 *      index = sequence & (cap - 1)
 *
 * avoiding expensive modulo operations.
 *
 * ---------------------------------------------------------------------
 * Probe invariants
 * ---------------------------------------------------------------------
 *
 * Linear probing requires probe chains to remain contiguous.
 * Removing a slot by simply clearing it would break lookups.
 *
 * Therefore deletion uses backshift relocation:
 *
 *      when a slot is removed,
 *      later entries whose home bucket lies before the hole
 *      are shifted backward.
 *
 * This preserves lookup correctness without tombstones.
 *
 * ---------------------------------------------------------------------
 * Fair reply polling
 * ---------------------------------------------------------------------
 *
 * cookie_jar_drain() scans the table starting at scan_cursor.
 *
 * This prevents early buckets from being polled more often than
 * later buckets and avoids starvation when many cookies exist.
 *
 * Work per invocation is bounded by max_replies
 * (defaulting to COOKIE_JAR_MAX_REPLIES_PER_TICK).
 *
 * ---------------------------------------------------------------------
 * Reply readiness hint
 * ---------------------------------------------------------------------
 *
 * The jar tracks a hint (replies_may_exist) indicating that replies
 * might be available in the XCB queue.
 *
 * This hint is set by the event loop when socket reads occur
 * (typically while processing X events).
 *
 * If the hint is not set and no timeouts are pending,
 * reply polling is skipped entirely when hint-based polling
 * is enabled (normal event-loop operation).
 *
 * This reduces overhead when many cookies are pending.
 *
 * ---------------------------------------------------------------------
 * Timeout tracking
 * ---------------------------------------------------------------------
 *
 * Each cookie stores its creation timestamp.
 *
 * Instead of scanning the entire table every tick to find the oldest
 * cookie, the jar maintains:
 *
 *      earliest_cookie_ns
 *
 * This provides an O(1) hint for the next timeout deadline.
 *
 * If the earliest entry is removed, the hint becomes dirty and is
 * recomputed lazily.
 *
 * ---------------------------------------------------------------------
 * Re-entrancy guarantee
 * ---------------------------------------------------------------------
 *
 * A cookie slot is removed before invoking its handler.
 *
 * This guarantees that handlers may safely push new cookies
 * without corrupting probe chains or iterator state.
 *
 * ---------------------------------------------------------------------
 * Memory ownership
 * ---------------------------------------------------------------------
 *
 * XCB allocates reply and error structures returned by
 * xcb_poll_for_reply().
 *
 * These pointers are passed to the handler as borrowed references
 * and are freed here after the handler returns.
 *
 * Handlers must not free them.
 *
 * ---------------------------------------------------------------------
 * Sequence wrap safety
 * ---------------------------------------------------------------------
 *
 * XCB sequence numbers are 32-bit and eventually wrap.
 *
 * Cookies are short-lived and the jar size is small, so a wrapped
 * sequence colliding with a still-live entry is unlikely in practice.
 *
 * ---------------------------------------------------------------------
 * X11 ordering note
 * ---------------------------------------------------------------------
 *
 * This file relies on X11/XCB's request-reply sequencing guarantees:
 * the sequence carried by a reply identifies the request it belongs to,
 * even when unrelated events interleave on the connection.
 *
 * Dispatch order here is driven by table scan position and fairness
 * limits, not by strict sequence order.
 */

#include "cookie_jar.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcbext.h>

#include "hxm.h"

__attribute__((weak)) void* cj_calloc(size_t n, size_t size) {
  return calloc(n, size);
}

// Load factor threshold: grow when live_count/cap >= 0.7
#define COOKIE_JAR_MAX_LOAD_NUM 7
#define COOKIE_JAR_MAX_LOAD_DEN 10

#define COOKIE_JAR_ASSERT(cj)                             \
  do {                                                    \
    assert((cj) != NULL);                                 \
    assert((cj)->slots != NULL);                          \
    assert((cj)->cap >= 2);                               \
    assert((((cj)->cap & ((cj)->cap - 1)) == 0));         \
    assert((cj)->live_count <= (cj)->cap);                \
  } while (0)

static size_t cookie_jar_next_pow2(size_t x) {
  // Round a capacity up to the next power of two.
  // The cookie table always uses power-of-two sizing so
  // index wrapping can use bit masking instead of modulo.
  size_t p = 1;
  while (p < x)
    p <<= 1;
  return p;
}

static inline size_t cookie_home(uint32_t seq, size_t mask) {
  // Compute the home bucket for a sequence number.
  // Sequence numbers are already reasonably distributed so
  // masking is sufficient as a hash function.
  return ((size_t)seq) & mask;
}
static inline size_t cookie_next(size_t i, size_t mask) {
  // Advance to the next slot in the probe chain.
  // Wrapping uses a mask instead of a branch.
  return (i + 1) & mask;
}

static void cookie_jar_refresh_timeout_hint(cookie_jar_t* cj) {
  /*
   * Recompute earliest_cookie_ns if the cached hint is invalid.
   *
   * The jar tracks the timestamp of the oldest cookie to avoid
   * scanning the table every time we compute a timeout.
   *
   * If that entry is removed the hint becomes dirty and is
   * lazily recomputed here.
   */
  COOKIE_JAR_ASSERT(cj);

  if (cj->live_count == 0) {
    cj->earliest_cookie_ns = UINT64_MAX;
    cj->timeout_hint_dirty = false;
    return;
  }

  if (!cj->timeout_hint_dirty && cj->earliest_cookie_ns != UINT64_MAX)
    return;

  uint64_t earliest = UINT64_MAX;
  for (size_t i = 0; i < cj->cap; i++) {
    cookie_slot_t* slot = &cj->slots[i];
    if (!slot->live)
      continue;
    if (slot->timestamp_ns < earliest)
      earliest = slot->timestamp_ns;
  }

  cj->earliest_cookie_ns = earliest;
  cj->timeout_hint_dirty = false;
}

static size_t cookie_jar_probe(const cookie_jar_t* cj, uint32_t seq) {
  /*
   * Locate the slot corresponding to a sequence number.
   *
   * Returns either:
   *      - the slot containing the sequence
   *      - the first empty slot where it should be inserted
   *
   * Linear probing continues until a matching or empty slot
   * is encountered.
   */
  size_t mask = cj->cap - 1;
  size_t i = cookie_home(seq, mask);
  while (cj->slots[i].live && cj->slots[i].sequence != seq) {
    i = cookie_next(i, mask);
  }
  return i;
}

static void cookie_jar_grow(cookie_jar_t* cj, size_t new_cap) {
  /*
   * Resize the cookie table and rehash all live entries.
   *
   * Growth occurs when the load factor exceeds 0.7.
   * Doubling the table size keeps probe chains short.
   *
   * Rehashing is required because bucket positions depend
   * on the table mask (cap - 1).
   */
  COOKIE_JAR_ASSERT(cj);

  new_cap = cookie_jar_next_pow2(new_cap);
  if (new_cap < 2)
    new_cap = 2;

  cookie_slot_t* new_slots = cj_calloc(new_cap, sizeof(*new_slots));
  if (!new_slots) {
    LOG_ERROR("cookie_jar_grow failed");
    exit(1);
  }

  size_t old_cap = cj->cap;
  cookie_slot_t* old_slots = cj->slots;
  bool replies_may_exist = cj->replies_may_exist;

  cj->slots = new_slots;
  cj->cap = new_cap;
  cj->live_count = 0;
  cj->scan_cursor = 0;
  cj->earliest_cookie_ns = UINT64_MAX;
  cj->timeout_hint_dirty = false;
  cj->replies_may_exist = replies_may_exist;

  for (size_t i = 0; i < old_cap; i++) {
    if (!old_slots[i].live)
      continue;

    cookie_slot_t slot = old_slots[i];
    size_t idx = cookie_jar_probe(cj, slot.sequence);
    cj->slots[idx] = slot;
    cj->live_count++;
    if (slot.timestamp_ns < cj->earliest_cookie_ns) {
      cj->earliest_cookie_ns = slot.timestamp_ns;
    }
  }
  free(old_slots);
}

/*
 * Remove a slot while preserving linear probing invariants.
 *
 * Linear probing relies on contiguous probe chains.
 * Simply clearing a slot would break lookups for entries
 * inserted later in the chain.
 *
 * Backshift deletion (Knuth Algorithm R) fixes this by moving
 * forward entries backward when their home bucket lies before
 * the removed position.
 *
 * This restores the probe chain without using tombstones.
 */
static void cookie_jar_remove(cookie_jar_t* cj, size_t idx) {
  COOKIE_JAR_ASSERT(cj);
  assert(idx < cj->cap);
  assert(cj->slots[idx].live);

  size_t mask = cj->cap - 1;
  uint64_t removed_ts = cj->slots[idx].timestamp_ns;
  bool removed_was_earliest = (removed_ts == cj->earliest_cookie_ns);

  cj->slots[idx].live = false;
  cj->slots[idx].handler = NULL;
  cj->live_count--;

  // Backshift deletion to preserve linear-probe invariants
  size_t hole = idx;
  size_t i = cookie_next(hole, mask);

  while (cj->slots[i].live) {
    size_t home = cookie_home(cj->slots[i].sequence, mask);

    bool should_move;
    if (home <= i) {
      should_move = (home <= hole && hole < i);
    }
    else {
      // Home wrapped around end of table
      // Move if hole is in wrapped interval
      should_move = (hole < i) || (home <= hole);
    }

    if (should_move) {
      cj->slots[hole] = cj->slots[i];
      cj->slots[i].live = false;
      cj->slots[i].handler = NULL;
      hole = i;
    }

    i = cookie_next(i, mask);
  }

  if (cj->live_count == 0) {
    cj->earliest_cookie_ns = UINT64_MAX;
    cj->timeout_hint_dirty = false;
  }
  else if (removed_was_earliest) {
    cj->timeout_hint_dirty = true;
  }
}

void cookie_jar_init(cookie_jar_t* cj) {
  assert(cj);

  // Ensure cap is a power of two for fast masking
  size_t cap = COOKIE_JAR_CAP;
  if (cap < 16)
    cap = 16;
  cap = cookie_jar_next_pow2(cap);

  cj->slots = cj_calloc(cap, sizeof(*cj->slots));
  if (!cj->slots) {
    LOG_ERROR("cookie_jar_init failed");
    exit(1);
  }

  cj->cap = cap;
  cj->live_count = 0;
  cj->scan_cursor = 0;
  cj->earliest_cookie_ns = UINT64_MAX;
  cj->timeout_hint_dirty = false;
  cj->replies_may_exist = false;
}

void cookie_jar_destroy(cookie_jar_t* cj) {
  assert(cj);

  free(cj->slots);
  memset(cj, 0, sizeof(*cj));
}

void cookie_jar_push(cookie_jar_t* cj, uint32_t sequence, cookie_type_t type, handle_t client, uintptr_t data, uint64_t txn_id, cookie_handler_fn handler) {
  COOKIE_JAR_ASSERT(cj);
  assert(sequence != 0);
  assert(handler != NULL);

  /*
   * Register a new pending XCB request.
   *
   * Each slot records:
   *      sequence        XCB request sequence
   *      type            request classification
   *      client          owning client handle
   *      data            caller-provided context
   *      timestamp_ns    used for timeout detection
   *      txn_id          higher-level transaction identifier
   *      handler         callback to process the reply
   *
   * If the sequence already exists its metadata is overwritten.
   * This should be rare but prevents duplicate entries.
   */

  // Grow before insertion to preserve probe behavior and keep load bounded.
  if (cj->live_count * COOKIE_JAR_MAX_LOAD_DEN >= cj->cap * COOKIE_JAR_MAX_LOAD_NUM) {
    cookie_jar_grow(cj, cj->cap * 2);
  }

  // If sequence is already present, overwrite its metadata
  size_t idx = cookie_jar_probe(cj, sequence);
  cookie_slot_t* slot = &cj->slots[idx];
  bool replacing = slot->live;
  uint64_t old_ts = slot->timestamp_ns;

  if (!slot->live) {
    cj->live_count++;
  }

  uint64_t now = monotonic_time_ns();
  slot->sequence = sequence;
  slot->type = type;
  slot->client = client;
  slot->data = data;
  slot->timestamp_ns = now;
  slot->txn_id = txn_id;
  slot->handler = handler;
  slot->live = true;

  if (cj->live_count == 1) {
    cj->earliest_cookie_ns = now;
    cj->timeout_hint_dirty = false;
  }
  else {
    assert(cj->earliest_cookie_ns != UINT64_MAX);
    if (replacing && old_ts == cj->earliest_cookie_ns && now > old_ts)
      cj->timeout_hint_dirty = true;
    if (now < cj->earliest_cookie_ns)
      cj->earliest_cookie_ns = now;
  }
}

size_t cookie_jar_remove_client(cookie_jar_t* cj, handle_t client) {
  /*
   * Remove all cookies belonging to a specific client.
   *
   * Used when a client is destroyed or management is aborted.
   *
   * Without this cleanup a delayed reply could reference
   * a freed client handle and corrupt state.
   */
  COOKIE_JAR_ASSERT(cj);
  assert(client != HANDLE_INVALID);

  if (cj->live_count == 0)
    return 0;

  size_t removed = 0;
  for (size_t idx = 0; idx < cj->cap && cj->live_count > 0;) {
    cookie_slot_t* slot = &cj->slots[idx];
    if (slot->live && slot->client == client) {
      cookie_jar_remove(cj, idx);
      removed++;
      continue;
    }
    idx++;
  }

  if (cj->scan_cursor >= cj->cap)
    cj->scan_cursor = 0;

  return removed;
}

void cookie_jar_mark_replies_may_exist(cookie_jar_t* cj) {
  // Mark that replies may be available in the XCB queue.
  // The event loop calls this after reading from the X socket.
  COOKIE_JAR_ASSERT(cj);
  cj->replies_may_exist = true;
}

int32_t cookie_jar_next_timeout_ms(cookie_jar_t* cj, uint64_t now_ns) {
  /*
   * Compute the time until the next cookie timeout.
   *
   * Returns:
   *      -1  -> no cookies pending
   *       0  -> at least one cookie already expired
   *      >0  -> milliseconds until the earliest timeout
   *
   * The result can be used by the main event loop when choosing
   * the next poll/epoll timeout.
   */
  COOKIE_JAR_ASSERT(cj);

  if (cj->live_count == 0)
    return -1;

  cookie_jar_refresh_timeout_hint(cj);
  assert(cj->earliest_cookie_ns != UINT64_MAX);

  uint64_t age_ns = (now_ns >= cj->earliest_cookie_ns) ? (now_ns - cj->earliest_cookie_ns) : 0;
  if (age_ns >= COOKIE_JAR_TIMEOUT_NS)
    return 0;

  uint64_t remaining_ns = COOKIE_JAR_TIMEOUT_NS - age_ns;
  uint64_t timeout_ms = (remaining_ns + 999999ULL) / 1000000ULL;
  if (timeout_ms > (uint64_t)INT32_MAX)
    return INT32_MAX;
  return (int32_t)timeout_ms;
}

/*
 * Process completed replies and expired cookies.
 *
 * This function never blocks.
 *
 * It performs three tasks:
 *      1. poll for completed replies using xcb_poll_for_reply()
 *      2. invoke the registered handler
 *      3. expire requests that exceed COOKIE_JAR_TIMEOUT_NS
 *
 * Reply polling may be skipped entirely when the reply hint
 * indicates no replies are expected and no timeout is due.
 *
 * Work per invocation is bounded by max_replies to prevent
 * cookie handling from monopolizing the event loop.
 *
 * Slots are removed before invoking handlers so callbacks
 * may safely enqueue additional cookies.
 */
void cookie_jar_drain(cookie_jar_t* cj, xcb_connection_t* conn, struct server* s, size_t max_replies) {
  COOKIE_JAR_ASSERT(cj);
  assert(conn);

  if (cj->live_count == 0)
    return;
  if (max_replies == 0)
    max_replies = COOKIE_JAR_MAX_REPLIES_PER_TICK;

  uint64_t now = monotonic_time_ns();
  cookie_jar_refresh_timeout_hint(cj);
  assert(cj->earliest_cookie_ns != UINT64_MAX);

  uint64_t age_ns = (now >= cj->earliest_cookie_ns) ? (now - cj->earliest_cookie_ns) : 0;
  bool timeout_due = (age_ns >= COOKIE_JAR_TIMEOUT_NS);

  bool poll_replies = cj->replies_may_exist;
  if (!poll_replies && !timeout_due)
    return;
  cj->replies_may_exist = false;

  size_t processed = 0;
  size_t scanned = 0;
  size_t idx = cj->scan_cursor;
  size_t mask = cj->cap - 1;
  bool made_reply_progress = false;

  while (scanned < cj->cap && processed < max_replies && cj->live_count > 0) {
    cookie_slot_t* slot = &cj->slots[idx];

    if (slot->live) {
      if (poll_replies) {
        void* reply = NULL;
        xcb_generic_error_t* err = NULL;

        // Returns 1 if a reply or error is ready, 0 otherwise
        int ready = xcb_poll_for_reply(conn, slot->sequence, &reply, &err);

        if (ready) {
          // Remove before invoking handler so re-entrancy can safely push new
          // cookies
          cookie_slot_t local = *slot;
          cookie_jar_remove(cj, idx);

          assert(local.handler);
          local.handler(s, &local, reply, err);

          // Handler receives borrowed pointers; cleanup stays centralized here.
          if (reply)
            free(reply);
          if (err)
            free(err);

          made_reply_progress = true;
          processed++;
          continue;
        }
      }

      uint64_t age_ns = (now >= slot->timestamp_ns) ? (now - slot->timestamp_ns) : 0;
      if (age_ns >= COOKIE_JAR_TIMEOUT_NS) {
        cookie_slot_t local = *slot;
        cookie_jar_remove(cj, idx);

        LOG_WARN("Cookie %u timed out, dropping", local.sequence);
        assert(local.handler);
        local.handler(s, &local, NULL, NULL);

        processed++;
        continue;
      }
    }

    idx = cookie_next(idx, mask);
    scanned++;
  }

  cj->scan_cursor = idx;
  if (poll_replies && made_reply_progress && cj->live_count > 0)
    cj->replies_may_exist = true;
}

void cookie_jar_expire(cookie_jar_t* cj, struct server* s, size_t max_expirations) {
  COOKIE_JAR_ASSERT(cj);

  if (cj->live_count == 0)
    return;
  if (max_expirations == 0)
    max_expirations = COOKIE_JAR_MAX_REPLIES_PER_TICK;

  uint64_t now = monotonic_time_ns();
  cookie_jar_refresh_timeout_hint(cj);
  assert(cj->earliest_cookie_ns != UINT64_MAX);
  uint64_t earliest_age = (now >= cj->earliest_cookie_ns) ? (now - cj->earliest_cookie_ns) : 0;
  if (earliest_age < COOKIE_JAR_TIMEOUT_NS)
    return;

  size_t processed = 0;
  size_t scanned = 0;
  size_t idx = cj->scan_cursor;
  size_t mask = cj->cap - 1;

  while (scanned < cj->cap && processed < max_expirations && cj->live_count > 0) {
    cookie_slot_t* slot = &cj->slots[idx];
    if (slot->live) {
      uint64_t age_ns = (now >= slot->timestamp_ns) ? (now - slot->timestamp_ns) : 0;
      if (age_ns >= COOKIE_JAR_TIMEOUT_NS) {
        cookie_slot_t local = *slot;
        cookie_jar_remove(cj, idx);

        LOG_WARN("Cookie %u timed out, dropping", local.sequence);
        assert(local.handler);
        local.handler(s, &local, NULL, NULL);

        processed++;
        continue;
      }
    }

    idx = cookie_next(idx, mask);
    scanned++;
  }

  cj->scan_cursor = idx;
}
