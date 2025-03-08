# TODO

Wayland-inspired architectural improvements for hxm.
Focus: determinism, correctness under load, and compositor-friendly behavior (picom).

---

## 1. Enforce explicit commit boundaries everywhere

**Goal:** ensure state is only made visible to X in fully-consistent commits.

### Tasks

* [ ] Audit all XCB flush points
* [ ] Ensure focus changes are staged in the model and applied only at commit
* [ ] Ensure stacking transitions are staged and committed atomically
* [ ] Ensure workspace switches do not expose intermediate states
* [ ] Ensure frame rebuilds never flush partially updated geometry or visuals
* [ ] Add debug assertion: no X requests emitted outside commit phase

### Invariants

* No partial intermediate states are ever flushed
* One logical change == one commit

---

## 2. Introduce serial / tokenized state changes

**Goal:** prevent races caused by late or stale X replies.

### Tasks

* [ ] Add monotonically increasing `uint64_t txn_id` per tick/commit
* [ ] Store `last_applied_txn_id` per client
* [ ] Tag outgoing requests that expect replies with current `txn_id`
* [ ] Ignore or drop replies with stale `txn_id`
* [ ] Log discarded stale replies in debug builds

### Applies to

* property queries
* geometry queries
* strut/workarea probes
* any retry-based logic

---

## 3. Make damage a first-class internal concept

**Goal:** reduce redraw cost and compositor wakeups without adding compositing.

### Tasks

* [ ] Add per-client dirty flags (frame, title, buttons, border)
* [ ] Track damage rectangles for frame redraw
* [ ] Union damage across a tick
* [ ] Redraw only damaged regions during commit
* [ ] Avoid full frame repaint for minor state changes

### Notes

* This applies only to frame/menu rendering
* No X Damage extension handling needed in the WM

---

## 4. Convert input grabs into explicit state machines

**Goal:** eliminate re-entrancy and simplify move/resize correctness.

### Tasks

* [ ] Define explicit interaction state enum
  `none | move | resize(edge) | menu`
* [ ] Capture initial geometry and pointer position on grab start
* [ ] Apply deltas during tick processing
* [ ] End interaction cleanly on button release
* [ ] Cancel interaction on focus loss or client unmap
* [ ] Assert only one interaction state is active at a time

### Benefits

* deterministic behavior
* simpler testing
* easier debugging

---

## 5. Formalize stacking layers with a single manager

**Goal:** centralize all stacking policy and make ordering predictable.

### Tasks

* [ ] Define canonical stacking layers:

  * desktop/background
  * normal
  * above
  * dock/panel
  * overlay/menu
  * fullscreen
* [ ] Ensure every client belongs to exactly one layer
* [ ] Route all raise/lower/restack operations through the layer manager
* [ ] Update `_NET_CLIENT_LIST_STACKING` strictly from this model
* [ ] Add debug checks for illegal cross-layer ordering

---

## 6. Add per-output awareness (multi-monitor correctness)

**Goal:** make multi-monitor behavior explicit and consistent.

### Tasks

* [ ] Track preferred output per client (via geometry center)
* [ ] Compute workareas per output
* [ ] Scope fullscreen state to a specific output
* [ ] Ensure focus follows output changes cleanly
* [ ] Handle output changes without implicit client movement

### Notes

* Uses existing EWMH mechanisms
* No compositor involvement required


---

## 7. Refactor Client Management for Commit Correctness

**Goal:** Ensure client management operations respect the commit phase boundary.

### Tasks

* [ ] Refactor `client_manage_start` to stage requests instead of sending immediately where possible.
* [ ] Audit `client_finish_manage` for direct XCB calls and move to `wm_flush_dirty` logic.
* [ ] Ensure `wm_adopt_children` uses the same staged path as new windows.

---

## 8. Performance and Scalability

**Goal:** Remove remaining blocking calls and optimize hot paths.

### Tasks

* [ ] Cache RandR output state (geometry, crtcs) to eliminate synchronous `xcb_randr_get_screen_resources` in `wm_get_monitor_geometry`.
* [ ] Optimize `wm_flush_dirty` to avoid iterating the entire client list when only root properties change.
* [ ] Implement `txn_id` tracking for serializing state changes (prevent race conditions).

---

## 9. Unify Input Handling

**Goal:** consolidate input logic into a single state machine.

### Tasks

* [ ] Integrate menu input handling (`src/menu.c`) into the main interaction state machine.
* [ ] Remove ad-hoc `if (s->menu.visible)` checks in `wm.c`.
* [ ] Define a clear `INPUT_MODE_MENU` state in `interaction_mode_t`.

---

## 10. Test Coverage

**Goal:** Ensure stability under stress.

### Tasks

* [ ] Add stress tests for rapid window creation/destruction loops (`tests/stress_lifecycle.c`).
* [ ] Add tests for multi-monitor geometry calculations (mocking RandR replies).
* [ ] Add EWMH compliance tests using external tools if feasible.

---

## Guiding principle

> No blocking
> No surprise round-trips
> No hidden re-entrancy

If a change violates any of the above, it’s a regression.
