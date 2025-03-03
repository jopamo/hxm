# System Architecture

## High-Level Architecture

hxm is a single-process X11 window manager built on XCB. It is structured as a
deterministic, tick-driven system with an explicit commit phase.

A single epoll-backed event loop ingests X events, coalesces them per tick,
updates an authoritative in-memory model, and flushes batched X requests to the
X server. The X server is treated as an asynchronous output backend, not as the
source of truth.

Rendering is limited to window frames and the built-in menu UI. hxm does not
perform compositing and is designed to coexist cleanly with external
compositors.

For a step-by-step runtime walkthrough, see `FLOW.md`.

---

## Core Components

### Event Loop and Ingestion
- Single epoll-backed loop
- X events are ingested and bucketed per tick
- Deterministic processing order within each tick
- No blocking X round-trips in hot paths

### X11 Integration
- Fully asynchronous XCB connection management
- Atom resolution and caching
- Targeted ICCCM and EWMH handling
- Late or out-of-order replies handled defensively

### Window Management
- Explicit client lifecycle management
- Stacking-first ordering model
- Predictable focus handling
- Geometry and workspace state managed in-memory
- No implicit or heuristic-driven rearrangement

### Configuration and Theme
- Declarative configuration parsing
- Theme loading with runtime reload support
- Configuration changes applied at commit boundaries

### Rendering and Menu
- Frame rendering only (no client content rendering)
- Minimal redraws driven by internal state changes
- Built-in menu rendered as WM-owned UI

### Diagnostics
- Integrated structured logging
- Runtime counters for debugging and performance analysis
- Signal-driven runtime control and introspection

---

## Data Flow

1. X events arrive via the XCB connection.
2. Events are ingested and bucketed for the current tick.
3. The in-memory model is updated:
   - clients
   - stacking order
   - focus state
   - workspaces
4. A single commit phase flushes batched X requests.
5. Optional diagnostics and counters are emitted.

No partial or intermediate state is flushed to X outside the commit phase.

---

## Architectural Decisions

- **Non-compositing by design**  
  Keeps the WM small and predictable and allows clean coexistence with external
  compositors.

- **Fully asynchronous XCB I/O**  
  Avoids blocking round-trips and re-entrancy in hot paths.

- **Deterministic, tick-based processing**  
  Bounds work under load and ensures reproducible behavior during event storms.

- **Authoritative in-memory model**  
  All window manager state is derived from internal data structures, not inferred
  from X server state.

- **Stacking-first semantics**  
  Explicit control over window ordering, with optional lightweight tiling layered
  on top.

---

## Non-Goals

- Built-in compositing or visual effects
- Animation-driven UI behavior
- Implicit, heuristic-based window management policies

Visual effects and presentation timing are expected to be handled by external
compositors.
