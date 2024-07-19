# BBox Application Flow

This document summarizes how bbox starts, owns the X server, and processes events.

## High-level execution path

```
main
  -> server_init
  -> server_run (tick loop)
  -> server_cleanup
```

The binary can also act as a controller. CLI flags (`--exit`, `--restart`, `--reconfigure`, `--dump-stats`)
locate the running WM via `WM_S0` and `_NET_WM_PID`, then signal it.

## Core invariants

- The in-memory model is the source of truth; X is treated as I/O.
- No blocking X round-trips in hot paths.
- Each tick has bounded work (`MAX_EVENTS_PER_TICK`).
- Events are coalesced where possible and X requests are batched per tick.

## Startup and initialization

`server_init` in `src/event.c` sets up the WM process and its state:

- Connect to X (`xcb_connect_cached`), set the X socket non-blocking.
- Allocate keysyms for keybinding lookup.
- Detect XDamage extension availability.
- Load config and theme in priority order:
  - `$XDG_CONFIG_HOME/bbox/bbox.conf` and `themerc`
  - `$HOME/.config/bbox/bbox.conf` and `themerc`
  - `/etc/bbox/bbox.conf` and `themerc`
- Initialize workspace state (`desktop_count`, `current_desktop`).
- Become WM (`wm_become`):
  - Acquire `WM_S0` selection on the root window.
  - Set root event mask and cursor.
  - Publish EWMH atoms and supporting WM window.
  - Publish desktop/workarea properties.
- Adopt existing windows via `wm_adopt_children` (async attribute probes).
- Initialize epoll and register the X connection fd.
- Initialize per-tick arena, event buckets, maps, and layer lists.
- Initialize cookie jar for async replies.
- Allocate slotmap for clients (hot/cold storage).
- Initialize frame resources, root menu, and key grabs.

## Main event loop (tick)

`server_run` is a tight loop with signal-driven control:

- Shutdown: `SIGINT`/`SIGTERM` set `g_shutdown_pending`.
- Reload: `SIGHUP` sets `g_reload_pending`.
- Restart: `SIGUSR2` sets `g_restart_pending` and triggers `execv`.

Per tick (simplified):

```
server_wait_for_events
event_drain_cookies
event_ingest
event_process
xcb_flush
```

The loop records per-tick timing and counters for `--dump-stats`.

## Event ingestion and coalescing

`event_ingest` pulls queued and ready events and buckets them for the tick:

- `Expose` and `Damage` are coalesced by drawable with dirty-region union.
- `ConfigureRequest` is coalesced per window; the latest values win.
- `ConfigureNotify` is coalesced per window (last wins).
- `PropertyNotify` is coalesced by `(window, atom)`.
- `MotionNotify` keeps the last motion per window.
- `Enter/LeaveNotify` keep the last event per tick.
- `MapRequest`, `UnmapNotify`, `DestroyNotify`, `KeyPress`, `Button*`,
  and `ClientMessage` are queued in order.

## Event processing pipeline

`event_process` applies bucketed work in a stable order:

1. Lifecycle: `MapRequest`, `UnmapNotify`, `DestroyNotify`.
2. Key presses: keybindings and WM commands.
3. Button press/release: focus, menu, move/resize.
4. Expose: redraw frames or menu.
5. Client messages: EWMH/ICCCM actions.
6. Motion/enter/leave: interactive move/resize, cursor updates.
7. Configure requests: forward or update managed geometry.
8. Configure notifies: update server-side geometry tracking.
9. Property notifies: mark dirty fields (title, hints, struts).
10. Damage: accumulate dirty regions per client.
11. Flush model to X (`wm_flush_dirty`).

## Client lifecycle

### Management start

`client_manage_start` allocates a client slot, initializes state, and
queues 15 async property queries (attrs, geometry, class, hints, transients,
window type, protocols, names, state, desktop, struts, icon).

### Reply handling

`wm_handle_reply` consumes async replies:

- Pre-management adoption: if an existing window is mapped and not
  `override_redirect`, management begins.
- For managed clients, replies update:
  - Attributes (override redirect, visual/depth)
  - Geometry (server and desired)
  - Titles (net name or WM_NAME fallback)
  - WM_HINTS / WM_NORMAL_HINTS
  - Transient relationships
  - Window type and protocols
  - Desktop, state, struts, and icons
- When `pending_replies` drops to zero, management is finished or aborted.

### Finish manage

`client_finish_manage` completes setup:

- Apply rules and placement (center, mouse, transient positioning, or
  clamped within workarea).
- Create a frame window matching client visual/depth.
- Reparent client into the frame and set `_NET_FRAME_EXTENTS`.
- Map or unmap based on visibility in current workspace.
- Register damage tracking if supported.
- Install passive button grabs for focus and Alt move/resize.
- Insert into stacking lists and focus history.
- Focus the new window when appropriate.
- Redraw decorations and update root EWMH properties.

### Unmanage

`client_unmanage` tears down a client:

- Remove from stacking, transient lists, and focus history.
- Update focus to a suitable next candidate.
- Remove from SaveSet, reparent to root, and destroy the frame.
- Remove xid mappings and free resources.
- Mark root properties dirty for refresh.

## Focus and stacking

`wm_set_focus` updates focus state:

- Marks old and new clients dirty for frame/style updates.
- Maintains MRU focus history.
- Sets X input focus and sends `WM_TAKE_FOCUS` when supported.
- Updates `_NET_ACTIVE_WINDOW`.

Stacking uses per-layer lists (`LAYER_*`) in `src/stack.c`:

- `stack_raise`/`stack_lower`/`stack_place_above` update the list and issue
  X restack operations.
- Transient windows are raised together with their parent.

## Workspaces

Workspace state is held in `desktop_count` and `current_desktop`:

- Switching workspaces maps/unmaps frames and updates `WM_STATE`.
- Sticky windows (`desktop == -1`) remain visible across desktops.
- EWMH messages (`_NET_CURRENT_DESKTOP`, `_NET_WM_DESKTOP`,
  `_NET_NUMBER_OF_DESKTOPS`) are handled in `wm_handle_client_message`.

## Rendering pipeline

- Frames use Cairo/Pango rendering in `render_frame`.
- Expose and XDamage events drive dirty-region redraws via `frame_redraw_region`.
- The menu is a separate override-redirect window with its own render context.

## Menu flow

- Right click on root opens the root menu; middle click opens a client list.
- Menu grabs pointer/keyboard while visible and consumes button/motion events.
- Actions can spawn commands or trigger WM operations (reload/restart).

## Configuration and reload

- Config defaults are defined in `src/config.c`.
- Reload (`SIGHUP`) is applied inside the tick loop:
  - Load new config and theme.
  - Rebuild frame resources and menu.
  - Re-grab keys.
  - Mark all frames dirty for repaint.
  - Re-publish desktop properties.

## Signals and control

- `SIGHUP`: reload configuration (applied in tick).
- `SIGUSR1`: dump counters to stdout.
- `SIGUSR2`: restart in-place (cleanup then `execv`).
- `SIGINT`/`SIGTERM`: request shutdown.

## Key data structures

- `slotmap_t`: handle-based storage for clients with hot/cold split.
- `event_buckets_t`: per-tick coalescing structures for event processing.
- `cookie_jar_t`: async reply tracking to avoid blocking X calls.
- `arena_t` and `small_vec_t`: fast per-tick allocations and event queues.
