<div style="background-color:#1e1e1e; padding:1em; display:block; border-radius:8px; margin:0;">
  <img src="assets/hxm-clear.png" alt="hxm logo" width="300" style="display:block; margin:0;">
</div>

`hxm` is a lightweight stacking window manager for **Linux/X11** written in **C**.

It focuses on **predictable behavior, stable window management, and low overhead** while remaining configurable and easy to integrate into traditional desktop setups.

## Status

⚠️ **Experimental**

`hxm` is under active development and may change frequently.

Current version: **0.1.0**

---

# Features

* Lightweight stacking window manager
* EWMH and ICCCM compliance
* Multiple desktops
* Configurable keybindings and rules
* Theming and window decorations
* YAML-based root menu with icon support
* Edge snapping with preview
* Autostart script support

---

# Installation

## Dependencies

Runtime dependencies (pkg-config names):

```
xcb
xcb-icccm
xcb-xkb
xcb-randr
xcb-shape
xcb-damage
xcb-sync
cairo
pango
pangocairo
fontconfig
librsvg-2.0
yaml-0.1
```

Optional runtime integrations (enabled when available):

```
xcb-xinerama
xcb-keysyms
xkbcommon
```

Build tools:

```
meson
ninja
pkg-config
clang or gcc
```

Package names vary by distribution.

---

## Build

```sh
meson setup build
meson compile -C build
```

---

## Install

```sh
meson install -C build
```

This installs:

* `hxm` binary
* `hxm.desktop` session entry
* default configuration (`hxm.conf`)

---

# Running

Start `hxm` from an X session:

```sh
hxm
```

or launch manually:

```sh
./build/hxm
```

## Control commands

These commands communicate with a running `hxm` instance.

```sh
hxm --reconfigure
hxm --restart
hxm --exit
```

Show CLI help:

```sh
hxm --help
```

---

# Configuration

User configuration is searched in the following order:

```
$XDG_CONFIG_HOME/hxm/hxm.conf
$HOME/.config/hxm/hxm.conf
/etc/hxm/hxm.conf
```

Related files:

```
menu.conf    root menu configuration
themerc      theme settings
autostart    startup script
```

Autostart locations:

```
$XDG_CONFIG_HOME/hxm/autostart
$HOME/.config/hxm/autostart
/etc/hxm/autostart
```

Example defaults:

```
desktop_count=4
border_width=2
title_height=20
snap_enable=true
snap_threshold_px=24
```

---

# Testing

Run the test suite:

```sh
meson test -C build --print-errorlogs
```

Some tests require **Xvfb** and additional tools.

---

**Control commands not working**

Control flags require a running `hxm` instance on the same X server.

---

# Contributing

`hxm` is still evolving. Contributions and testing are welcome.

Before submitting changes:

```
meson test -C build
```

Ensure the test suite passes.

---

# License

GNU General Public License v2
