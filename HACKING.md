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
