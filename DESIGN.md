# System Architecture

## High-Level Architecture

hxm is a single-process X11 window manager built on XCB. A single epoll-backed
loop ingests X events, coalesces them, updates an in-memory model, and flushes
batched X requests. Rendering is limited to window frames and menu UI and does
not include compositing.

For a step-by-step runtime walkthrough, see `FLOW.md`.

## Core Components

- Event loop and ingestion: epoll-driven tick loop, event coalescing, and
  deterministic processing order.
- X11 integration: XCB connection management, atoms, and ICCCM/EWMH handling.
- Window management: client lifecycle, stacking, focus, geometry, and workspaces.
- Configuration and theme: config parsing and theme loading with reload support.
- Rendering and menu: frame drawing and the built-in menu.
- Diagnostics: logging, counters, and signal-driven runtime control.

## Data Flow

1. X events arrive via the XCB connection.
2. Events are ingested and bucketed for the current tick.
3. The in-memory model is updated (clients, workspaces, stacking, focus).
4. Batched X requests are flushed to apply changes.
5. Optional diagnostics and counters are emitted for debugging.

## Decision Log

- Non-compositing design to keep the WM small and to coexist with external
  compositors.
- Fully asynchronous XCB I/O to avoid blocking round-trips in hot paths.
- Deterministic, tick-based event processing for predictable behavior under load.
- Stacking-first semantics with optional lightweight tiling for explicit control.
