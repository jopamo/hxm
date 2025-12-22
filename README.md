<div style="background-color:#1e1e1e; padding:1em; display:block; border-radius:8px; margin:0;">
  <img src="assets/hxm-clear.png" alt="hxm-clear logo" width="300" style="display:block; margin:0;">
</div>

**hxm** is a lightweight, non-compositing X11 window manager focused on correctness,
low latency, and predictable behavior on modern Linux systems.

You can think of it as **Openbox-style semantics**, **fvwm-like control**, and
**bspwm-level minimalism**, built on a modern, asynchronous XCB core.

hxm is designed to behave like a well-engineered system component, not a dynamic
desktop environment.

## Design goals

- Minimal, explicit behavior
- No hidden policy or heuristic decision-making
- Fully asynchronous X11 interaction
- Stable and deterministic event ordering under load
- Correct RandR handling for heterogeneous display setups

hxm manages window lifecycle, stacking, focus, geometry, and optional lightweight
tiling. Rendering and compositing are intentionally left to clients and external
compositors.

## Architecture

- **XCB-based**
  - All X11 interaction is asynchronous
  - No blocking round-trips in the hot path

- **Event-driven**
  - Single epoll-based event loop
  - Coalesced input, configure, and property events
  - Deterministic processing order

- **Non-compositing**
  - No OpenGL, no scene graph, no texture management
  - Designed to coexist cleanly with external compositors

- **Stacking-first window management**
  - Traditional overlapping windows and layers
  - Explicit stacking and layer control

- **Optional lightweight tiling**
  - Simple, explicit tiling modes
  - Applied per container or workspace
  - No global layout solver or implicit reflow

- **Advanced multi-monitor support**
  - RandR 1.5 aware
  - Correct handling of mixed DPI and mixed refresh rates

## Features

- Targeted ICCCM and EWMH compliance
- Predictable focus and stacking behavior
- Mixed stacking and tiling workflows
- Signal-based runtime diagnostics (SIGUSR1)
- Integrated logging
- Designed for long-running stability

## Requirements

- Linux
- X11
- XCB libraries
  - libxcb
  - xcb-randr
  - xcb-xkb
  - other standard XCB extensions
- C11 compiler (clang or gcc)
- Meson
- Ninja

## Building

```sh
meson setup build
meson compile -C build
````

## Running

```sh
./build/hxm
```

To use hxm as your window manager, configure your display manager or X startup
scripts accordingly.

## Status

hxm is under active development.
Interfaces may change and some EWMH behavior is still being refined.

The project prioritizes correctness, performance, and architectural clarity over
feature breadth.

## Naming

`hxm` stands for **high-performance X manager**.

The name follows traditional Unix conventions, similar to `twm`, `dwm`, and `cwm`.

## License

TBD
