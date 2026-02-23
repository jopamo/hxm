<div style="background-color:#1e1e1e; padding:1em; display:block; border-radius:8px; margin:0;">
  <img src="assets/hxm-clear.png" alt="hxm logo" width="300" style="display:block; margin:0;">
</div>

## Overview

`hxm` is a small stacking window manager for Linux/X11 written in C and built with Meson. The core of the runtime is a straightforward tick loop: pull in X events, drain any pending async replies, process internal state changes, then flush batched updates back to the X server.

It is not trying to be clever. It is trying to be predictable.

## Why this project exists

X11 makes it easy to write a window manager that mostly works and hard to write one that behaves consistently under load. `hxm` is structured to keep event handling explicit and ordering stable, especially around async `xcb` interactions.

Synchronous `xcb_*_reply` usage is treated as a policy decision, not an accident. There are CI checks and allowlists in place to make sure new sync calls do not quietly creep in.

The goal is tight control over interaction paths and fewer “mystery stalls” when the event stream gets noisy.

## Status

Experimental. The current Meson version is `0.1.0` and `TODO.md` tracks ongoing hardening and cleanup work.

## Features

* A stable event pipeline with clear ingest → process → commit phases.
* Per-tick coalescing so geometry and state changes are flushed in a controlled batch.
* EWMH and ICCCM handling for desktops, focus, and workarea.
* Config and theme parsing for keybindings, rules, and visual settings.
* YAML-driven root menu with icon support and actions.
* Snap-to-edge logic with a click-through preview window.
* Scripted integration and stress testing wired into Meson.
* CI guard scripts that fail on skipped tests and detect sync-reply drift.

## Building

On Fedora, you can install the CI dependencies with:

```sh
sudo dnf install $(tr '\n' ' ' < scripts/deps-fedora.txt)
```

Configure and build:

```sh
meson setup build
meson compile -C build
```

Install:

```sh
meson install -C build
```

Installed targets include:

* The `hxm` binary
* An X session file (`hxm.desktop`)
* A system-wide default config (`hxm.conf`)

Meson option:

* `-Dverbose_logs=true` enables verbose logging (`HXM_VERBOSE_LOGS`).

## Running

Show CLI help:

```sh
./build/hxm --help
```

Supported flags:

* `--exit`
* `--restart`
* `--reconfigure`
* `--help`
* `--dump-stats` (when built with `HXM_DIAG`)

Typical usage:

```sh
./build/hxm
./build/hxm --reconfigure
./build/hxm --restart
./build/hxm --exit
```

Control flags expect a running `hxm` instance on the same X server.

## Configuration

`hxm.conf` is searched in the following order:

1. `$XDG_CONFIG_HOME/hxm/hxm.conf`
2. `$HOME/.config/hxm/hxm.conf`
3. `/etc/hxm/hxm.conf`
4. `data/hxm.conf`
5. `../data/hxm.conf`

`menu.conf` and `themerc` follow similar lookup rules, preferring user-level paths before system defaults.

Autostart scripts are searched in:

1. `$XDG_CONFIG_HOME/hxm/autostart`
2. `$HOME/.config/hxm/autostart`
3. `/etc/hxm/autostart`

Defaults such as `desktop_count=4`, `border_width=2`, `title_height=20`, `snap_enable=true`, and `snap_threshold_px=24` are defined in code and mirrored in `data/hxm.conf`.

Relevant environment variables:

* `XDG_CONFIG_HOME`, `HOME` for config resolution
* `HXM_LOG_UTC`, `HXM_LOG_MONO` for log timestamp modes

## Architecture at a glance

* `src/main.c` — CLI entry point and instance control.
* `src/event.c` — event loop, server lifecycle, reload and restart handling.
* `src/wm.c`, `src/wm_desktop.c`, `src/wm_input_keys.c` — core window management logic.
* `src/wm_dirty.c` — commit phase that publishes accumulated state to X11.
* `src/frame.c`, `src/render.c`, `src/menu.c` — decorations and menu rendering.
* `src/config.c` — config, keybind, rule, and theme parsing.

The design centers on separating model mutation from the final X11 flush step.

## Testing

Run the full suite:

```sh
meson test -C build --print-errorlogs
```

Run integration-heavy tests directly:

```sh
meson test -C build integration_script headless_script ewmh_xvfb conky_xvfb --print-errorlogs
```

CI includes:

* Full Xvfb-based suite
* AddressSanitizer
* UndefinedBehaviorSanitizer

Guard scripts enforce policy around skipped tests and sync-reply usage.

## Troubleshooting

* If you see `SKIP: Xvfb not found` or `SKIP: conky not found`, install the missing tools and rerun tests.
* Integration scripts may fail early with hints like `hxm binary not found; set HXM_BIN`.
* CLI control flags require an active `hxm` instance on the same X server.

## License

GNU General Public License v2.
