## 0) Test harness shape that makes all of this easy

### Option A: link-time wrappers (fastest to adopt)

In your test executable link args:

* `-Wl,--wrap=xcb_connect_cached`
* `-Wl,--wrap=epoll_create1`
* `-Wl,--wrap=epoll_ctl`
* `-Wl,--wrap=epoll_wait`
* `-Wl,--wrap=signalfd`
* `-Wl,--wrap=timerfd_create`
* `-Wl,--wrap=timerfd_settime`
* `-Wl,--wrap=read`
* `-Wl,--wrap=fork`
* `-Wl,--wrap=setsid`
* `-Wl,--wrap=execl`
* `-Wl,--wrap=waitpid`
* `-Wl,--wrap=exit`
* `-Wl,--wrap=abort`
* `-Wl,--wrap=access`
* `-Wl,--wrap=xcb_poll_for_queued_event`
* `-Wl,--wrap=xcb_poll_for_event`
* `-Wl,--wrap=xcb_connection_has_error`
* `-Wl,--wrap=xcb_flush`


---

## 1) `server_init` tests (all the “exit(1)” and extension branches)

### 1.1 connect failure exits

**Covers**: `if (!s->conn) { ... exit(1); }` (currently ✗)

* Arrange: `__wrap_xcb_connect_cached` returns `NULL`
* Arrange: `__wrap_exit` records status instead of terminating (use `setjmp/longjmp`)
* Assert: exit called with `1`

### 1.2 fcntl nonblock set fails exits

**Covers**: `fcntl(F_SETFL)` failure branch (✗)

* Arrange: `__wrap_fcntl(F_GETFL)` returns `0` (blocking)
* Arrange: `__wrap_fcntl(F_SETFL, ...)` returns `-1` and sets `errno`
* Assert: `exit(1)` called

### 1.3 keysyms alloc failure exits

**Covers**: `if (!s->keysyms) exit(1)` (✗)

* Arrange: `__wrap_xcb_key_symbols_alloc` returns `NULL`
* Assert: exit called

### 1.4 XDamage present but version query reply NULL disables damage

**Covers**: branch at lines ~103–106 (✗)

* Arrange: `xcb_get_extension_data(&xcb_damage_id)` returns `{present=1, first_event=...}`
* Arrange: `xcb_damage_query_version_reply` returns `NULL`
* Assert:

  * `s->damage_supported == false`
  * `s->damage_event_base` may remain but damage_supported is what matters

### 1.5 RandR present but version query reply NULL disables randr

**Covers**: branch at lines ~118–121 (✗)

* Arrange: `xcb_get_extension_data(&xcb_randr_id)` present=1
* Arrange: `xcb_randr_query_version_reply` returns `NULL`
* Assert: `s->randr_supported == false` and `s->randr_event_base == 0`

### 1.6 restoring `_NET_ACTIVE_WINDOW` sets initial_focus

**Covers**: lines ~153–156 (currently ✗)

* Arrange: `xcb_get_property_reply` for `_NET_ACTIVE_WINDOW` returns a fake reply:

  * type WINDOW, format 32, length >= 4, value = some window id
* Assert: `s->initial_focus == that window id`

### 1.7 `sigprocmask` fails exits

**Covers**: branch lines ~188–191 (✗)

* Arrange: `__wrap_sigprocmask` returns `-1`
* Assert: exit called

### 1.8 `signalfd` fails exits

**Covers**: branch ~194–197 (✗)

* Arrange: `__wrap_signalfd` returns `-1`
* Assert: exit called

### 1.9 `timerfd_create` fails exits

**Covers**: branch ~202–205 (✗)

* Arrange: `__wrap_timerfd_create` returns `-1`
* Assert: exit called

### 1.10 client slotmap init fails aborts

**Covers**: branch ~248–251 (✗)

* Arrange: `slotmap_init` returns false (wrap it or inject)
* Assert: `abort()` called (wrap abort)

---

## 2) `server_cleanup` tests (prefetch + pending_unmanaged_states cleanup)

### 2.1 prefetched_event freed and nulled

**Covers**: lines ~273–276 (✗)

* Arrange: `s->prefetched_event = malloc(…)`
* Call `server_cleanup(&s)`
* Assert: pointer becomes `NULL` (you can also wrap `free` and verify it was called)

### 2.2 pending_unmanaged_states destroys vectors and frees them

**Covers**: the loop lines ~329–337 (✗)

* Arrange: create `s->pending_unmanaged_states` with at least one occupied entry:

  * key != 0
  * value = `small_vec_t*` allocated with `malloc`
  * initialize vec with `small_vec_init` and push something
* Call cleanup
* Assert:

  * `small_vec_destroy` called for that vec (wrap it or detect via poison)
  * `free(v)` called
  * map destroyed after

This is a classic leak path and worth having even if you later refactor the map.

---

## 3) `make_epoll_or_die` and `epoll_add_fd_or_die`

### 3.1 epoll_create1 fails exits

**Covers**: branch ~354–357 (✗)

* Arrange: `__wrap_epoll_create1` returns `-1`
* Assert: exit called

### 3.2 epoll_ctl add fails exits

**Covers**: branch ~366–369 (✗)

* Arrange: `__wrap_epoll_ctl` returns `-1`
* Assert: exit called

---

## 4) `run_autostart` (currently completely uncovered where it matters)

You want this unit-tested because it’s fork/exec logic with multiple search paths.

### 4.1 chooses XDG_CONFIG_HOME script when executable

**Covers**: priority path selection

* Arrange env:

  * `XDG_CONFIG_HOME=/tmp/xdg`
  * `HOME=/tmp/home`
* Arrange:

  * `__wrap_access("/tmp/xdg/hxm/autostart", X_OK)` returns `0`
* Arrange fork path but **do not actually fork**:

  * `__wrap_fork` returns `1234` (parent)
* Assert: it tried to fork and did not fall back to `HOME` or `/etc`

### 4.2 falls back to HOME when XDG not executable

* access(xdg) => -1, access(home) => 0, fork => parent
* Assert: exec_path becomes HOME one (you can detect via `__wrap_fork` capturing last computed path by wrapping `execl` and forcing child path in a separate test)

### 4.3 falls back to /etc when neither XDG nor HOME exists

* access(xdg) => -1, access(home) => -1, access("/etc/hxm/autostart") => 0

### 4.4 child: setsid fails still execl called

**Covers**: `setsid() < 0` warn path and continues

* Arrange: `__wrap_fork` returns `0` (child)
* Arrange: `__wrap_setsid` returns `-1`
* Arrange: `__wrap_execl` records args and then calls `_exit(…)` via wrap
* Assert: execl invoked with `exec_path` and argv0 = exec_path

### 4.5 child: execl fails triggers exit(1)

* fork => 0, execl => -1, expect exit(1)

### 4.6 fork fails logs error (no exit)

* fork => -1, assert it does not call exit/abort

---

## 5) `event_ingest_one` (coalescing branches you’re missing)

### 5.1 RandR screen change notify coalesces

**Covers**: entire RandR branch around ~552–565 (currently ✗)

* Arrange: `s->randr_supported=true`, `s->randr_event_base=BASE`
* Build an event with response_type = `BASE + XCB_RANDR_SCREEN_CHANGE_NOTIFY`
* First call:

  * Assert: `s->buckets.randr_dirty==true`, width/height set, `coalesced` not incremented
* Second call with different width/height:

  * Assert: `coalesced++` happened and latest width/height stored

### 5.2 XCB_COLORMAP_NOTIFY dispatches handler

**Covers**: case `XCB_COLORMAP_NOTIFY` (currently ✗)

* Wrap `wm_handle_colormap_notify` to increment a counter
* Feed a colormap notify event
* Assert handler called exactly once

### 5.3 configure_request coalesces existing entry

**Covers**: `if (existing)` block ~667–679 (✓)

* Feed first configure_request with mask X|Y|WIDTH
* Feed second configure_request for same window with mask HEIGHT only
* Assert:

  * resulting `pending_config_t` has mask OR’d
  * x/y/width preserved, height updated
  * coalesced incremented

### 5.4 enter/leave notify coalesces when already valid

**Covers**: enter_valid and leave_valid branches (currently ✗)

* Feed ENTER twice

  * Assert coalesced incremented on second
* Feed LEAVE twice

  * same

### 5.5 log_unhandled_summary actually logs and resets counters

**Covers**: all the ✗ inside `log_unhandled_summary` (~793–806)

* Arrange: set `counters.events_unhandled[123]=5`
* Arrange: force `rl_allow` to return true (wrap or set rl state so it allows)
* Wrap `LOG_INFO` macro destination or provide a logger hook
* Call `log_unhandled_summary()`
* Assert:

  * log contains “123=5”
  * `counters.events_unhandled[123]==0`

---

## 6) `event_process` (several loops are not executed in tests)

Your coverage shows several loops never run in your unit tests (key presses, buttons, menu expose path, motion-notify path, configure_request unknown-window path, RandR process path).

### 6.1 key press dispatch

**Covers**: loop lines ~840–843 (✗)

* Put one fake `xcb_key_press_event_t` into `s->buckets.key_presses`
* Wrap `wm_handle_key_press` to count calls
* Call `event_process`
* Assert called

### 6.2 button press and release dispatch

**Covers**: loop lines ~846–853 (✗)

* Add one button press event and one button release event to `button_events`
* Wrap `wm_handle_button_press` and `_release`
* Assert both invoked

### 6.3 menu expose region goes to menu handler

**Covers**: branch `if (win == s->menu.window)` (✗)

* Set `s->menu.window = 0xabc`
* Insert expose region entry for key=0xabc with valid region
* Wrap `menu_handle_expose_region`
* Assert called and frame redraw not called

### 6.4 motion notify dispatch

**Covers**: loop around ~893–900 (✗)

* Insert a motion notify into `buckets.motion_notifies`
* Wrap `wm_handle_motion_notify`
* Assert called

### 6.5 configure_request for unknown window calls xcb_configure_window

**Covers**: branch lines ~911–926 (✗)

* Insert a `pending_config_t` in `buckets.configure_requests`
* Wrap `server_get_client_by_window` to return `HANDLE_INVALID`
* Wrap `xcb_configure_window` to capture mask/values
* Assert called with correct mask/values ordering

### 6.6 RandR dirty processing path

**Covers**: block lines ~989–1009 (✗)

* Set `s->buckets.randr_dirty=true` with width/height set
* Wrap:

  * `wm_update_monitors`
  * `xcb_change_property`
  * `wm_compute_workarea`
  * `wm_publish_workarea`
* Assert each called
* If `fullscreen_use_workarea=false`, set up slotmap with one fullscreen client and assert it gets `DIRTY_GEOM`

---

## 7) `server_schedule_timer` minimal-positive nsec branch

### 7.1 ms > 0 but computed it_value is zero sets nsec=1

**Covers**: branch ~1022–1024 (✗)

This branch is rare, but you can force it by making `ms` tiny and ensuring integer math produces 0/0.

* Call `server_schedule_timer(s, 1)` normally yields nsec=1,000,000, so not zero
* To hit the branch, you need `ms > 0` but both `tv_sec==0` and `tv_nsec==0`

  * That can only happen if `ms` is >0 but `% 1000 == 0` and `/1000 == 0`, impossible with normal arithmetic
    So this looks like a defensive check that will never trigger for sane `ms` values.

Two options:

* If you keep it: treat as “unreachable” and exclude from branch coverage
* If you want it testable: change conversion to something that can round to zero (eg if you accept sub-ms inputs in future)

Still, you *can* unit-test the rest:

* Wrap `timerfd_settime` and assert it’s called with correct seconds/nanos for `ms=1500`, `ms=10`, `ms=0` (disarm)

---

## 8) `apply_reload` and signal handling

### 8.1 SIGHUP sets reload pending

**Covers**: switch cases in `event_handle_signals` (SIGHUP ✗)

* Arrange: `read(signal_fd)` returns a full `signalfd_siginfo` with `.ssi_signo = SIGHUP`
* Call `event_handle_signals(s)`
* Assert: `g_reload_pending == 1`

### 8.2 SIGUSR1 calls counters_dump

* Wrap `counters_dump` and assert called

### 8.3 SIGUSR2 sets restart pending

* Assert `g_restart_pending == 1`

### 8.4 SIGCHLD reaps zombies

* Wrap `waitpid` to return `>0` twice then `0`
* Assert loop executed

### 8.5 apply_reload: desktop_count changes, clamps current desktop, updates client desktops

**Covers**: most of `apply_reload` (currently fully ✗)

This is a big one but valuable:

* Arrange initial `s->config.desktop_count=4`, `s->desktop_count=4`, `s->current_desktop=3`
* Make “next config” load produce desktop_count=2
* Put a non-sticky client with `hot->desktop=3` (out of range)
* Wrap `xcb_change_property` to assert it writes `_NET_WM_DESKTOP` with clamped value
* Assert:

  * `s->desktop_count==2`
  * `s->current_desktop` clamped if needed
  * `workarea_dirty==true`
  * `frame_cleanup_resources` then `frame_init_resources` called
  * `menu_destroy` then `menu_init` called
  * `wm_setup_keys` called
  * all clients got `DIRTY_FRAME_STYLE|DIRTY_GEOM`

---

## 9) `server_wait_for_events` branches (error paths + epoll bits)

### 9.1 returns true if epoll_fd <= 0

**Covers**: early return

### 9.2 returns true if prefetched_event exists

**Covers**: early return

### 9.3 queued event sets prefetched_event and returns true

**Covers**: `xcb_poll_for_queued_event` branch

### 9.4 connection has error sets shutdown and returns false

**Covers**: lines ~1182–1185 (✗)

* Arrange: `xcb_connection_has_error` returns nonzero
* Assert: `g_shutdown_pending==1`, function returns false

### 9.5 epoll_wait returns EPOLLERR for xcb fd triggers shutdown false

**Covers**: lines ~1200–1204 (✗)

* Arrange: `epoll_wait` returns one event:

  * `.events = EPOLLERR`
  * `.data.fd = s->xcb_fd`
* Assert: shutdown pending and returns false

### 9.6 epoll_wait timeout n==0 returns false

**Covers**: `if (n == 0) return false` (✗)

### 9.7 epoll_wait EINTR loops unless shutdown/restart/reload set

**Covers**: EINTR branch (✗)

* Arrange: first epoll_wait => -1, errno=EINTR, no pending flags
* Arrange: second epoll_wait => 0
* Assert: returns false and did not set shutdown

### 9.8 only signal event wakes, x_ready=false returns false

**Covers**: the “signals only” return false path around ~1222–1226

---

## 10) `server_run` (restart path and reload path)

### 10.1 restart requested: cleanup then execv attempted

**Covers**: restart block (currently ✗)

* Set `g_restart_pending=1`
* Wrap `readlink` to return `/proc/self/exe` path
* Wrap `server_cleanup` to record call
* Wrap `execv` to record args and fail (return -1)
* Assert:

  * `s->restarting==true`
  * cleanup called
  * execv called with argv0=path

### 10.2 reload requested calls apply_reload then skips wait logic

**Covers**: reload branch (✗)

* Set `g_reload_pending=1`
* Wrap `apply_reload` to record call
* Run one iteration by making `g_shutdown_pending=1` after
* Assert apply_reload called and `reload_applied==true` behavior implies it doesn’t call `server_wait_for_events` that tick
