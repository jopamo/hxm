/* src/event.c
 * Event loop and signal handling
 */

#include "event.h"

#include <errno.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/xcb_keysyms.h>

#include "frame.h"
#include "hxm.h"
#include "wm.h"
#include "wm_internal.h"
#include "xcb_utils.h"

/*
 * event.c
 *
 * Responsibilities:
 *  - server_init / server_cleanup: lifetime of the server process and core resources
 *  - event_ingest: poll X, bucket events, and coalesce where appropriate (bounded per tick)
 *  - event_process: apply bucketed work in a stable order (lifecycle -> input -> configure -> property)
 *  - event_drain_cookies: drain async replies (never block in hot loop)
 *  - server_run: tick loop: wait -> drain -> ingest -> process -> flush
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
static void run_autostart(void);
static void apply_reload(server_t* s);
static void buckets_reset(event_buckets_t* b);
static void event_ingest_one(server_t* s, xcb_generic_event_t* ev);
static bool server_wait_for_events(server_t* s);

volatile sig_atomic_t g_shutdown_pending = 0;
volatile sig_atomic_t g_restart_pending = 0;
volatile sig_atomic_t g_reload_pending = 0;

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
    s->default_colormap = screen->default_colormap;

    s->xcb_fd = xcb_get_file_descriptor(s->conn);
    int flags = fcntl(s->xcb_fd, F_GETFL, 0);
    if (flags >= 0 && !(flags & O_NONBLOCK)) {
        if (fcntl(s->xcb_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            LOG_ERROR("fcntl(O_NONBLOCK) on xcb fd failed: %s", strerror(errno));
            exit(1);
        }
    }

    // Keysyms are used by keybinding setup and keypress handling
    s->keysyms = xcb_key_symbols_alloc(s->conn);
    if (!s->keysyms) {
        LOG_ERROR("xcb_key_symbols_alloc failed");
        exit(1);
    }

    s->damage_supported = false;
    s->damage_event_base = 0;
    s->damage_error_base = 0;
    const xcb_query_extension_reply_t* damage_ext = xcb_get_extension_data(s->conn, &xcb_damage_id);
    if (damage_ext && damage_ext->present) {
        s->damage_supported = true;
        s->damage_event_base = damage_ext->first_event;
        s->damage_error_base = damage_ext->first_error;

        xcb_damage_query_version_cookie_t cookie = xcb_damage_query_version(s->conn, 1, 1);
        xcb_damage_query_version_reply_t* reply = xcb_damage_query_version_reply(s->conn, cookie, NULL);
        if (!reply) {
            s->damage_supported = false;
            LOG_WARN("XDamage present but version query failed; disabling damage tracking");
        }
        free(reply);
    }

    s->randr_supported = false;
    s->randr_event_base = 0;
    const xcb_query_extension_reply_t* randr_ext = xcb_get_extension_data(s->conn, &xcb_randr_id);
    if (randr_ext && randr_ext->present) {
        s->randr_supported = true;
        s->randr_event_base = randr_ext->first_event;
        xcb_randr_query_version_cookie_t rc = xcb_randr_query_version(s->conn, 1, 5);
        xcb_randr_query_version_reply_t* rr = xcb_randr_query_version_reply(s->conn, rc, NULL);
        if (!rr) {
            s->randr_supported = false;
            s->randr_event_base = 0;
        }
        free(rr);
    }

    // Initialize configuration (defaults then optional load)
    config_init_defaults(&s->config);
    load_config_from_home(s);
    run_autostart();

    // Initialize workspace state from config
    s->desktop_count = s->config.desktop_count ? s->config.desktop_count : 1;

    // Restore current desktop
    s->current_desktop = 0;
    xcb_get_property_cookie_t ck =
        xcb_get_property(s->conn, 0, s->root, atoms._NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, 0, 1);
    xcb_get_property_reply_t* r = xcb_get_property_reply(s->conn, ck, NULL);
    if (r) {
        if (r->type == XCB_ATOM_CARDINAL && r->format == 32 && xcb_get_property_value_length(r) >= 4) {
            uint32_t val = *(uint32_t*)xcb_get_property_value(r);
            if (val < s->desktop_count) {
                s->current_desktop = val;
            }
        }
        free(r);
    }

    // Restore active window (focus)
    s->initial_focus = XCB_NONE;
    ck = xcb_get_property(s->conn, 0, s->root, atoms._NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 0, 1);
    r = xcb_get_property_reply(s->conn, ck, NULL);
    if (r) {
        if (r->type == XCB_ATOM_WINDOW && r->format == 32 && xcb_get_property_value_length(r) >= 4) {
            s->initial_focus = *(xcb_window_t*)xcb_get_property_value(r);
            LOG_INFO("Restoring focus to window %u", s->initial_focus);
        }
        free(r);
    }

    // Cookie jar for async request/reply handling
    cookie_jar_init(&s->cookie_jar);

    // Become WM (WM_S0 selection + supporting WM check + _NET_SUPPORTED baseline)
    wm_become(s);
    xcb_install_colormap(s->conn, s->default_colormap);
    if (s->randr_supported) {
        xcb_randr_select_input(s->conn, s->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }

    // Adopt existing windows (must happen after we are the WM)
    wm_update_monitors(s);
    wm_adopt_children(s);

    // Create epoll instance and register X connection fd
    s->epoll_fd = make_epoll_or_die();
    epoll_add_fd_or_die(s->epoll_fd, s->xcb_fd);

    // Setup signalfd
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGCHLD);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOG_ERROR("sigprocmask failed: %s", strerror(errno));
        exit(1);
    }

    s->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (s->signal_fd < 0) {
        LOG_ERROR("signalfd failed: %s", strerror(errno));
        exit(1);
    }
    epoll_add_fd_or_die(s->epoll_fd, s->signal_fd);

    // Setup timerfd (initially disarmed)
    s->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (s->timer_fd < 0) {
        LOG_ERROR("timerfd_create failed: %s", strerror(errno));
        exit(1);
    }
    epoll_add_fd_or_die(s->epoll_fd, s->timer_fd);

    // Initialize per-tick arena (64KB blocks)
    arena_init(&s->tick_arena, 64 * 1024);

    // Initialize event buckets
    small_vec_init(&s->buckets.map_requests);
    small_vec_init(&s->buckets.unmap_notifies);
    small_vec_init(&s->buckets.destroy_notifies);
    small_vec_init(&s->buckets.key_presses);
    small_vec_init(&s->buckets.button_events);
    small_vec_init(&s->buckets.client_messages);

    hash_map_init(&s->buckets.expose_regions);
    hash_map_init(&s->buckets.configure_requests);
    hash_map_init(&s->buckets.configure_notifies);
    hash_map_init(&s->buckets.destroyed_windows);
    hash_map_init(&s->buckets.property_notifies);
    hash_map_init(&s->buckets.motion_notifies);
    hash_map_init(&s->buckets.damage_regions);

    s->buckets.pointer_notify.enter_valid = false;
    s->buckets.pointer_notify.leave_valid = false;
    s->buckets.ingested = 0;
    s->buckets.coalesced = 0;

    // Initialize global maps
    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    hash_map_init(&s->pending_unmanaged_states);

    // Layer stacks and focus ring
    for (int i = 0; i < LAYER_COUNT; i++) {
        small_vec_init(&s->layers[i]);
    }
    list_init(&s->focus_history);
    s->focused_client = HANDLE_INVALID;

    // Interaction state
    s->interaction_handle = HANDLE_INVALID;

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

static void cleanup_client_visitor(void* hot, void* cold, handle_t h, void* user) {
    (void)hot;
    (void)cold;
    server_t* s = (server_t*)user;
    client_unmanage(s, h);
}

void server_cleanup(server_t* s) {
    if (s->prefetched_event) {
        free(s->prefetched_event);
        s->prefetched_event = NULL;
    }

    // Unmanage all clients (reparent back to root)
    slotmap_for_each_used(&s->clients, cleanup_client_visitor, s);

    if (s->keysyms) {
        xcb_key_symbols_free(s->keysyms);
        s->keysyms = NULL;
    }

    frame_cleanup_resources(s);
    menu_destroy(s);
    config_destroy(&s->config);

    if (s->monitors) {
        free(s->monitors);
        s->monitors = NULL;
    }

    if (s->signal_fd > 0) {
        close(s->signal_fd);
        s->signal_fd = -1;
    }
    if (s->timer_fd > 0) {
        close(s->timer_fd);
        s->timer_fd = -1;
    }
    if (s->epoll_fd > 0) {
        close(s->epoll_fd);
        s->epoll_fd = -1;
    }
    if (s->conn) {
        xcb_disconnect(s->conn);
        s->conn = NULL;
    }
    cookie_jar_destroy(&s->cookie_jar);

    arena_destroy(&s->tick_arena);

    small_vec_destroy(&s->buckets.map_requests);
    small_vec_destroy(&s->buckets.unmap_notifies);
    small_vec_destroy(&s->buckets.destroy_notifies);
    small_vec_destroy(&s->buckets.key_presses);
    small_vec_destroy(&s->buckets.button_events);
    small_vec_destroy(&s->buckets.client_messages);

    hash_map_destroy(&s->buckets.expose_regions);
    hash_map_destroy(&s->buckets.configure_requests);
    hash_map_destroy(&s->buckets.configure_notifies);
    hash_map_destroy(&s->buckets.destroyed_windows);
    hash_map_destroy(&s->buckets.property_notifies);
    hash_map_destroy(&s->buckets.motion_notifies);
    hash_map_destroy(&s->buckets.damage_regions);

    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);

    // Clean up pending states
    for (size_t i = 0; i < s->pending_unmanaged_states.capacity; i++) {
        hash_map_entry_t* entry = &s->pending_unmanaged_states.entries[i];
        if (entry->key != 0) {
            small_vec_t* v = (small_vec_t*)entry->value;
            if (v) {
                small_vec_destroy(v);
                free(v);
            }
        }
    }
    hash_map_destroy(&s->pending_unmanaged_states);

    slotmap_destroy(&s->clients);

    for (int i = 0; i < LAYER_COUNT; i++) {
        small_vec_destroy(&s->layers[i]);
    }

    // Global library cleanup for ASan
    pango_cairo_font_map_set_default(NULL);
    FcFini();
}

static int make_epoll_or_die(void) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        exit(1);
    }
    return epfd;
}

static void epoll_add_fd_or_die(int epfd, int fd) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl add fd=%d failed: %s", fd, strerror(errno));
        exit(1);
    }
}

static void run_autostart(void) {
    char path[1024];
    const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    const char* exec_path = NULL;

    // Check XDG_CONFIG_HOME
    if (xdg_config_home) {
        snprintf(path, sizeof(path), "%s/hxm/autostart", xdg_config_home);
        if (access(path, X_OK) == 0) {
            exec_path = path;
        }
    }

    // Check ~/.config
    if (!exec_path && home) {
        snprintf(path, sizeof(path), "%s/.config/hxm/autostart", home);
        if (access(path, X_OK) == 0) {
            exec_path = path;
        }
    }

    // Check /etc
    if (!exec_path) {
        if (access("/etc/hxm/autostart", X_OK) == 0) {
            exec_path = "/etc/hxm/autostart";
        }
    }

    if (exec_path) {
        LOG_INFO("Executing autostart script: %s", exec_path);
        pid_t pid = fork();
        if (pid == 0) {
            if (setsid() < 0) {
                LOG_WARN("setsid failed in autostart: %s", strerror(errno));
            }
            execl(exec_path, exec_path, NULL);
            LOG_ERROR("Failed to exec autostart script: %s", strerror(errno));
            exit(1);
        } else if (pid < 0) {
            LOG_ERROR("Failed to fork for autostart: %s", strerror(errno));
        }
    }
}

static void load_config_from_home(server_t* s) {
    char path[1024];
    bool config_loaded = false;
    const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");

    if (xdg_config_home) {
        snprintf(path, sizeof(path), "%s/hxm/hxm.conf", xdg_config_home);
        config_loaded = config_load(&s->config, path);
    }

    if (!config_loaded && home) {
        snprintf(path, sizeof(path), "%s/.config/hxm/hxm.conf", home);
        config_loaded = config_load(&s->config, path);
    }

    if (!config_loaded) {
        config_load(&s->config, "/etc/hxm/hxm.conf");
    }

    // Now try to load the theme
    bool theme_loaded = false;
    if (xdg_config_home) {
        snprintf(path, sizeof(path), "%s/hxm/themerc", xdg_config_home);
        theme_loaded = theme_load(&s->config.theme, path);
    }

    if (!theme_loaded && home) {
        snprintf(path, sizeof(path), "%s/.config/hxm/themerc", home);
        theme_loaded = theme_load(&s->config.theme, path);
    }

    if (!theme_loaded) {
        theme_load(&s->config.theme, "/etc/hxm/themerc");
    }
}

static void buckets_reset(event_buckets_t* b) {
    small_vec_clear(&b->map_requests);
    small_vec_clear(&b->unmap_notifies);
    small_vec_clear(&b->destroy_notifies);
    small_vec_clear(&b->key_presses);
    small_vec_clear(&b->button_events);
    small_vec_clear(&b->client_messages);

    // Simple per-tick reset strategy: destroy+init
    hash_map_destroy(&b->expose_regions);
    hash_map_init(&b->expose_regions);

    hash_map_destroy(&b->configure_requests);
    hash_map_init(&b->configure_requests);

    hash_map_destroy(&b->configure_notifies);
    hash_map_init(&b->configure_notifies);

    hash_map_destroy(&b->destroyed_windows);
    hash_map_init(&b->destroyed_windows);

    hash_map_destroy(&b->property_notifies);
    hash_map_init(&b->property_notifies);

    hash_map_destroy(&b->motion_notifies);
    hash_map_init(&b->motion_notifies);

    hash_map_destroy(&b->damage_regions);
    hash_map_init(&b->damage_regions);

    b->pointer_notify.enter_valid = false;
    b->pointer_notify.leave_valid = false;

    b->randr_dirty = false;
    b->randr_width = 0;
    b->randr_height = 0;

    b->ingested = 0;
    b->coalesced = 0;
}

void event_ingest(server_t* s, bool x_ready) {
    buckets_reset(&s->buckets);
    arena_reset(&s->tick_arena);

    uint64_t count = 0;
    if (s->prefetched_event) {
        uint64_t before = s->buckets.coalesced;
        event_ingest_one(s, s->prefetched_event);
        s->prefetched_event = NULL;
        if (s->buckets.coalesced == before) count++;
    }

    while (count < MAX_EVENTS_PER_TICK) {
        xcb_generic_event_t* ev = xcb_poll_for_queued_event(s->conn);
        if (!ev) break;
        uint64_t before = s->buckets.coalesced;
        event_ingest_one(s, ev);
        if (s->buckets.coalesced == before) count++;
    }

    if (!x_ready) {
        s->x_poll_immediate = (count >= MAX_EVENTS_PER_TICK);
        s->buckets.ingested = count;
        return;
    }

    while (count < MAX_EVENTS_PER_TICK) {
        xcb_generic_event_t* ev = xcb_poll_for_event(s->conn);
        if (!ev) break;
        uint64_t before = s->buckets.coalesced;
        event_ingest_one(s, ev);
        if (s->buckets.coalesced == before) count++;
    }

    s->x_poll_immediate = (count >= MAX_EVENTS_PER_TICK);
    s->buckets.ingested = count;
}

static void event_ingest_one(server_t* s, xcb_generic_event_t* ev) {
    uint8_t type = ev->response_type & ~0x80;
    counters.events_seen[type]++;

    if (s->damage_supported && type == (uint8_t)(s->damage_event_base + XCB_DAMAGE_NOTIFY)) {
        xcb_damage_notify_event_t* e = (xcb_damage_notify_event_t*)ev;
        dirty_region_t* region = hash_map_get(&s->buckets.damage_regions, e->drawable);
        if (region) {
            dirty_region_union_rect(region, e->area.x, e->area.y, e->area.width, e->area.height);
            counters.coalesced_drops[type]++;
            s->buckets.coalesced++;
            TRACE_LOG("coalesce damage drawable=%u area=%dx%d+%d+%d", e->drawable, e->area.width, e->area.height,
                      e->area.x, e->area.y);
        } else {
            dirty_region_t* copy = arena_alloc(&s->tick_arena, sizeof(*copy));
            *copy = dirty_region_make(e->area.x, e->area.y, e->area.width, e->area.height);
            hash_map_insert(&s->buckets.damage_regions, e->drawable, copy);
        }
        free(ev);
        return;
    }

    if (s->randr_supported && type == (uint8_t)(s->randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY)) {
        xcb_randr_screen_change_notify_event_t* e = (xcb_randr_screen_change_notify_event_t*)ev;
        if (s->buckets.randr_dirty) {
            counters.coalesced_drops[type]++;
            s->buckets.coalesced++;
        }
        s->buckets.randr_dirty = true;
        s->buckets.randr_width = e->width;
        s->buckets.randr_height = e->height;
        TRACE_LOG("coalesce randr notify width=%u height=%u", e->width, e->height);

        free(ev);
        return;
    }

    switch (type) {
        case XCB_EXPOSE: {
            xcb_expose_event_t* e = (xcb_expose_event_t*)ev;
            dirty_region_t* region = hash_map_get(&s->buckets.expose_regions, e->window);
            if (region) {
                dirty_region_union_rect(region, e->x, e->y, e->width, e->height);
                counters.coalesced_drops[type]++;
                s->buckets.coalesced++;
            } else {
                dirty_region_t* copy = arena_alloc(&s->tick_arena, sizeof(*copy));
                *copy = dirty_region_make(e->x, e->y, e->width, e->height);
                hash_map_insert(&s->buckets.expose_regions, e->window, copy);
            }
            break;
        }

        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE: {
            size_t sz =
                (type == XCB_BUTTON_PRESS) ? sizeof(xcb_button_press_event_t) : sizeof(xcb_button_release_event_t);
            void* copy = arena_alloc(&s->tick_arena, sz);
            memcpy(copy, ev, sz);
            small_vec_push(&s->buckets.button_events, copy);
            break;
        }

        case XCB_CLIENT_MESSAGE: {
            xcb_client_message_event_t* e = (xcb_client_message_event_t*)ev;
            TRACE_LOG("ingest client_message win=%u type=%u format=%u", e->window, e->type, e->format);
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
            TRACE_LOG("ingest map_request win=%u parent=%u", e->window, e->parent);
            void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
            memcpy(copy, e, sizeof(*e));
            small_vec_push(&s->buckets.map_requests, copy);
            break;
        }

        case XCB_MAP_NOTIFY: {
            xcb_map_notify_event_t* e = (xcb_map_notify_event_t*)ev;
            (void)e;
            TRACE_LOG("ingest map_notify win=%u event=%u override=%u", e->window, e->event, e->override_redirect);
            break;
        }

        case XCB_COLORMAP_NOTIFY: {
            xcb_colormap_notify_event_t* e = (xcb_colormap_notify_event_t*)ev;
            wm_handle_colormap_notify(s, e);
            break;
        }

        case XCB_REPARENT_NOTIFY: {
            xcb_reparent_notify_event_t* e = (xcb_reparent_notify_event_t*)ev;
            (void)e;
            TRACE_LOG("ingest reparent_notify win=%u parent=%u x=%d y=%d override=%u", e->window, e->parent, e->x, e->y,
                      e->override_redirect);
            break;
        }

        case XCB_UNMAP_NOTIFY: {
            xcb_unmap_notify_event_t* e = (xcb_unmap_notify_event_t*)ev;
            TRACE_LOG("ingest unmap_notify win=%u event=%u from_configure=%u", e->window, e->event, e->from_configure);
            void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
            memcpy(copy, e, sizeof(*e));
            small_vec_push(&s->buckets.unmap_notifies, copy);
            break;
        }

        case XCB_DESTROY_NOTIFY: {
            xcb_destroy_notify_event_t* e = (xcb_destroy_notify_event_t*)ev;
            TRACE_LOG("ingest destroy_notify win=%u event=%u", e->window, e->event);

            hash_map_insert(&s->buckets.destroyed_windows, e->window, (void*)1);
            hash_map_remove(&s->buckets.configure_requests, e->window);

            void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
            memcpy(copy, e, sizeof(*e));
            small_vec_push(&s->buckets.destroy_notifies, copy);
            break;
        }

        case XCB_CONFIGURE_REQUEST: {
            xcb_configure_request_event_t* e = (xcb_configure_request_event_t*)ev;

            pending_config_t* existing = hash_map_get(&s->buckets.configure_requests, e->window);
            if (existing) {
                TRACE_LOG("coalesce configure_request win=%u mask=0x%x", e->window, e->value_mask);
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
                TRACE_LOG("ingest configure_request win=%u mask=0x%x", e->window, e->value_mask);
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
            xcb_configure_notify_event_t* copy = arena_alloc(&s->tick_arena, sizeof(*e));
            memcpy(copy, e, sizeof(*e));

            if (hash_map_get(&s->buckets.configure_notifies, e->window)) {
                counters.coalesced_drops[type]++;
                s->buckets.coalesced++;
                TRACE_LOG("coalesce configure_notify win=%u", e->window);
            }
            hash_map_insert(&s->buckets.configure_notifies, e->window, copy);
            break;
        }

        case XCB_PROPERTY_NOTIFY: {
            xcb_property_notify_event_t* e = (xcb_property_notify_event_t*)ev;

            uint64_t key = ((uint64_t)e->window << 32) | (uint64_t)e->atom;
            if (hash_map_get(&s->buckets.property_notifies, key)) {
                counters.coalesced_drops[type]++;
                s->buckets.coalesced++;
                TRACE_LOG("coalesce property_notify win=%u atom=%u (%s)", e->window, e->atom, atom_name(e->atom));
                break;
            }

            TRACE_LOG("ingest property_notify win=%u atom=%u (%s) state=%u", e->window, e->atom, atom_name(e->atom),
                      e->state);
            void* copy = arena_alloc(&s->tick_arena, sizeof(*e));
            memcpy(copy, e, sizeof(*e));
            hash_map_insert(&s->buckets.property_notifies, key, copy);
            break;
        }

        case XCB_MOTION_NOTIFY: {
            xcb_motion_notify_event_t* e = (xcb_motion_notify_event_t*)ev;
            xcb_motion_notify_event_t* existing = hash_map_get(&s->buckets.motion_notifies, e->event);
            if (existing) {
                counters.coalesced_drops[type]++;
                s->buckets.coalesced++;
                *existing = *e;
                break;
            }
            xcb_motion_notify_event_t* copy = arena_alloc(&s->tick_arena, sizeof(*e));
            *copy = *e;
            hash_map_insert(&s->buckets.motion_notifies, e->event, copy);
            break;
        }

        case XCB_ENTER_NOTIFY: {
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
            xcb_leave_notify_event_t* e = (xcb_leave_notify_event_t*)ev;
            if (s->buckets.pointer_notify.leave_valid) {
                counters.coalesced_drops[type]++;
                s->buckets.coalesced++;
            }
            s->buckets.pointer_notify.leave = *e;
            s->buckets.pointer_notify.leave_valid = true;
            break;
        }

        case XCB_NO_EXPOSURE:
        case XCB_CREATE_NOTIFY:
        case XCB_FOCUS_IN:
        case XCB_FOCUS_OUT:
        case XCB_MAPPING_NOTIFY:
            // These are noisy and we don't use them for anything right now
            break;

        default:
            counters.events_unhandled[type]++;
            break;
    }

    free(ev);
}

static void log_unhandled_summary(void) {
    static rl_t rl = {0};
    uint64_t now = monotonic_time_ns();
    if (!rl_allow(&rl, now, 1000000000)) return;  // 1s

    bool any = false;
    char buf[1024];
    int pos = 0;
    buf[0] = '\0';

    for (int i = 0; i < 256; i++) {
        if (counters.events_unhandled[i] > 0) {
            if (!any) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, "EV unhandled:");
                any = true;
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %d=%lu", i, counters.events_unhandled[i]);
            counters.events_unhandled[i] = 0;
            if (pos >= (int)sizeof(buf) - 32) break;
        }
    }

    if (any) {
        LOG_INFO("%s", buf);
    }
}

void event_process(server_t* s) {
    static rl_t rl_process = {0};
    uint64_t now = monotonic_time_ns();
    if (rl_allow(&rl_process, now, 1000000000)) {  // 1s
        TRACE_LOG("event_process buckets map=%zu unmap=%zu destroy=%zu client=%zu configure=%zu property=%zu",
                  s->buckets.map_requests.length, s->buckets.unmap_notifies.length, s->buckets.destroy_notifies.length,
                  s->buckets.client_messages.length, hash_map_size(&s->buckets.configure_requests),
                  hash_map_size(&s->buckets.property_notifies));
    }
    // 1. lifecycle
    for (size_t i = 0; i < s->buckets.map_requests.length; i++) {
        xcb_map_request_event_t* ev = s->buckets.map_requests.items[i];
        if (hash_map_get(&s->buckets.destroyed_windows, ev->window)) continue;
        TRACE_LOG("process map_request win=%u", ev->window);
        wm_handle_map_request(s, ev);
    }

    for (size_t i = 0; i < s->buckets.unmap_notifies.length; i++) {
        xcb_unmap_notify_event_t* ev = s->buckets.unmap_notifies.items[i];
        if (hash_map_get(&s->buckets.destroyed_windows, ev->window)) continue;
        TRACE_LOG("process unmap_notify win=%u event=%u", ev->window, ev->event);
        wm_handle_unmap_notify(s, ev);
    }

    for (size_t i = 0; i < s->buckets.destroy_notifies.length; i++) {
        xcb_destroy_notify_event_t* ev = s->buckets.destroy_notifies.items[i];
        TRACE_LOG("process destroy_notify win=%u event=%u", ev->window, ev->event);
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
    if (s->buckets.expose_regions.capacity > 0) {
        for (size_t i = 0; i < s->buckets.expose_regions.capacity; i++) {
            hash_map_entry_t* entry = &s->buckets.expose_regions.entries[i];
            if (entry->key == 0) continue;

            xcb_window_t win = (xcb_window_t)entry->key;
            dirty_region_t* region = (dirty_region_t*)entry->value;
            if (!region || !region->valid) continue;

            if (win == s->menu.window) {
                menu_handle_expose_region(s, region);
                continue;
            }

            handle_t h = server_get_client_by_frame(s, win);
            if (h != HANDLE_INVALID) {
                frame_redraw_region(s, h, region);
            }
        }
    }

    // 5. client messages (EWMH/ICCCM)
    for (size_t i = 0; i < s->buckets.client_messages.length; i++) {
        xcb_client_message_event_t* ev = s->buckets.client_messages.items[i];
        TRACE_LOG("process client_message win=%u type=%u", ev->window, ev->type);
        wm_handle_client_message(s, ev);
    }

    // 6. motion/enter/leave
    if (s->buckets.pointer_notify.enter_valid) {
        // wm_handle_enter_notify(s, &s->buckets.pointer_notify.enter);
    }
    if (s->buckets.pointer_notify.leave_valid) {
        // wm_handle_leave_notify(s, &s->buckets.pointer_notify.leave);
    }
    if (s->buckets.motion_notifies.capacity > 0) {
        for (size_t i = 0; i < s->buckets.motion_notifies.capacity; i++) {
            hash_map_entry_t* entry = &s->buckets.motion_notifies.entries[i];
            if (entry->key == 0) continue;

            xcb_motion_notify_event_t* ev = (xcb_motion_notify_event_t*)entry->value;
            if (ev) wm_handle_motion_notify(s, ev);
        }
    }

    // 7. configure requests (coalesced)
    if (s->buckets.configure_requests.capacity > 0) {
        for (size_t i = 0; i < s->buckets.configure_requests.capacity; i++) {
            hash_map_entry_t* entry = &s->buckets.configure_requests.entries[i];
            if (entry->key == 0) continue;

            pending_config_t* ev = (pending_config_t*)entry->value;
            handle_t h = server_get_client_by_window(s, ev->window);

            if (h == HANDLE_INVALID) {
                uint32_t mask = ev->mask;
                uint32_t values[7];
                int j = 0;
                int32_t x = ev->x;
                int32_t y = ev->y;
                if (mask & XCB_CONFIG_WINDOW_X) values[j++] = (uint32_t)x;
                if (mask & XCB_CONFIG_WINDOW_Y) values[j++] = (uint32_t)y;
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
            if (h == HANDLE_INVALID) h = server_get_client_by_frame(s, ev->window);
            if (h != HANDLE_INVALID) {
                wm_handle_configure_notify(s, h, ev);
            }
        }
    }

    // 9. property notifies (coalesced)
    if (s->buckets.property_notifies.capacity > 0) {
        for (size_t i = 0; i < s->buckets.property_notifies.capacity; i++) {
            hash_map_entry_t* entry = &s->buckets.property_notifies.entries[i];
            if (entry->key == 0) continue;

            xcb_property_notify_event_t* ev = (xcb_property_notify_event_t*)entry->value;
            // Fix 1: Ignore _NET_WORKAREA on root to prevent feedback loop
            if (ev->window == s->root && ev->atom == atoms._NET_WORKAREA) continue;

            if (hash_map_get(&s->buckets.destroyed_windows, ev->window)) continue;

            handle_t h = server_get_client_by_window(s, ev->window);
            if (h != HANDLE_INVALID) {
                wm_handle_property_notify(s, h, ev);
            }
        }
    }

    // 10. damage (coalesced)
    if (s->buckets.damage_regions.capacity > 0) {
        for (size_t i = 0; i < s->buckets.damage_regions.capacity; i++) {
            hash_map_entry_t* entry = &s->buckets.damage_regions.entries[i];
            if (entry->key == 0) continue;

            xcb_window_t win = (xcb_window_t)entry->key;
            dirty_region_t* region = (dirty_region_t*)entry->value;
            if (!region || !region->valid) continue;

            handle_t h = server_get_client_by_window(s, win);
            if (h == HANDLE_INVALID) continue;
            client_hot_t* hot = server_chot(s, h);
            if (!hot) continue;

            dirty_region_union(&hot->damage_region, region);
            if (hot->damage != XCB_NONE) {
                xcb_damage_subtract(s->conn, hot->damage, XCB_NONE, XCB_NONE);
            }
        }
    }

    // 11. RandR (coalesced)
    if (s->buckets.randr_dirty) {
        TRACE_LOG("process randr dirty width=%u height=%u", s->buckets.randr_width, s->buckets.randr_height);
        wm_update_monitors(s);
        uint32_t geometry[] = {s->buckets.randr_width, s->buckets.randr_height};
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_DESKTOP_GEOMETRY, XCB_ATOM_CARDINAL, 32,
                            2, geometry);

        rect_t wa;
        wm_compute_workarea(s, &wa);
        wm_publish_workarea(s, &wa);

        if (!s->config.fullscreen_use_workarea) {
            for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
                if (!slotmap_is_used_idx(&s->clients, i)) continue;
                client_hot_t* hot = (client_hot_t*)slotmap_hot_at(&s->clients, i);
                if (hot->layer != LAYER_FULLSCREEN) continue;
                wm_get_monitor_geometry(s, hot, &hot->desired);
                hot->dirty |= DIRTY_GEOM;
            }
        }
    }

    // 12. maintenance
    wm_flush_dirty(s);
}

void server_schedule_timer(server_t* s, int ms) {
    if (s->timer_fd <= 0) return;
    struct itimerspec its;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;  // One-shot
    its.it_value.tv_sec = ms / 1000;
    its.it_value.tv_nsec = (ms % 1000) * 1000000;
    if (ms > 0 && its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) {
        its.it_value.tv_nsec = 1;  // Minimal positive value
    }
    timerfd_settime(s->timer_fd, 0, &its, NULL);
}

void event_drain_cookies(server_t* s) {
    if (!s) return;

    for (int pass = 0; pass < 3; pass++) {
        size_t before_live = s->cookie_jar.live_count;

        cookie_jar_drain(&s->cookie_jar, s->conn, s, COOKIE_JAR_MAX_REPLIES_PER_TICK);

        bool cookies_progress = (s->cookie_jar.live_count != before_live);

        bool need_flush = (s->root_dirty != 0);
        if (!need_flush) {
            for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
                if (!slotmap_is_used_idx(&s->clients, i)) continue;
                client_hot_t* hot = (client_hot_t*)slotmap_hot_at(&s->clients, i);
                if (hot->dirty != DIRTY_NONE) {
                    need_flush = true;
                    break;
                }
            }
        }

        bool flushed = false;
        if (need_flush) {
            wm_flush_dirty(s);
            flushed = true;
        }

        if (!cookies_progress && !flushed) break;
        if (s->cookie_jar.live_count == 0) break;
    }
}

static void apply_reload(server_t* s) {
    LOG_INFO("Reloading configuration");

    config_t next_config;
    config_init_defaults(&next_config);

    char path[1024];
    const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    bool loaded = false;

    if (xdg_config_home) {
        snprintf(path, sizeof(path), "%s/hxm/hxm.conf", xdg_config_home);
        loaded = config_load(&next_config, path);
    }

    if (!loaded && home) {
        snprintf(path, sizeof(path), "%s/.config/hxm/hxm.conf", home);
        loaded = config_load(&next_config, path);
    }

    if (!loaded) {
        loaded = config_load(&next_config, "/etc/hxm/hxm.conf");
    }

    bool theme_loaded = false;
    if (xdg_config_home) {
        snprintf(path, sizeof(path), "%s/hxm/themerc", xdg_config_home);
        theme_loaded = theme_load(&next_config.theme, path);
    }
    if (!theme_loaded && home) {
        snprintf(path, sizeof(path), "%s/.config/hxm/themerc", home);
        theme_loaded = theme_load(&next_config.theme, path);
    }
    if (!theme_loaded) {
        theme_load(&next_config.theme, "/etc/hxm/themerc");
    }

    config_destroy(&s->config);
    s->config = next_config;

    uint32_t desired = s->config.desktop_count ? s->config.desktop_count : s->desktop_count;
    if (desired == 0) desired = 1;
    if (desired != s->desktop_count) {
        s->desktop_count = desired;
        if (s->current_desktop >= s->desktop_count) s->current_desktop = 0;

        for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
            if (!slotmap_is_used_idx(&s->clients, i)) continue;
            client_hot_t* hot = (client_hot_t*)slotmap_hot_at(&s->clients, i);
            if (hot->sticky) continue;
            if (hot->desktop >= (int32_t)s->desktop_count) {
                hot->desktop = (int32_t)s->current_desktop;
                uint32_t prop_val = (uint32_t)hot->desktop;
                xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL,
                                    32, 1, &prop_val);
            }
        }
    }

    wm_publish_desktop_props(s);
    s->workarea_dirty = true;

    frame_cleanup_resources(s);
    frame_init_resources(s);

    menu_destroy(s);
    menu_init(s);

    wm_setup_keys(s);

    for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
        if (!slotmap_is_used_idx(&s->clients, i)) continue;
        client_hot_t* hot = (client_hot_t*)slotmap_hot_at(&s->clients, i);
        hot->dirty |= DIRTY_FRAME_STYLE | DIRTY_GEOM;
    }

    wm_publish_desktop_props(s);
}

static void event_handle_signals(server_t* s) {
    struct signalfd_siginfo fdsi;
    ssize_t s_size = read(s->signal_fd, &fdsi, sizeof(fdsi));
    if (s_size != sizeof(fdsi)) return;

    int sig = (int)fdsi.ssi_signo;
    switch (sig) {
        case SIGHUP:
            LOG_INFO("Reload requested (signalfd)");
            g_reload_pending = 1;
            break;
        case SIGINT:
        case SIGTERM:
            LOG_INFO("Shutting down (signalfd)");
            g_shutdown_pending = 1;
            break;
        case SIGUSR1:
            counters_dump();
            break;
        case SIGUSR2:
            LOG_INFO("Restart requested (signalfd)");
            g_restart_pending = 1;
            break;
        case SIGCHLD:
            // Handle zombies if we fork processes
            while (waitpid(-1, NULL, WNOHANG) > 0);
            break;
    }
}

static bool server_wait_for_events(server_t* s) {
    if (s->epoll_fd <= 0) return true;

    if (s->prefetched_event) return true;

    xcb_generic_event_t* queued = xcb_poll_for_queued_event(s->conn);
    if (queued) {
        s->prefetched_event = queued;
        return true;
    }

    if (xcb_connection_has_error(s->conn)) {
        g_shutdown_pending = 1;
        return false;
    }

    // If we have pending cookies, make sure requests are flushed so replies can arrive
    if (cookie_jar_has_pending(&s->cookie_jar)) {
        xcb_flush(s->conn);
    }

    struct epoll_event evs[8];
    int timeout_ms = -1;

    for (;;) {
        int n = epoll_wait(s->epoll_fd, evs, 8, timeout_ms);
        if (n > 0) {
            bool x_ready = false;
            for (int i = 0; i < n; i++) {
                if (evs[i].events & (EPOLLERR | EPOLLHUP)) {
                    if (evs[i].data.fd == s->xcb_fd) {
                        g_shutdown_pending = 1;
                        return false;
                    }
                    continue;
                }

                if (evs[i].data.fd == s->xcb_fd) {
                    x_ready = true;
                } else if (evs[i].data.fd == s->signal_fd) {
                    event_handle_signals(s);
                } else if (evs[i].data.fd == s->timer_fd) {
                    uint64_t expirations;
                    (void)read(s->timer_fd, &expirations, sizeof(expirations));
                    // Timer logic if needed
                }
            }

            if (x_ready) return true;
            if (g_shutdown_pending || g_restart_pending || g_reload_pending) return false;

            // If we woke up but X wasn't ready (just signals), and we aren't stopping,
            // we might want to continue waiting or return false to let the loop run once.
            // Returning false will trigger a tick with 0 events, which is fine.
            return false;
        }
        if (n == 0) return false;
        if (errno == EINTR) {
            if (g_shutdown_pending || g_restart_pending || g_reload_pending) return false;
            continue;
        }
        LOG_ERROR("epoll_wait failed: %s", strerror(errno));
        return false;
    }
}

void server_run(server_t* s) {
    LOG_INFO("Starting event loop");

    for (;;) {
        if (g_shutdown_pending) break;

        if (g_restart_pending) {
            LOG_INFO("Restarting hxm...");
            s->restarting = true;
            char path[1024];
            ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
            if (len != -1) {
                path[len] = '\0';
                char* args[] = {path, NULL};

                server_cleanup(s);
                execv(path, args);
            }
            LOG_ERROR("Failed to restart: %s", strerror(errno));
            break;
        }

        bool reload_applied = false;
        if (g_reload_pending) {
            g_reload_pending = 0;
            apply_reload(s);
            reload_applied = true;
        }

        bool x_ready = false;
        if (!reload_applied) {
            if (s->x_poll_immediate) {
                x_ready = true;
            } else {
                x_ready = server_wait_for_events(s);
            }
        }

        uint64_t start = monotonic_time_ns();
        s->txn_id++;

        event_ingest(s, x_ready);
        event_drain_cookies(s);
        event_process(s);

        // Fix 3: Debounced workarea calculation
        if (s->workarea_dirty) {
            rect_t wa;
            wm_compute_workarea(s, &wa);
            static rl_t rl_wa = {0};
            if (rl_allow(&rl_wa, monotonic_time_ns(), 1000000000)) {
                TRACE_LOG("publish_workarea debounced x=%d y=%d w=%u h=%u", wa.x, wa.y, wa.w, wa.h);
            }
            wm_publish_workarea(s, &wa);
            s->workarea_dirty = false;
        }

        xcb_flush(s->conn);
        counters.x_flush_count++;

        log_unhandled_summary();

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
    }
}
