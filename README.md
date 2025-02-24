<div style="background-color:#1e1e1e; padding:1em; display:block; border-radius:8px; margin:0;">
  <img src="assets/hxm-clear.png" alt="hxm logo" width="300" style="display:block; margin:0;">
</div>
hxm is a lightweight, non-compositing X11 window manager focused on correctness,
low latency, and predictable behavior on modern Linux systems.

## Features

- Targeted ICCCM and EWMH compliance
- Predictable focus and stacking behavior
- Signal-based runtime diagnostics
- Integrated logging
- Designed for long-running stability

## Installation / Usage

Install hxm from your distribution if available, or build from source using the
steps in `HACKING.md`.

Run hxm:

```sh
hxm
```

Configure hxm by placing a config file at `~/.config/hxm/hxm.conf` or
`$XDG_CONFIG_HOME/hxm/hxm.conf`. To start hxm as your window manager, configure
your display manager or X startup scripts accordingly.

For contributor setup, build details, and tests, see `HACKING.md`. For system
architecture, see `DESIGN.md`.

## License

TBD
