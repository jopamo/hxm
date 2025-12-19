# bbox Theme Specification (`themerc`)

This document defines the supported theme keys and behaviors for `bbox`, modeled after the Openbox `themerc` format.

## Overview

`bbox` uses a subset of the Openbox theme format to provide a familiar and powerful way to customize the window manager's appearance. The theme is defined in a file named `themerc` located within the theme directory.

## Supported Keys

### Border
- `border.width`: The width of the window border in pixels.
- `padding.width`: Internal padding for various elements (not yet fully implemented).

### Window Active (Focused)
- `window.active.border.color`: Color of the border for the active window.
- `window.active.title.bg`: Background style for the active title bar.
- `window.active.title.bg.color`: Primary color for the active title bar background.
- `window.active.title.bg.colorTo`: Secondary color for the active title bar background (for gradients).
- `window.active.label.text.color`: Color of the text in the active title bar.
- `window.active.handle.bg`: Background style for the active handle (bottom bar).
- `window.active.handle.bg.color`: Primary color for the active handle background.
- `window.active.grip.bg`: Background style for the active grips (corners of the handle).
- `window.active.grip.bg.color`: Primary color for the active grip background.

### Window Inactive (Unfocused)
- `window.inactive.border.color`: Color of the border for inactive windows.
- `window.inactive.title.bg`: Background style for inactive title bars.
- `window.inactive.title.bg.color`: Primary color for inactive title bar background.
- `window.inactive.title.bg.colorTo`: Secondary color for inactive title bar background (for gradients).
- `window.inactive.label.text.color`: Color of the text in inactive title bars.
- `window.inactive.handle.bg`: Background style for inactive handles.
- `window.inactive.handle.bg.color`: Primary color for inactive handle background.
- `window.inactive.grip.bg`: Background style for inactive grips.
- `window.inactive.grip.bg.color`: Primary color for inactive grip background.

### Menu
- `menu.items.bg`: Background style for menu items.
- `menu.items.bg.color`: Primary color for menu item background.
- `menu.items.text.color`: Color of the text for menu items.
- `menu.items.active.bg`: Background style for the selected menu item.
- `menu.items.active.bg.color`: Primary color for the selected menu item background.
- `menu.items.active.text.color`: Color of the text for the selected menu item.

## Background Styles

The `*.bg` keys support the following style combinations:

- `Solid`: A single solid color.
- `Gradient`: A gradient between two colors.
- `Vertical`: Vertical gradient (default for gradients).
- `Horizontal`: Horizontal gradient.
- `Diagonal`: Diagonal gradient.
- `CrossDiagonal`: Cross-diagonal gradient.
- `Raised`: Adds a 3D raised effect (bevel).
- `Sunken`: Adds a 3D sunken effect.
- `Flat`: No 3D effect (default).
- `Bevel1`: Small bevel.
- `Bevel2`: Large bevel.

Example: `window.active.title.bg: raised gradient vertical`

## Unsupported Keys (Explicitly Ignored)

The following Openbox keys are currently NOT supported and will be ignored by the parser:

- `window.active.button.*` (Planned for future)
- `window.inactive.button.*` (Planned for future)
- `menu.title.*`
- `menu.border.*`
- `*.interlaced`
- `*.pixmap`

## Compatibility Notes vs Openbox

- `bbox` uses Cairo for all rendering, ensuring high-quality scaling and anti-aliasing.
- Some Openbox themes use complex pixmaps; `bbox` currently focuses on vector-based (colors/gradients) themes.
- `bbox` aims for visual similarity but may have slight differences in bevel rendering or gradient interpolation compared to original Openbox.
