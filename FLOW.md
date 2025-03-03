# Hxm Application Flow

This document describes how hxm starts, becomes the window manager, and processes
X11 events using a deterministic, tick-based execution model.

---

## High-level execution path

```

main
-> server_init
-> server_run (tick loop)
-> server_cleanup

```

The binary can also act as a controller. CLI flags (`--exit`, `--restart`,
`--reconfigure`, `--dump-stats`) locate the running WM via the `WM_S0` selection
and `_NET_WM_PID`, then signal it.

---

## Core invariants

These invariants are architectural and must not be violated:

- The in-memory model is the source of truth; X is treated strictly as I/O.
- No blocking X round-trips occur in hot paths.
- Each tick performs bounded work (`MAX_EVENTS_PER_TICK`).
- Events are coalesced before processing.
- X requests are emitted only during the commit phase.
- No partial or intermediate state is flushed to X.

---

## Startup and initialization

`server_init` (in `src/event.c`) initializes the WM process and global state:

- Connect to X using `xcb_connect_cached` and set the X socket non-blocking.
- Allocate keysyms for keybinding lookup.
- Detect XDamage extension availability.
- Load configuration and theme in priority order:
  - `$XDG_CONFIG_HOME/hxm/hxm.conf` and `themerc`
  - `$HOME/.config/hxm/hxm.conf` and `themerc`
  - `/etc/hxm/hxm.conf` and `themerc`
- Initialize workspace state (`desktop_count`, `current_desktop`).
- Become the window manager (`wm_become`):
  - Acquire the `WM_S0` selection on the root window.
  - Set the root event mask and cursor.
  - Publish EWMH atoms and the supporting WM window.
  - Publish initial desktop and workarea properties.
- Adopt existing windows via `wm_adopt_children` using asynchronous attribute
  probes.
- Initialize epoll and register the X connection file descriptor.
- Initialize per-tick arena, event buckets, maps, and layer lists.
- Initialize the cookie jar for asynchronous replies.
- Allocate the client slotmap (hot/cold storage).
- Initialize frame resources, root menu, and key grabs.

No user-visible X state is mutated until the first commit phase.

---

## Main event loop (tick)

`server_run` executes a tight, signal-aware loop.

Signal handling:
- `SIGINT` / `SIGTERM` request shutdown.
- `SIGHUP` requests configuration reload.
- `SIGUSR2` requests in-place restart via `execv`.

Per tick (simplified):

```

server_wait_for_events
event_drain_cookies
event_ingest
event_process
xcb_flush

```

Per-tick timing and counters are recorded and exposed via `--dump-stats`.

---

## Event ingestion and coalescing

`event_ingest` pulls queued and ready X events and buckets them for the current
tick.

Coalescing rules:
- `Expose` and `Damage` are coalesced per drawable with dirty-region union.
- `ConfigureRequest` is coalesced per window; last request wins.
- `ConfigureNotify` is coalesced per window; last wins.
- `PropertyNotify` is coalesced per `(window, atom)`.
- `MotionNotify` keeps only the final motion per window.
- `EnterNotify` / `LeaveNotify` keep only the final event per tick.
- `MapRequest`, `UnmapNotify`, `DestroyNotify`, `KeyPress`, `Button*`, and
  `ClientMessage` are queued in arrival order.

Ingestion performs no state mutation beyond event bucketing.

---

## Event processing pipeline

`event_process` applies bucketed work in a stable, deterministic order:

1. Client lifecycle:
   - `MapRequest`
   - `UnmapNotify`
   - `DestroyNotify`
2. Key presses:
   - Keybindings
   - WM commands
3. Button press/release:
   - Focus changes
   - Menu activation
   - Move/resize initiation
4. Expose:
   - Frame redraw
   - Menu redraw
5. Client messages:
   - EWMH and ICCCM actions
6. Motion / enter / leave:
   - Interactive move/resize
   - Cursor updates
7. Configure requests:
   - Forwarded or translated into managed geometry updates
8. Configure notifies:
   - Server-side geometry tracking
9. Property notifies:
   - Mark client fields dirty (title, hints, struts)
10. Damage:
    - Accumulate dirty regions per client
11. Commit:
    - Flush model changes to X (`wm_flush_dirty`)

No X requests are emitted outside step 11.

---

## Client lifecycle

### Management start

`client_manage_start` allocates a client slot, initializes state, and queues
asynchronous property queries:

- Attributes and geometry
- WM_CLASS and titles
- WM_HINTS and WM_NORMAL_HINTS
- Transient relationships
- Window type and supported protocols
- Desktop, state, struts, and icons

Replies are tracked via the cookie jar.

---

### Reply handling

`wm_handle_reply` consumes asynchronous replies:

- Pre-management adoption of existing windows that are mapped and not
  `override_redirect`.
- For managed clients, replies update:
  - Attributes (override redirect, visual, depth)
  - Geometry (server-reported and desired)
  - Titles (`_NET_WM_NAME` with `WM_NAME` fallback)
  - Hints and size constraints
  - Transient relationships
  - Window type, protocols, desktop, state, struts, icons

When `pending_replies` reaches zero, management either completes or aborts.

---

### Finish manage

`client_finish_manage` completes client setup:

- Apply placement rules (center, mouse, transient, or workarea-clamped).
- Create a frame window matching client visual and depth.
- Reparent the client into the frame and publish `_NET_FRAME_EXTENTS`.
- Map or unmap based on workspace visibility.
- Register damage tracking if supported.
- Install passive button grabs for focus and Alt-based move/resize.
- Insert into stacking lists and focus history.
- Focus the new client when appropriate.
- Mark decorations and root EWMH properties dirty.

---

### Unmanage

`client_unmanage` tears down a client:

- Remove from stacking, transient lists, and focus history.
- Select and focus the next suitable client.
- Remove from the SaveSet.
- Reparent the client to the root and destroy the frame.
- Remove XID mappings and free resources.
- Mark root properties dirty for refresh.

---

## Focus and stacking

`wm_set_focus`:
- Updates focus state in the model.
- Marks old and new clients dirty for frame updates.
- Maintains MRU focus history.
- Sets X input focus and sends `WM_TAKE_FOCUS` if supported.
- Updates `_NET_ACTIVE_WINDOW`.

Stacking is managed via per-layer lists (`LAYER_*`) in `src/stack.c`:
- Raise/lower operations update the model and issue restack requests at commit.
- Transient windows are raised together with their parent.

---

## Workspaces

Workspace state is tracked via `desktop_count` and `current_desktop`:

- Switching workspaces maps or unmaps frames and updates `WM_STATE`.
- Sticky clients (`desktop == -1`) remain visible across workspaces.
- EWMH messages (`_NET_CURRENT_DESKTOP`, `_NET_WM_DESKTOP`,
  `_NET_NUMBER_OF_DESKTOPS`) are handled in `wm_handle_client_message`.

---

## Rendering pipeline

- Frames are rendered using Cairo and Pango (`render_frame`).
- Expose and XDamage events drive dirty-region redraws via
  `frame_redraw_region`.
- The menu is a separate override-redirect window with its own render context.

---

## Menu flow

- Right click on the root opens the root menu.
- Middle click opens a client list.
- The menu grabs pointer and keyboard while visible.
- Menu actions spawn commands or trigger WM operations.

---

## Configuration and reload

- Configuration defaults are defined in `src/config.c`.
- Reload (`SIGHUP`) is applied inside the tick loop:
  - Load new configuration and theme.
  - Rebuild frame resources and menu.
  - Re-grab keys.
  - Mark all frames dirty for repaint.
  - Re-publish desktop and workarea properties.

---

## Signals and control

- `SIGHUP`: reload configuration (applied at tick boundary)
- `SIGUSR1`: dump counters to stdout
- `SIGUSR2`: restart in-place (`execv`)
- `SIGINT` / `SIGTERM`: request shutdown

---

## Key data structures

- `slotmap_t`: handle-based client storage with hot/cold split.
- `event_buckets_t`: per-tick event coalescing structures.
- `cookie_jar_t`: asynchronous reply tracking to avoid blocking X calls.
- `arena_t` and `small_vec_t`: fast per-tick allocations and queues.
