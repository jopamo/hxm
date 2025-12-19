#include "event.h"

#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb_keysyms.h>

#include "bbox.h"
#include "frame.h"
#include "wm.h"
#include "xcb_utils.h"

/*
 * event.c
 *
 * Responsibilities:
 *  - server_init / server_cleanup: lifetime of the server process and core resources
 *  - event_ingest: poll X, bucket events, and coalesce where appropriate (bounded per tick)
 *  - event_process: apply bucketed work in a stable order (lifecycle -> input -> configure -> property)
 *  - event_drain_cookies: drain async replies (never block in hot loop)
 *  - server_run: tick loop: ingest -> process -> drain -> flush
 *
 * Invariants:
 *  - No blocking X round-trips in hot paths
 *  - Bounded work per tick (MAX_EVENTS_PER_TICK)
 *  - Use tick_arena for per-tick allocations and copies
 *  - Batch X requests and flush once per tick
 */

static int make_epoll_or_die(void);
static void epoll_add_fd_or_die(int epfd, int fd);
static void load_config_from_home(server_t* s);
static void apply_reload(server_t* s);
static void buckets_reset(event_buckets_t* b);

void server_init(server_t* s) {
    memset(s, 0, sizeof(*s));

    s->conn = xcb_connect_cached();
    if (!s->conn) {
        LOG_ERROR("Failed to connect to X server");
        exit(1);
    }

    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
    s->root = screen->root;
    s->root_visual = screen->root_visual;
    s->root_visual_type = xcb_get_visualtype(s->conn, s->root_visual);
    s->root_depth = screen->root_depth;
    s->xcb_fd = xcb_get_file_descriptor(s->conn);

    // Keysyms are used by keybinding setup and keypress handling
    s->keysyms = xcb_key_symbols_alloc(s->conn);
    if (!s->keysyms) {
        LOG_ERROR("xcb_key_symbols_alloc failed");
        exit(1);
    }

    // Initialize configuration (defaults then optional load)
    config_init_defaults(&s->config);
    load_config_from_home(s);

    // Initialize workspace state from config
    s->desktop_count = s->config.desktop_count ? s->config.desktop_count : 1;
    s->current_desktop = 0;

    // Become WM (WM_S0 selection + supporting WM check + _NET_SUPPORTED baseline)
    wm_become(s);

    // Adopt existing windows (must happen after we are the WM)s
    wm_adopt_children(s);

    // Create epoll instance and register X connection fd
    s->epoll_fd = make_epoll_or_die();
    epoll_add_fd_or_die(s->epoll_fd, s->xcb_fd);

    // Initialize per-tick arena (64KB blocks)
    arena_init(&s->tick_arena, 64 * 1024);

    // Initialize event buckets
    small_vec_init(&s->buckets.map_requests);
    small_vec_init(&s->buckets.unmap_notifies);
    small_vec_init(&s->buckets.destroy_notifies);
    small_vec_init(&s->buckets.key_presses);
    small_vec_init(&s->buckets.expose_events);
    small_vec_init(&s->buckets.button_events);
    small_vec_init(&s->buckets.client_messages);

    hash_map_init(&s->buckets.configure_requests);
    hash_map_init(&s->buckets.configure_notifies);
    hash_map_init(&s->buckets.destroyed_windows);
    hash_map_init(&s->buckets.property_notifies);

    s->buckets.motion_notify.valid = false;
    s->buckets.pointer_notify.enter_valid = false;
    s->buckets.pointer_notify.leave_valid = false;
    s->buckets.ingested = 0;
    s->buckets.coalesced = 0;

    // Initialize global maps
    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);

    // Layer lists and focus ring
    for (int i = 0; i < LAYER_COUNT; i++) {
        list_init(&s->layers[i]);
    }
    list_init(&s->focus_history);
    s->focused_client = HANDLE_INVALID;

    // Cookie jar for async request/reply handling
    cookie_jar_init(&s->cookie_jar);

    // Client storage (handles + hot/cold split)
    if (!slotmap_init(&s->clients, 8192, sizeof(client_hot_t), sizeof(client_cold_t))) {
        LOG_ERROR("client slotmap init failed");
        abort();
    }

    // Setup decoration resources (colors/fonts/gcs/etc)
    frame_init_resources(s);

    // Root menu
    menu_init(s);

    // Setup keys
    wm_setup_keys(s);

    LOG_INFO("Server initialized");
}

void server_cleanup(server_t* s) {
    // Unmanage all clients (reparent back to root)
    if (s->clients.hdr) {
        for (uint32_t i = 1; i < s->clients.cap; i++) {
            if (s->clients.hdr[i].live) {
                handle_t h = handle_make(i, s->clients.hdr[i].gen);
                client_unmanage(s, h);
            }
        }
    }

    if (s->keysyms) xcb_key_symbols_free(s->keysyms);

    frame_cleanup_resources(s);
    menu_destroy(s);
    config_destroy(&s->config);

    if (s->epoll_fd > 0) close(s->epoll_fd);
    if (s->conn) xcb_disconnect(s->conn);

    arena_destroy(&s->tick_arena);

    small_vec_destroy(&s->buckets.map_requests);
    small_vec_destroy(&s->buckets.unmap_notifies);
    small_vec_destroy(&s->buckets.destroy_notifies);
    small_vec_destroy(&s->buckets.key_presses);
    small_vec_destroy(&s->buckets.expose_events);
    small_vec_destroy(&s->buckets.button_events);
    small_vec_destroy(&s->buckets.client_messages);

    hash_map_destroy(&s->buckets.configure_requests);
    hash_map_destroy(&s->buckets.configure_notifies);
    hash_map_destroy(&s->buckets.destroyed_windows);
    hash_map_destroy(&s->buckets.property_notifies);

    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);

    slotmap_destroy(&s->clients);

    // Global library cleanup for ASan
    pango_cairo_font_map_set_default(NULL);
    FcFini();
}

static int make_epoll_or_die(void) {
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        exit(1);
    }
    return epfd;
}

static void epoll_add_fd_or_die(int epfd, int fd) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl add fd=%d failed: %s", fd, strerror(errno));
        exit(1);
    }
}

static void load_config_from_home(server_t* s) {
    char path[1024];
    bool config_loaded = false;
    const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");

    if (xdg_config_home) {
        snprintf(path, sizeof(path), "%s/bbox/bbox.conf", xdg_config_home);
        config_loaded = config_load(&s->config, path);
    }

    if (!config_loaded && home) {
        snprintf(path, sizeof(path), "%s/.config/bbox/bbox.conf", home);
        config_loaded = config_load(&s->config, path);
    }

    if (!config_loaded) {
        config_load(&s->config, "/etc/bbox/bbox.conf");
    }

    // Now try to load the theme
    bool theme_loaded = false;
    if (xdg_config_home) {
        snprintf(path, sizeof(path), "%s/bbox/themerc", xdg_config_home);
        theme_loaded = theme_load(&s->config.theme, path);
    }

    if (!theme_loaded && home) {
        snprintf(path, sizeof(path), "%s/.config/bbox/themerc", home);
        theme_loaded = theme_load(&s->config.theme, path);
    }

    if (!theme_loaded) {
        theme_load(&s->config.theme, "/etc/bbox/themerc");
    }
}

static void buckets_reset(event_buckets_t* b) {
    small_vec_clear(&b->map_requests);
    small_vec_clear(&b->unmap_notifies);
    small_vec_clear(&b->destroy_notifies);
    small_vec_clear(&b->key_presses);
    small_vec_clear(&b->expose_events);
    small_vec_clear(&b->button_events);
    small_vec_clear(&b->client_messages);

    // For hash maps, keep the API simple: destroy+init per tick
    // If this shows up in profiles, replace with hash_map_clear without realloc
    hash_map_destroy(&b->configure_requests);
    hash_map_init(&b->configure_requests);

    hash_map_destroy(&b->configure_notifies);
    hash_map_init(&b->configure_notifies);

    hash_map_destroy(&b->destroyed_windows);
    hash_map_init(&b->destroyed_windows);

    hash_map_destroy(&b->property_notifies);
    hash_map_init(&b->property_notifies);

    b->motion_notify.valid = false;
    b->pointer_notify.enter_valid = false;
    b->pointer_notify.leave_valid = false;

    b->ingested = 0;
    b->coalesced = 0;
}

void event_ingest(server_t* s) {
    buckets_reset(&s->buckets);
    arena_reset(&s->tick_arena);

    uint64_t count = 0;
    while (count < MAX_EVENTS_PER_TICK) {
        xcb_generic_event_t* ev = xcb_poll_for_event(s->conn);
        if (!ev) break;

        count++;
        uint8_t type = ev->response_type & ~0x80;
        counters.events_seen[type]++;

        switch (type) {
            case XCB_EXPOSE: {
                xcb_expose_event_t* e = (xcb_expose_event_t*)ev;
                // Only repaint on the final expose in a series
                if (e->count == 0) {
                    void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
                    memcpy(copy, e, sizeof(*e));
                    small_vec_push(&s->buckets.expose_events, copy);
                } else {
                    counters.coalesced_drops[type]++;
                }
                break;
            }

            case XCB_BUTTON_PRESS:
            case XCB_BUTTON_RELEASE: {
                // Keep ordering for press/release (don’t coalesce)
                // Pointer motion is coalesced separately
                size_t sz =
                    (type == XCB_BUTTON_PRESS) ? sizeof(xcb_button_press_event_t) : sizeof(xcb_button_release_event_t);
                void* copy = arena_alloc(&s->tick_arena, sz);
                memcpy(copy, ev, sz);
                small_vec_push(&s->buckets.button_events, copy);
                break;
            }

            case XCB_CLIENT_MESSAGE: {
                xcb_client_message_event_t* e = (xcb_client_message_event_t*)ev;
                void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
                memcpy(copy, e, sizeof(*e));
                small_vec_push(&s->buckets.client_messages, copy);
                break;
            }

            case XCB_KEY_PRESS: {
                xcb_key_press_event_t* e = (xcb_key_press_event_t*)ev;
                void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
                memcpy(copy, e, sizeof(*e));
                small_vec_push(&s->buckets.key_presses, copy);
                break;
            }

            case XCB_MAP_REQUEST: {
                xcb_map_request_event_t* e = (xcb_map_request_event_t*)ev;
                void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
                memcpy(copy, e, sizeof(*e));
                small_vec_push(&s->buckets.map_requests, copy);
                break;
            }

            case XCB_UNMAP_NOTIFY: {
                xcb_unmap_notify_event_t* e = (xcb_unmap_notify_event_t*)ev;
                void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
                memcpy(copy, e, sizeof(*e));
                small_vec_push(&s->buckets.unmap_notifies, copy);
                break;
            }

            case XCB_DESTROY_NOTIFY: {
                xcb_destroy_notify_event_t* e = (xcb_destroy_notify_event_t*)ev;

                // Mark destroyed in this tick
                hash_map_insert(&s->buckets.destroyed_windows, e->window, (void*)1);

                // Drop earlier ConfigureRequests for this window in this tick
                hash_map_remove(&s->buckets.configure_requests, e->window);

                void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
                memcpy(copy, e, sizeof(*e));
                small_vec_push(&s->buckets.destroy_notifies, copy);
                break;
            }

            case XCB_CONFIGURE_REQUEST: {
                xcb_configure_request_event_t* e = (xcb_configure_request_event_t*)ev;

                // If the window is destroyed later this tick, this work is dropped by rule
                pending_config_t* existing = hash_map_get(&s->buckets.configure_requests, e->window);
                if (existing) {
                    if (e->value_mask & XCB_CONFIG_WINDOW_X) existing->x = e->x;
                    if (e->value_mask & XCB_CONFIG_WINDOW_Y) existing->y = e->y;
                    if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) existing->width = e->width;
                    if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) existing->height = e->height;
                    if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) existing->border_width = e->border_width;
                    if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) existing->sibling = e->sibling;
                    if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) existing->stack_mode = e->stack_mode;

                    existing->mask |= e->value_mask;
                    counters.coalesced_drops[type]++;
                    s->buckets.coalesced++;
                } else {
                    pending_config_t* pc = arena_alloc(&s->tick_arena, sizeof(*pc));
                    pc->window = e->window;
                    pc->x = e->x;
                    pc->y = e->y;
                    pc->width = e->width;
                    pc->height = e->height;
                    pc->border_width = e->border_width;
                    pc->sibling = e->sibling;
                    pc->stack_mode = e->stack_mode;
                    pc->mask = e->value_mask;
                    hash_map_insert(&s->buckets.configure_requests, e->window, pc);
                }
                break;
            }

            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t* e = (xcb_configure_notify_event_t*)ev;

                // Coalesce by window: last wins
                xcb_configure_notify_event_t* copy = arena_alloc(&s->tick_arena, sizeof(*e));
                memcpy(copy, e, sizeof(*e));

                if (hash_map_get(&s->buckets.configure_notifies, e->window)) {
                    counters.coalesced_drops[type]++;
                    s->buckets.coalesced++;
                }
                hash_map_insert(&s->buckets.configure_notifies, e->window, copy);
                break;
            }

            case XCB_PROPERTY_NOTIFY: {
                xcb_property_notify_event_t* e = (xcb_property_notify_event_t*)ev;

                // Coalesce by (window, atom)
                uint64_t key = ((uint64_t)e->window << 32) | (uint64_t)e->atom;
                if (hash_map_get(&s->buckets.property_notifies, key)) {
                    counters.coalesced_drops[type]++;
                    s->buckets.coalesced++;
                    break;
                }

                void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
                memcpy(copy, e, sizeof(*e));
                hash_map_insert(&s->buckets.property_notifies, key, copy);
                break;
            }

            case XCB_MOTION_NOTIFY: {
                // Coalesce motion: keep only latest
                xcb_motion_notify_event_t* e = (xcb_motion_notify_event_t*)ev;
                if (s->buckets.motion_notify.valid) {
                    counters.coalesced_drops[type]++;
                    s->buckets.coalesced++;
                }
                s->buckets.motion_notify.window = e->event;
                s->buckets.motion_notify.event = *e;
                s->buckets.motion_notify.valid = true;
                break;
            }

            case XCB_ENTER_NOTIFY: {
                // Keep only the latest enter per tick
                xcb_enter_notify_event_t* e = (xcb_enter_notify_event_t*)ev;
                if (s->buckets.pointer_notify.enter_valid) {
                    counters.coalesced_drops[type]++;
                    s->buckets.coalesced++;
                }
                s->buckets.pointer_notify.enter = *e;
                s->buckets.pointer_notify.enter_valid = true;
                break;
            }

            case XCB_LEAVE_NOTIFY: {
                // Keep only the latest leave per tick
                xcb_leave_notify_event_t* e = (xcb_leave_notify_event_t*)ev;
                if (s->buckets.pointer_notify.leave_valid) {
                    counters.coalesced_drops[type]++;
                    s->buckets.coalesced++;
                }
                s->buckets.pointer_notify.leave = *e;
                s->buckets.pointer_notify.leave_valid = true;
                break;
            }

            default:
                // Intentionally ignored in this phase
                break;
        }

        free(ev);
    }

    s->buckets.ingested = count;
}

void event_process(server_t* s) {
    // Stage ordering:
    //  1. lifecycle (map/unmap/destroy) so the model is correct for the rest of the tick
    //  2. user input (key/buttons) which may change focus/stacking
    //  3. expose/menu draw
    //  4. client messages (EWMH/ICCCM requests)
    //  5. motion/enter/leave (interactive ops)
    //  6. configure requests/notifies
    //  7. property notifies
    //  8. flush dirty model -> X requests

    // 1. lifecycle
    for (size_t i = 0; i < s->buckets.map_requests.length; i++) {
        xcb_map_request_event_t* ev = s->buckets.map_requests.items[i];
        if (hash_map_get(&s->buckets.destroyed_windows, ev->window)) continue;
        wm_handle_map_request(s, ev);
    }

    for (size_t i = 0; i < s->buckets.unmap_notifies.length; i++) {
        xcb_unmap_notify_event_t* ev = s->buckets.unmap_notifies.items[i];
        if (hash_map_get(&s->buckets.destroyed_windows, ev->window)) continue;
        wm_handle_unmap_notify(s, ev);
    }

    for (size_t i = 0; i < s->buckets.destroy_notifies.length; i++) {
        xcb_destroy_notify_event_t* ev = s->buckets.destroy_notifies.items[i];
        wm_handle_destroy_notify(s, ev);
    }

    // 2. keys (keybindings)
    for (size_t i = 0; i < s->buckets.key_presses.length; i++) {
        xcb_key_press_event_t* ev = s->buckets.key_presses.items[i];
        wm_handle_key_press(s, ev);
    }

    // 3. buttons (menu, focus, move/resize)
    for (size_t i = 0; i < s->buckets.button_events.length; i++) {
        xcb_generic_event_t* gev = s->buckets.button_events.items[i];
        uint8_t type = gev->response_type & ~0x80;
        if (type == XCB_BUTTON_PRESS) {
            wm_handle_button_press(s, (xcb_button_press_event_t*)gev);
        } else if (type == XCB_BUTTON_RELEASE) {
            wm_handle_button_release(s, (xcb_button_release_event_t*)gev);
        }
    }

    // 4. expose (frames + menu)
    for (size_t i = 0; i < s->buckets.expose_events.length; i++) {
        xcb_expose_event_t* ev = s->buckets.expose_events.items[i];
        if (ev->window == s->menu.window) {
            menu_handle_expose(s);
            continue;
        }

        handle_t h = server_get_client_by_frame(s, ev->window);
        if (h != HANDLE_INVALID) {
            frame_redraw(s, h, FRAME_REDRAW_ALL);
        }
    }

    // 5. client messages (EWMH/ICCCM)
    for (size_t i = 0; i < s->buckets.client_messages.length; i++) {
        xcb_client_message_event_t* ev = s->buckets.client_messages.items[i];
        wm_handle_client_message(s, ev);
    }

    // 6. motion/enter/leave (interactive)
    if (s->buckets.pointer_notify.enter_valid) {
        // Optional: focus-follows-mouse or pointer tracking
        // wm_handle_enter_notify(s, &s->buckets.pointer_notify.enter);
    }
    if (s->buckets.pointer_notify.leave_valid) {
        // Optional: focus-follows-mouse or pointer tracking
        // wm_handle_leave_notify(s, &s->buckets.pointer_notify.leave);
    }
    if (s->buckets.motion_notify.valid) {
        wm_handle_motion_notify(s, &s->buckets.motion_notify.event);
    }

    // 7. configure requests (coalesced)
    if (s->buckets.configure_requests.capacity > 0) {
        for (size_t i = 0; i < s->buckets.configure_requests.capacity; i++) {
            hash_map_entry_t* entry = &s->buckets.configure_requests.entries[i];
            if (entry->key == 0) continue;

            pending_config_t* ev = (pending_config_t*)entry->value;
            handle_t h = server_get_client_by_window(s, ev->window);

            if (h == HANDLE_INVALID) {
                // Unmanaged window: forward the request as-is
                uint32_t mask = ev->mask;
                uint32_t values[7];
                int j = 0;
                if (mask & XCB_CONFIG_WINDOW_X) values[j++] = (uint32_t)ev->x;
                if (mask & XCB_CONFIG_WINDOW_Y) values[j++] = (uint32_t)ev->y;
                if (mask & XCB_CONFIG_WINDOW_WIDTH) values[j++] = (uint32_t)ev->width;
                if (mask & XCB_CONFIG_WINDOW_HEIGHT) values[j++] = (uint32_t)ev->height;
                if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[j++] = (uint32_t)ev->border_width;
                if (mask & XCB_CONFIG_WINDOW_SIBLING) values[j++] = (uint32_t)ev->sibling;
                if (mask & XCB_CONFIG_WINDOW_STACK_MODE) values[j++] = (uint32_t)ev->stack_mode;

                xcb_configure_window(s->conn, ev->window, mask, values);
            } else {
                wm_handle_configure_request(s, h, ev);
            }
        }
    }

    // 8. configure notifies (coalesced)
    if (s->buckets.configure_notifies.capacity > 0) {
        for (size_t i = 0; i < s->buckets.configure_notifies.capacity; i++) {
            hash_map_entry_t* entry = &s->buckets.configure_notifies.entries[i];
            if (entry->key == 0) continue;

            xcb_configure_notify_event_t* ev = (xcb_configure_notify_event_t*)entry->value;
            handle_t h = server_get_client_by_window(s, ev->window);
            if (h == HANDLE_INVALID) {
                h = server_get_client_by_frame(s, ev->window);
            }
            if (h != HANDLE_INVALID) {
                wm_handle_configure_notify(s, h, ev);
            }
        }
    }

    // 9. property changes (coalesced)
    if (s->buckets.property_notifies.capacity > 0) {
        for (size_t i = 0; i < s->buckets.property_notifies.capacity; i++) {
            hash_map_entry_t* entry = &s->buckets.property_notifies.entries[i];
            if (entry->key == 0) continue;

            xcb_property_notify_event_t* ev = (xcb_property_notify_event_t*)entry->value;
            if (hash_map_get(&s->buckets.destroyed_windows, ev->window)) continue;

            handle_t h = server_get_client_by_window(s, ev->window);
            if (h != HANDLE_INVALID) {
                wm_handle_property_notify(s, h, ev);
            }
        }
    }

    // 10. maintenance (model->X)
    wm_flush_dirty(s);

    // Root props flush should be a separate dirty mask and applied here
    // wm_flush_root_props(s);
}

void event_drain_cookies(server_t* s) {
    cookie_slot_t slot;
    void* reply;

    // Drain any replies that are ready without blocking
    while (cookie_jar_poll(&s->cookie_jar, s->conn, &slot, &reply)) {
        wm_handle_reply(s, &slot, reply);
        if (reply) free(reply);
    }
}

static void apply_reload(server_t* s) {
    // Reload is executed in tick context, not in the signal handler
    LOG_INFO("Reloading configuration");

    config_t next_config;
    config_init_defaults(&next_config);

    char path[1024];
    const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    bool loaded = false;

    if (xdg_config_home) {
        snprintf(path, sizeof(path), "%s/bbox/bbox.conf", xdg_config_home);
        loaded = config_load(&next_config, path);
    }

    if (!loaded && home) {
        snprintf(path, sizeof(path), "%s/.config/bbox/bbox.conf", home);
        loaded = config_load(&next_config, path);
    }

    if (!loaded) {
        loaded = config_load(&next_config, "/etc/bbox/bbox.conf");
    }

    // Load theme for next_config
    bool theme_loaded = false;
    if (xdg_config_home) {
        snprintf(path, sizeof(path), "%s/bbox/themerc", xdg_config_home);
        theme_loaded = theme_load(&next_config.theme, path);
    }
    if (!theme_loaded && home) {
        snprintf(path, sizeof(path), "%s/.config/bbox/themerc", home);
        theme_loaded = theme_load(&next_config.theme, path);
    }
    if (!theme_loaded) {
        theme_load(&next_config.theme, "/etc/bbox/themerc");
    }

    // Success: swap configs
    config_destroy(&s->config);
    s->config = next_config;

    // Rebuild resources that depend on config
    frame_cleanup_resources(s);
    frame_init_resources(s);

    menu_destroy(s);
    menu_init(s);

    // Rebind keys (ungrab + grab)
    // The setup function should do the right thing, but ensure it clears old binds
    // TODO: wm_setup_keys should probably ungrab first if we had a way to track all grabbed keys
    wm_setup_keys(s);

    // Mark all frames dirty for style/geometry repaint
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (!s->clients.hdr[i].live) continue;
        handle_t h = handle_make(i, s->clients.hdr[i].gen);
        client_hot_t* hot = server_chot(s, h);
        if (hot) hot->dirty |= DIRTY_FRAME_STYLE | DIRTY_GEOM;
    }

    // Update desktop count / current desktop if config changed
    if (s->config.desktop_count == 0) s->config.desktop_count = 1;

    if (s->desktop_count != s->config.desktop_count) {
        s->desktop_count = s->config.desktop_count;
        if (s->current_desktop >= s->desktop_count) {
            wm_switch_workspace(s, s->desktop_count - 1);
        }
    }

    // Publish _NET_NUMBER_OF_DESKTOPS (root prop)
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, 32,
                        1, &s->desktop_count);

    // Publish _NET_DESKTOP_NAMES
    if (s->config.desktop_names) {
        size_t total_len = 0;
        for (uint32_t i = 0; i < s->config.desktop_names_count; i++) {
            if (s->config.desktop_names[i]) {
                total_len += strlen(s->config.desktop_names[i]) + 1;
            } else {
                total_len += 1;
            }
        }
        char* buf = malloc(total_len);
        if (buf) {
            char* p = buf;
            for (uint32_t i = 0; i < s->config.desktop_names_count; i++) {
                if (s->config.desktop_names[i]) {
                    size_t l = strlen(s->config.desktop_names[i]);
                    memcpy(p, s->config.desktop_names[i], l);
                    p[l] = '\0';
                    p += l + 1;
                } else {
                    *p = '\0';
                    p++;
                }
            }
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_DESKTOP_NAMES, atoms.UTF8_STRING, 8,
                                (uint32_t)total_len, buf);
            free(buf);
        }
    }
}

volatile sig_atomic_t g_shutdown_pending = 0;
volatile sig_atomic_t g_restart_pending = 0;

void server_run(server_t* s) {
    LOG_INFO("Starting event loop");

    for (;;) {
        if (g_shutdown_pending) {
            break;
        }

        if (g_restart_pending) {
            LOG_INFO("Restarting bbox...");
            char path[1024];
            ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
            if (len != -1) {
                path[len] = '\0';
                char* args[] = {path, NULL};

                // Cleanup before exec
                server_cleanup(s);

                execv(path, args);
            }
            LOG_ERROR("Failed to restart: %s", strerror(errno));
            // If restart fails, we just continue or exit? Better exit if it was requested.
            break;
        }

        if (g_reload_pending) {
            g_reload_pending = 0;
            apply_reload(s);
        }

        uint64_t start = monotonic_time_ns();

        event_ingest(s);
        event_process(s);
        event_drain_cookies(s);

        // Flush X requests once per tick
        xcb_flush(s->conn);
        counters.x_flush_count++;

        uint64_t end = monotonic_time_ns();
        uint64_t duration = end - start;

        if (counters.tick_count == 0) {
            counters.tick_duration_min = duration;
            counters.tick_duration_max = duration;
        } else {
            if (duration < counters.tick_duration_min) counters.tick_duration_min = duration;
            if (duration > counters.tick_duration_max) counters.tick_duration_max = duration;
        }
        counters.tick_duration_sum += duration;
        counters.tick_count++;

        // Optional: avoid spinning if there was nothing to do
        // A later improvement is to block in epoll_wait with a timeout when:
        //  - no pending cookies
        //  - no interactive grab in progress
        //  - no scheduled timers
    }
}
