# Hacking Guide

This document describes how to build, test, and modify hxm without violating its
core architectural invariants. Read `DESIGN.md` and `FLOW.md` first.

---

## Development Setup

### Required dependencies

- C11 compiler (clang or gcc)
- Meson and Ninja
- X11 and XCB libraries:
  - libxcb
  - xcb-randr
  - xcb-xkb
  - related XCB extensions
- cairo, pango, fontconfig, librsvg

### Optional tools

- Xephyr (used by `./scripts/run-xephyr.sh`)
- Xvfb (used by `./tests/ewmh/run_in_xvfb.sh`)

---

## Project Structure

- `src/`  
  Core window manager implementation: event loop, focus, stacking, rendering.
- `include/`  
  Public and internal headers shared across modules.
- `tests/`  
  Unit and integration-style C tests.
- `tests/ewmh/`  
  EWMH shell tests and helpers.
- `scripts/`  
  Developer utilities: Xephyr runner, headless and integration test drivers.
- `data/`  
  Default configuration and desktop entry files (for example `data/hxm.conf`).

---

## Build and Test

### Build

```sh
meson setup build
meson compile -C build
````

### Run locally (existing X11 session)

```sh
./build/hxm
```

### Run tests

```sh
meson test -C build
```

### Useful scripts

```sh
./scripts/run-xephyr.sh           # run hxm in a nested Xephyr session
./scripts/test-headless.sh        # headless smoke + stress test
./scripts/test-integration.sh     # integration client checks
./tests/ewmh/run_in_xvfb.sh       # EWMH tests under Xvfb
```

---

## Coding Standards

* Language: C11
* Headers live in `include/`, sources in `src/`
* Indentation: 4 spaces
* Brace style: K&R
* Naming:

  * `snake_case` for functions and variables
  * `UPPER_CASE` for constants and macros
* No auto-formatter is enforced; follow nearby code

---

## Contribution Flow

* Commit messages follow Conventional Commits:

  * `type(scope): summary`
  * examples: `feat(menu): …`, `refactor(stack): …`
* Pull requests should:

  * describe behavioral changes
  * mention tests run (or explain why none)
  * link relevant issues or repro steps

---

## XCB Performance and Correctness Guide

This section documents **non-negotiable engineering rules** for working with XCB
in hxm. Violating these rules will introduce latency, jank, or correctness bugs.

### Core mindset

* Never block in hot paths
* Batch requests, drain replies opportunistically
* Treat X as an asynchronous message bus
* Make state transitions explicit and auditable

---

## Event Loop Design

### One loop, many fds

Use a single event loop (epoll or equivalent) that waits on:

* XCB connection fd
* timerfd (tick / coalescing)
* signalfd (SIGCHLD, SIGUSR1, etc)
* optional IPC fd

### Do not spin

* Block until at least one fd is readable
* When readable:

  1. Drain X events
  2. Drain cookie replies
  3. Process model updates
  4. Flush once

Invariant: **no wakeup without useful work**.

---

## Draining and Coalescing X Events

### Drain-all pattern

* Call `xcb_poll_for_event()` until it returns NULL
* Bound work per tick if fairness is required

### Coalesce aggressively

Expect storms of:

* `MotionNotify`
* `ConfigureNotify`
* `PropertyNotify`
* RandR events

Coalesce rules:

* `(window, event_type)` → last wins
* Geometry and property updates → last wins
* Apply in stable order:

  * lifecycle
  * property
  * input
  * layout
  * restack
  * commit

This is where perceived smoothness comes from.

---

## Eliminate Synchronous Round-Trips

### Forbidden in hot paths

* `xcb_wait_for_reply`
* inline `xcb_get_*_reply` during event handling

### Required pattern

* Issue request
* Store cookie + context
* Drain replies later when X fd is readable

Blocking is permitted **only** in:

* startup probes
* rare debug paths
* explicitly documented slow paths

---

## Cookie Jar (Reply Dispatcher)

A correct cookie jar provides:

* zero stalls
* out-of-order reply handling
* bounded memory
* predictable CPU usage

Per request, store:

* `sequence` (`cookie.sequence`)
* request type enum
* target window or client handle
* callback or apply function
* optional timeout policy

Drain replies using:

```c
xcb_poll_for_reply(conn, seq, &reply, &error)
```

Never block waiting for replies.

---

## Asynchronous Client Management

Client adoption must be fully pipelined.

Do **not** perform blocking query chains.

Pipeline example:

* `GetWindowAttributes`
* `GetGeometry`
* `GetProperty` (WM_CLASS, WM_HINTS, WM_NORMAL_HINTS, WM_PROTOCOLS, `_NET_*`)
* Create frame
* Reparent
* Map frame + client

Progress via a small per-client state machine as replies arrive.

Invariant: **managing a new window must never hitch the WM**.

---

## Event Mask Discipline

Select only what you need:

* Root:

  * `SubstructureRedirect`
  * `SubstructureNotify`
  * `PropertyChange`
  * `FocusChange`
  * RandR events
* Client:

  * minimal required set
* Frame:

  * decoration input
  * expose

Oversubscribing masks increases load and latency.

---

## Minimize Configure Churn

Rules:

* Call `xcb_configure_window` only when geometry actually changes
* Combine flags into one configure per window per tick
* Compute final layout first, then apply diffs
* Send synthetic `ConfigureNotify` once after commit

Maintain per-client dirty flags:

* position
* size
* stack
* properties

Flush in a single deterministic pass.

---

## Stacking and Focus as State Machines

Treat these as explicit systems:

* stacking layer (desktop, below, normal, above, dock)
* stacking index within layer
* MRU focus history
* activation policy (EWMH vs click-to-focus)

Derive:

* `_NET_CLIENT_LIST_STACKING`
* predictable restacks
* minimal raise/lower churn

---

## Property Parsing and Memory Safety

All X properties are untrusted input.

For every `GetProperty` reply:

* validate type, format, and length
* clamp sizes
* handle deletion events
* never assume NUL termination
* guard against overflow (`_NET_WM_ICON`)

Robust parsing is a competitive advantage.

---

## RandR Handling

RandR is noisy. Treat it as a pipeline:

* coalesce notify bursts
* query monitors once when settled
* recompute:

  * monitor geometry
  * workareas and struts
  * fullscreen placement
* apply layout in a single pass

---

## Keep X Server Load Low

* Avoid `QueryTree`
* Avoid `xcb_grab_server` except for rare atomic operations
* Batch root property updates:

  * `_NET_CLIENT_LIST`
  * `_NET_ACTIVE_WINDOW`
  * `_NET_WORKAREA`
* Subscribe to `PropertyNotify` instead of polling

---

## Instrument Like a Kernel Subsystem

Visibility is mandatory.

Maintain:

* per-tick counters
* reply and request counts
* tick time histogram
* explicit sync-boundary logs
* optional tracing ring buffer (signal-triggered)

Most jank is found via instrumentation, not guesswork.

---

## Toolkit Compatibility Rules

Do not fight toolkits.

Minimum required behaviors:

* ICCCM:

  * `WM_STATE`
  * `WM_PROTOCOLS`
  * size hints
* EWMH:

  * `_NET_SUPPORTING_WM_CHECK`
  * `_NET_CLIENT_LIST`
  * `_NET_CLIENT_LIST_STACKING`
  * `_NET_ACTIVE_WINDOW`
  * `_NET_WM_STATE`
  * `_NET_FRAME_EXTENTS`
  * desktops/workarea if implemented

Correctness beats features.

---

## Sync Boundary Policy

Blocking is sometimes necessary, but rare.

Define clearly:

* what may block (startup only)
* what must never block (event ingestion, input handling)
* how this is enforced (debug asserts, CI checks)

If a change crosses a sync boundary, it must be documented.
