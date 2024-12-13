# Repository Guidelines

## Project Structure & Module Organization

- `src/` contains the window manager implementation (event loop, focus, stacking, rendering).
- `include/` holds public and internal headers used across modules.
- `tests/` contains unit and integration-style C tests; `tests/ewmh/` hosts EWMH shell tests and helpers.
- `scripts/` provides developer utilities (Xephyr runner, headless and integration test drivers).
- `data/` includes default configuration and desktop entry files (example: `data/hxm.conf`).
- `FLOW.md` documents the runtime flow and core invariants; refer to it before deep changes.

## Build, Test, and Development Commands

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

## Coding Style & Naming Conventions

- Language: C11; headers live in `include/`, sources in `src/`.
- Indentation: 4 spaces; brace style is K&R (opening brace on the same line).
- Naming: `snake_case` for functions and variables, `UPPER_CASE` for constants and macros.
- No formatter is configured; follow existing patterns in nearby files.

## Testing Guidelines

- Unit tests live in `tests/test_*.c` and are wired into `meson test`.
- Integration/EWMH tests are in `tests/ewmh/` and run via shell scripts.
- No explicit coverage target is documented; run the relevant suite for your changes.

## Commit & Pull Request Guidelines

- Recent history uses Conventional Commits style: `type(scope): summary`
  (examples: `feat(menu): ...`, `refactor(stack): ...`).
- PRs should describe the behavioral change, mention tests run (or why not),
  and link any relevant issues or repro steps.
