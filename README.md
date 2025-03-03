<div style="background-color:#1e1e1e; padding:1em; display:block; border-radius:8px; margin:0;">
  <img src="assets/hxm-clear.png" alt="hxm logo" width="300" style="display:block; margin:0;">
</div>

hxm is a lightweight, non-compositing X11 window manager built around a
deterministic, tick-based architecture. It prioritizes correctness, low latency,
and predictable behavior under load on modern Linux systems.

hxm uses fully asynchronous XCB I/O, an explicit in-memory model, and batched
commits to the X server. The X server is treated as an output backend, not as the
source of truth.

## Features

- Deterministic tick → update → commit processing model
- Fully asynchronous XCB integration (no blocking round-trips in hot paths)
- Predictable focus, stacking, and workspace behavior
- Targeted ICCCM and EWMH compliance
- Compositor-friendly, non-compositing design (works well with picom)
- Signal-based runtime diagnostics and counters
- Integrated logging designed for long-running stability
- Testable under load (Xvfb-friendly, event-storm resistant)

## Non-Goals

- Built-in compositing or visual effects
- Animation-heavy UI or eye candy
- Implicit or heuristic-driven behavior

Visual effects are expected to be provided by an external compositor.

## Installation / Usage

Install hxm from your distribution if available, or build from source using the
steps in `HACKING.md`.

Run hxm:

```sh
hxm
````

Configure hxm by placing a config file at `~/.config/hxm/hxm.conf` or
`$XDG_CONFIG_HOME/hxm/hxm.conf`.

To start hxm as your window manager, configure your display manager or X startup
scripts accordingly.

For contributor setup, build details, and tests, see `HACKING.md`.
For system architecture and design rationale, see `DESIGN.md`.

## License

TBD
