# Hacking Guide

## Development Setup

Dependencies:

- C11 compiler (clang or gcc)
- Meson and Ninja
- X11 and XCB libraries (libxcb, xcb-randr, xcb-xkb, and related extensions)
- cairo, pango, fontconfig, and librsvg

Optional tools for scripts and tests:

- Xephyr (for `./scripts/run-xephyr.sh`)
- Xvfb (for `./tests/ewmh/run_in_xvfb.sh`)

For architecture and runtime flow, see `DESIGN.md` and `FLOW.md`.

## Project Structure

- `src/` contains the window manager implementation (event loop, focus,
  stacking, rendering).
- `include/` holds public and internal headers used across modules.
- `tests/` contains unit and integration-style C tests; `tests/ewmh/` hosts EWMH
  shell tests and helpers.
- `scripts/` provides developer utilities (Xephyr runner, headless and
  integration test drivers).
- `data/` includes default configuration and desktop entry files (example:
  `data/hxm.conf`).

## Build and Test

Build with Meson/Ninja:

```bash
meson setup build
meson compile -C build
```

Run locally (on an X11 session):

```bash
./build/hxm
```

Run unit tests registered in Meson:

```bash
meson test -C build
```

Useful scripts:

```bash
./scripts/run-xephyr.sh          # run hxm in a nested Xephyr session
./scripts/test-headless.sh       # headless smoke + stress test
./scripts/test-integration.sh    # integration client checks
./tests/ewmh/run_in_xvfb.sh       # EWMH tests under Xvfb
```

## Coding Standards

- Language: C11; headers live in `include/`, sources in `src/`.
- Indentation: 4 spaces; brace style is K&R (opening brace on the same line).
- Naming: `snake_case` for functions and variables, `UPPER_CASE` for constants
  and macros.
- No formatter is configured; follow existing patterns in nearby files.

## Contribution Flow

- Recent history uses Conventional Commits style: `type(scope): summary`
  (examples: `feat(menu): ...`, `refactor(stack): ...`).
- PRs should describe the behavioral change, mention tests run (or why not),
  and link any relevant issues or repro steps.

## XCB Performance and Correctness Guide

Here is a high-level guide to getting the most out of XCB when building an X11
WM: lowest latency, minimal X server load, maximum correctness under weird
client behavior.

### Core mindset

- Never block in the hot path.
- Batch requests, drain replies opportunistically.
- Treat X as an async message bus.
- Make state transitions explicit and auditable.

### Build a proper event loop around the X fd

#### One loop, many fds

Use one loop (epoll/poll) that waits on:

- XCB connection fd
- timerfd (tick/coalesce)
- signalfd (SIGCHLD/SIGUSR1/etc)
- IPC fd (optional)

#### Do not spin

- Block until readable.
- When readable, drain X events fully.
- Then drain cookie replies (if you keep a cookie jar).
- Then flush once.

Invariant: "No wakeup without doing useful work".

### Drain X events correctly

#### Drain-all pattern

- Call `xcb_poll_for_event()` in a loop until it returns NULL.
- Handle batches of events per wakeup (bounded if you want fairness).

#### Coalesce aggressively

You will see storms of:

- `MotionNotify`
- `ConfigureNotify`
- `PropertyNotify`
- RandR bursts

Coalesce by key:

- `(window, event_type)` last-wins for geometry/property updates.
- Apply in a stable order: lifecycle -> property -> input -> layout -> restack ->
  flush.

This is where "feels smooth" comes from.

### Eliminate synchronous round-trips

#### Never use blocking reply waits in your hot loop

Avoid:

- `xcb_wait_for_reply`
- `xcb_get_*_reply` in-line during event processing

Instead:

- issue request
- store cookie + context
- later, when X fd is readable, poll replies and resolve them

If you must force a reply, do it only in:

- startup probes
- rare debug paths
- slow path with explicit "sync boundary" comments

### Use a cookie jar (reply dispatcher)

A good cookie jar gives you:

- no stalls
- out-of-order reply handling
- bounded memory
- predictable CPU use

Store per request:

- `sequence` (`cookie.sequence`)
- "type" enum (what reply you expect)
- target window/client handle
- callback or small "apply" function
- deadline/timeout policy (optional)

Drain replies only when X fd is readable:

- `xcb_poll_for_reply(conn, seq, &reply, &error)`
- apply, free, recycle slot

Key win: you can pipeline "get attributes -> get properties -> manage window"
without ever blocking.

### Make manage/unmanage fully asynchronous

For managing a new window, do not do a chain of blocking queries. Pipeline:

- `GetWindowAttributes`
- `GetGeometry`
- `GetProperty` (WM_CLASS, WM_HINTS, WM_NORMAL_HINTS, WM_PROTOCOLS, _NET_*)
- optional: `QueryTree` (rare)
- Create frame / reparent / select events
- Map frame + client

All async, with a small per-window "adoption state machine" that progresses as
replies arrive.

Goal: a new window never causes a hitch.

### Use "subscribe once" event masks and avoid over-subscribing

Be deliberate about what you select:

- Root: `SubstructureRedirect`, `SubstructureNotify`, `PropertyChange`,
  `FocusChange`, RandR, etc
- Client: minimal needed set
- Frame: decoration input events, expose, etc

Too many masks = more traffic and more work.

### Minimize Configure churn

Churn is the fastest way to create jank.

Rules:

- Only call `xcb_configure_window` when geometry actually changes.
- Combine flags into one configure per window per tick.
- Prefer layout pass that computes final rects, then applies diffs.
- After applying, send required synthetic `ConfigureNotify` once.

Keep a `dirty` bitset per client:

- position dirty
- size dirty
- stack dirty
- property dirty

Then flush in a deterministic pass.

### Treat stacking and focus as first-class state machines

Make these explicit:

- stacking layer (desktop, below, normal, above, dock)
- stacking index inside layer
- focus history stack
- activation rules (EWMH _NET_ACTIVE_WINDOW vs click-to-focus)

Then you can:

- update `_NET_CLIENT_LIST_STACKING` from your own truth
- produce predictable restacks (one pass)
- avoid "raise storms" that fight clients/panels

### Be strict about properties parsing and memory safety

X properties are untrusted input.

For every `GetProperty` reply:

- validate type/format/length
- clamp sizes
- handle deletion (`PropertyNotify` with delete)
- never assume NUL termination (WM_CLASS, text)
- guard against overflow when parsing `_NET_WM_ICON`

This is how you become "best WM ever" in practice: you do not crash on weird
apps.

### Make RandR a pipeline, not an interrupt

RandR events can be noisy. Strategy:

- coalesce RandR notify bursts
- on "settled" tick, query monitors once
- recompute:
  - monitor geometry
  - workareas/struts
  - fullscreen placements
  - per-desktop viewport (if any)
- Then apply a single layout pass.

### Keep X server load low

- Avoid frequent `QueryTree`.
- Avoid grabbing server (`xcb_grab_server`) except for rare atomic operations.
- Batch property publishes:
  - `_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`, `_NET_WORKAREA` updated once per
    tick
- Avoid repeated `GetProperty` polling; subscribe and react to
  `PropertyNotify` instead.

### Instrument everything like a kernel subsystem

To build something elite, you need visibility:

- per-tick counters: events drained, replies drained, requests queued, flush
  count
- histogram of tick time
- log "sync boundaries" explicitly
- optional tracing ring buffer enabled by SIGUSR1

You will find 90% of "jank" this way.

### Do not fight toolkits; implement the handful of behaviors they assume

The "best WM" is not the one with the most features; it is the one that never
breaks apps.

Prioritize:

- ICCCM basics (WM_STATE, WM_PROTOCOLS, size hints)
- EWMH essentials:
  - `_NET_SUPPORTING_WM_CHECK`
  - `_NET_CLIENT_LIST` / `_NET_CLIENT_LIST_STACKING`
  - `_NET_ACTIVE_WINDOW`
  - `_NET_WM_STATE` (fullscreen, above/below, hidden)
- `_NET_FRAME_EXTENTS`
- `_NET_WM_DESKTOP` + desktops/workarea if you do them

### Have a clear "sync boundary" policy

You will occasionally need to force correctness over latency (rare). Define:

- which actions may block (startup only)
- which must never block (event ingestion, input handling)
- how you test it (CI asserts no blocking calls in hot loop)
