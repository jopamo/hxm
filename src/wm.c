#include "wm.h"

#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>

#include "bbox.h"
#include "client.h"
#include "event.h"
#include "frame.h"

// Small helpers

static uint32_t clean_mods(uint16_t state) {
    // Mask out NumLock/ScrollLock if they interfere (common in WMs)
    return state & ~(XCB_MOD_MASK_2 | XCB_MOD_MASK_5 | XCB_MOD_MASK_LOCK);
}

static bool check_wm_s0_available(server_t* s) {
    xcb_get_selection_owner_cookie_t cookie = xcb_get_selection_owner(s->conn, atoms.WM_S0);
    xcb_get_selection_owner_reply_t* reply = xcb_get_selection_owner_reply(s->conn, cookie, NULL);
    if (!reply) return false;
    bool available = (reply->owner == XCB_NONE);
    free(reply);
    return available;
}

static void set_root_cursor(server_t* s, uint16_t cursor_font_id) {
    xcb_font_t font = xcb_generate_id(s->conn);
    xcb_open_font(s->conn, font, (uint16_t)strlen("cursor"), "cursor");
    xcb_cursor_t cursor = xcb_generate_id(s->conn);
    xcb_create_glyph_cursor(s->conn, cursor, font, font, cursor_font_id, cursor_font_id + 1, 0, 0, 0, 0xffff, 0xffff,
                            0xffff);
    uint32_t mask = XCB_CW_CURSOR;
    uint32_t values[] = {cursor};
    xcb_change_window_attributes(s->conn, s->root, mask, values);
    xcb_free_cursor(s->conn, cursor);
    xcb_close_font(s->conn, font);
}

static void spawn(const char* cmd) {
    if (fork() == 0) {
        if (fork() == 0) {
            setsid();
            char* args[] = {"/bin/sh", "-c", (char*)cmd, NULL};
            execvp(args[0], args);
            exit(1);
        }
        exit(0);
    }
    wait(NULL);
}

static bool should_raise_on_click(client_hot_t* c, xcb_button_t button) {
    (void)c;
    return (button == 1 || button == 3);
}

static void wm_client_set_maximize(server_t* s, client_hot_t* hot, bool max_horz, bool max_vert);

void wm_publish_desktop_props(server_t* s) {
    xcb_connection_t* conn = s->conn;
    xcb_window_t root = s->root;

    if (s->desktop_count == 0) s->desktop_count = 1;
    if (s->current_desktop >= s->desktop_count) s->current_desktop = 0;

    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, 32, 1,
                        &s->desktop_count);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, 32, 1,
                        &s->current_desktop);

    size_t name_bytes = 0;
    for (uint32_t i = 0; i < s->desktop_count; i++) {
        const char* name = NULL;
        char fallback[32];
        if (s->config.desktop_names && i < s->config.desktop_names_count) {
            name = s->config.desktop_names[i];
        }
        if (!name || name[0] == '\0') {
            snprintf(fallback, sizeof(fallback), "%u", i + 1);
            name = fallback;
        }
        name_bytes += strlen(name) + 1;
    }

    char* buf = malloc(name_bytes);
    if (buf) {
        size_t offset = 0;
        for (uint32_t i = 0; i < s->desktop_count; i++) {
            const char* name = NULL;
            char fallback[32];
            if (s->config.desktop_names && i < s->config.desktop_names_count) {
                name = s->config.desktop_names[i];
            }
            if (!name || name[0] == '\0') {
                snprintf(fallback, sizeof(fallback), "%u", i + 1);
                name = fallback;
            }
            size_t len = strlen(name) + 1;
            memcpy(buf + offset, name, len);
            offset += len;
        }
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_DESKTOP_NAMES, atoms.UTF8_STRING, 8,
                            (uint32_t)name_bytes, buf);
        free(buf);
    }

    uint32_t* viewport = calloc(s->desktop_count * 2, sizeof(uint32_t));
    if (viewport) {
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_DESKTOP_VIEWPORT, XCB_ATOM_CARDINAL, 32,
                            s->desktop_count * 2, viewport);
        free(viewport);
    }
}

void wm_compute_workarea(server_t* s, rect_t* out) {
    if (!s || !out) return;

    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
    int32_t l = 0, r = (int32_t)screen->width_in_pixels, t = 0, b = (int32_t)screen->height_in_pixels;

    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (!s->clients.hdr[i].live) continue;

        handle_t h = handle_make(i, s->clients.hdr[i].gen);
        client_hot_t* c = server_chot(s, h);
        if (!c) continue;
        if (c->state != STATE_MAPPED) continue;

        if ((int32_t)c->strut.left > l) l = (int32_t)c->strut.left;
        if ((int32_t)((int32_t)screen->width_in_pixels - (int32_t)c->strut.right) < r)
            r = (int32_t)screen->width_in_pixels - (int32_t)c->strut.right;
        if ((int32_t)c->strut.top > t) t = (int32_t)c->strut.top;
        if ((int32_t)((int32_t)screen->height_in_pixels - (int32_t)c->strut.bottom) < b)
            b = (int32_t)screen->height_in_pixels - (int32_t)c->strut.bottom;
    }

    out->x = (int16_t)l;
    out->y = (int16_t)t;
    out->w = (uint16_t)((r > l) ? (r - l) : 0);
    out->h = (uint16_t)((b > t) ? (b - t) : 0);
}

static void wm_publish_workarea(server_t* s, const rect_t* wa) {
    if (!s || !wa) return;

    bool changed = (memcmp(&s->workarea, wa, sizeof(rect_t)) != 0);
    s->workarea = *wa;

    uint32_t n = s->desktop_count ? s->desktop_count : 1;
    uint32_t* wa_vals = malloc(n * 4 * sizeof(uint32_t));
    if (wa_vals) {
        for (uint32_t i = 0; i < n; i++) {
            wa_vals[i * 4 + 0] = (uint32_t)s->workarea.x;
            wa_vals[i * 4 + 1] = (uint32_t)s->workarea.y;
            wa_vals[i * 4 + 2] = (uint32_t)s->workarea.w;
            wa_vals[i * 4 + 3] = (uint32_t)s->workarea.h;
        }
        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_WORKAREA, XCB_ATOM_CARDINAL, 32, n * 4,
                            wa_vals);
        free(wa_vals);
    }

    if (!changed) return;

    // Re-apply workarea-dependent geometry for maximized/fullscreen windows.
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (!s->clients.hdr[i].live) continue;
        handle_t h = handle_make(i, s->clients.hdr[i].gen);
        client_hot_t* hot = server_chot(s, h);
        if (!hot) continue;
        if (hot->state == STATE_UNMANAGING || hot->state == STATE_DESTROYED) continue;

        if (hot->layer == LAYER_FULLSCREEN && s->config.fullscreen_use_workarea) {
            hot->desired = s->workarea;
            hot->dirty |= DIRTY_GEOM;
        } else if (hot->maximized_horz || hot->maximized_vert) {
            wm_client_set_maximize(s, hot, hot->maximized_horz, hot->maximized_vert);
        }
    }
}

static void wm_client_apply_maximize(server_t* s, client_hot_t* hot) {
    uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
    uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;

    if (hot->maximized_horz) {
        int32_t w = (int32_t)s->workarea.w - 2 * (int32_t)bw;
        hot->desired.x = s->workarea.x;
        hot->desired.w = (uint16_t)((w > 0) ? w : 0);
    }

    if (hot->maximized_vert) {
        int32_t h = (int32_t)s->workarea.h - (int32_t)th - (int32_t)bw;
        hot->desired.y = s->workarea.y;
        hot->desired.h = (uint16_t)((h > 0) ? h : 0);
    }
}

static void wm_client_set_maximize(server_t* s, client_hot_t* hot, bool max_horz, bool max_vert) {
    if (!hot) return;
    if (hot->layer == LAYER_FULLSCREEN) return;

    bool had_any = hot->maximized_horz || hot->maximized_vert;
    bool want_any = max_horz || max_vert;

    if (want_any && !had_any) {
        hot->saved_maximize_geom = hot->server;
        hot->saved_maximize_valid = true;
    }

    hot->maximized_horz = max_horz;
    hot->maximized_vert = max_vert;

    if (!want_any) {
        if (hot->saved_maximize_valid) {
            hot->desired = hot->saved_maximize_geom;
            hot->saved_maximize_valid = false;
        } else {
            hot->desired = hot->server;
        }
    } else {
        hot->desired = hot->server;
        if (hot->saved_maximize_valid) {
            if (!max_horz) {
                hot->desired.x = hot->saved_maximize_geom.x;
                hot->desired.w = hot->saved_maximize_geom.w;
            }
            if (!max_vert) {
                hot->desired.y = hot->saved_maximize_geom.y;
                hot->desired.h = hot->saved_maximize_geom.h;
            }
        }
        wm_client_apply_maximize(s, hot);
    }

    hot->dirty |= DIRTY_GEOM | DIRTY_STATE;
}

void wm_become(server_t* s) {
    xcb_connection_t* conn = s->conn;
    xcb_window_t root = s->root;

    if (!check_wm_s0_available(s)) {
        LOG_ERROR("Refusing to become WM: WM_S0 is already owned");
        return;
    }

    // Select SubstructureRedirect on root (WM_S0 ownership)
    uint32_t root_events = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_BUTTON_PRESS;

    xcb_void_cookie_t cwa = xcb_change_window_attributes_checked(conn, root, XCB_CW_EVENT_MASK, &root_events);
    xcb_generic_error_t* err = xcb_request_check(conn, cwa);
    if (err) {
        // If this hits, it's usually BadAccess (another WM)
        LOG_ERROR("Failed to select root events (likely another WM). error_code=%u", err->error_code);
        free(err);
        return;
    }

    // Set cursor (left_ptr)
    set_root_cursor(s, XC_left_ptr);

    // Set _NET_SUPPORTED with basic atoms
    xcb_atom_t supported_atoms[] = {
        atoms._NET_SUPPORTED,
        atoms._NET_SUPPORTING_WM_CHECK,
        atoms._NET_CLIENT_LIST,
        atoms._NET_CLIENT_LIST_STACKING,
        atoms._NET_ACTIVE_WINDOW,
        atoms._NET_WM_NAME,
        atoms._NET_WM_STATE,
        atoms._NET_WM_STATE_FULLSCREEN,
        atoms._NET_WM_STATE_ABOVE,
        atoms._NET_WM_STATE_BELOW,
        atoms._NET_WM_STATE_STICKY,
        atoms._NET_WM_STATE_DEMANDS_ATTENTION,
        atoms._NET_WM_STATE_HIDDEN,
        atoms._NET_WM_STATE_MAXIMIZED_HORZ,
        atoms._NET_WM_STATE_MAXIMIZED_VERT,
        atoms._NET_WM_STATE_FOCUSED,
        atoms._NET_WM_WINDOW_TYPE,
        atoms._NET_WM_WINDOW_TYPE_DOCK,
        atoms._NET_WM_WINDOW_TYPE_DIALOG,
        atoms._NET_WM_WINDOW_TYPE_NOTIFICATION,
        atoms._NET_WM_WINDOW_TYPE_NORMAL,
        atoms._NET_WM_WINDOW_TYPE_DESKTOP,
        atoms._NET_WM_WINDOW_TYPE_SPLASH,
        atoms._NET_WM_WINDOW_TYPE_TOOLBAR,
        atoms._NET_WM_WINDOW_TYPE_UTILITY,
        atoms._NET_WM_WINDOW_TYPE_MENU,
        atoms._NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
        atoms._NET_WM_WINDOW_TYPE_POPUP_MENU,
        atoms._NET_WM_WINDOW_TYPE_TOOLTIP,
        atoms._NET_WM_STRUT,
        atoms._NET_WM_STRUT_PARTIAL,
        atoms._NET_NUMBER_OF_DESKTOPS,
        atoms._NET_CURRENT_DESKTOP,
        atoms._NET_WM_DESKTOP,
        atoms._NET_WORKAREA,
        atoms._NET_DESKTOP_NAMES,
        atoms._NET_DESKTOP_VIEWPORT,
        atoms._NET_WM_ICON,
        atoms._NET_CLOSE_WINDOW,
        atoms._NET_WM_PID,
        atoms._NET_DESKTOP_GEOMETRY,
        atoms._NET_FRAME_EXTENTS,
        atoms._NET_WM_ALLOWED_ACTIONS,
        atoms._NET_WM_ACTION_MOVE,
        atoms._NET_WM_ACTION_RESIZE,
        atoms._NET_WM_ACTION_MINIMIZE,
        atoms._NET_WM_ACTION_STICK,
        atoms._NET_WM_ACTION_MAXIMIZE_HORZ,
        atoms._NET_WM_ACTION_MAXIMIZE_VERT,
        atoms._NET_WM_ACTION_FULLSCREEN,
        atoms._NET_WM_ACTION_CHANGE_DESKTOP,
        atoms._NET_WM_ACTION_CLOSE,
        atoms._NET_WM_ACTION_ABOVE,
        atoms._NET_WM_ACTION_BELOW,
    };

    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_SUPPORTED, XCB_ATOM_ATOM, 32,
                        (uint32_t)(sizeof(supported_atoms) / sizeof(supported_atoms[0])), supported_atoms);

    // Create supporting WM check window
    s->supporting_wm_check = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[] = {1, XCB_EVENT_MASK_PROPERTY_CHANGE};

    xcb_create_window(conn, XCB_COPY_FROM_PARENT, s->supporting_wm_check, root, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, mask, values);

    // Acquire WM_S0 selection (EWMH)
    xcb_set_selection_owner(conn, s->supporting_wm_check, atoms.WM_S0, XCB_CURRENT_TIME);

    // Verify ownership (defensive)
    {
        xcb_get_selection_owner_cookie_t ck = xcb_get_selection_owner(conn, atoms.WM_S0);
        xcb_get_selection_owner_reply_t* rep = xcb_get_selection_owner_reply(conn, ck, NULL);
        if (!rep || rep->owner != s->supporting_wm_check) {
            LOG_ERROR("Failed to acquire WM_S0 selection");
            free(rep);
            return;
        }
        free(rep);
    }

    // Set _NET_SUPPORTING_WM_CHECK on root and on the window
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 32, 1,
                        &s->supporting_wm_check);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, s->supporting_wm_check, atoms._NET_SUPPORTING_WM_CHECK,
                        XCB_ATOM_WINDOW, 32, 1, &s->supporting_wm_check);

    // Set _NET_WM_NAME on supporting window (and optionally root)
    const char* wm_name = "bbox";
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, s->supporting_wm_check, atoms._NET_WM_NAME, atoms.UTF8_STRING, 8,
                        (uint32_t)strlen(wm_name), wm_name);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_WM_NAME, atoms.UTF8_STRING, 8,
                        (uint32_t)strlen(wm_name), wm_name);

    // Set _NET_WM_PID
    uint32_t pid = (uint32_t)getpid();
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, s->supporting_wm_check, atoms._NET_WM_PID, XCB_ATOM_CARDINAL, 32,
                        1, &pid);
    // Also set on root for completeness (some pagers check root)
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_WM_PID, XCB_ATOM_CARDINAL, 32, 1, &pid);

    // _NET_DESKTOP_GEOMETRY
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    uint32_t geometry[] = {screen->width_in_pixels, screen->height_in_pixels};
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_DESKTOP_GEOMETRY, XCB_ATOM_CARDINAL, 32, 2,
                        geometry);

    // Initialize workarea to full screen (no struts yet).
    s->workarea.x = 0;
    s->workarea.y = 0;
    s->workarea.w = (uint16_t)screen->width_in_pixels;
    s->workarea.h = (uint16_t)screen->height_in_pixels;

    wm_publish_desktop_props(s);

    rect_t wa;
    wm_compute_workarea(s, &wa);
    wm_publish_workarea(s, &wa);

    // Initialize root lists and focus to sane empty values.
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_CLIENT_LIST, XCB_ATOM_WINDOW, 32, 0, NULL);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_CLIENT_LIST_STACKING, XCB_ATOM_WINDOW, 32, 0,
                        NULL);
    xcb_delete_property(conn, root, atoms._NET_ACTIVE_WINDOW);

    xcb_flush(conn);
    LOG_INFO("Became WM on root %u, supporting %u", root, s->supporting_wm_check);
}

void wm_adopt_children(server_t* s) {
    LOG_INFO("Adopting existing windows...");
    xcb_query_tree_cookie_t cookie = xcb_query_tree(s->conn, s->root);
    xcb_query_tree_reply_t* reply = xcb_query_tree_reply(s->conn, cookie, NULL);
    if (!reply) return;

    xcb_window_t* children = xcb_query_tree_children(reply);
    int len = xcb_query_tree_children_length(reply);

    for (int i = 0; i < len; i++) {
        xcb_window_t win = children[i];
        if (win == s->supporting_wm_check) continue;

        // Defer decision: use async attributes check via cookie jar and adopt in reply handler
        xcb_get_window_attributes_cookie_t ck = xcb_get_window_attributes(s->conn, win);
        cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_GET_WINDOW_ATTRIBUTES, HANDLE_INVALID, (uint64_t)win,
                        wm_handle_reply);
    }

    free(reply);
}

void wm_handle_map_request(server_t* s, xcb_map_request_event_t* ev) { client_manage_start(s, ev->window); }

void wm_handle_unmap_notify(server_t* s, xcb_unmap_notify_event_t* ev) {
    handle_t h = server_get_client_by_window(s, ev->window);
    if (h == HANDLE_INVALID) return;

    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    if (hot->ignore_unmap > 0) {
        hot->ignore_unmap--;
        return;
    }

    // Process if reported on the window itself, its frame parent, or the root.
    // Applications withdrawing will trigger this.
    if (ev->event != hot->xid && ev->event != hot->frame && ev->event != s->root) return;

    client_unmanage(s, h);
}

void wm_handle_destroy_notify(server_t* s, xcb_destroy_notify_event_t* ev) {
    handle_t h = server_get_client_by_window(s, ev->window);
    if (h == HANDLE_INVALID) return;

    client_hot_t* hot = server_chot(s, h);
    if (hot) hot->state = STATE_DESTROYED;
    client_unmanage(s, h);
}

void wm_handle_configure_request(server_t* s, handle_t h, pending_config_t* ev) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    if (ev->mask & XCB_CONFIG_WINDOW_X) hot->desired.x = ev->x;
    if (ev->mask & XCB_CONFIG_WINDOW_Y) hot->desired.y = ev->y;
    if (ev->mask & XCB_CONFIG_WINDOW_WIDTH) hot->desired.w = ev->width;
    if (ev->mask & XCB_CONFIG_WINDOW_HEIGHT) hot->desired.h = ev->height;

    client_constrain_size(&hot->hints, hot->hints_flags, &hot->desired.w, &hot->desired.h);
    hot->dirty |= DIRTY_GEOM;

    LOG_DEBUG("Client %lx desired geom updated: %d,%d %dx%d (mask %x)", h, hot->desired.x, hot->desired.y,
              hot->desired.w, hot->desired.h, ev->mask);
}

void wm_handle_configure_notify(server_t* s, handle_t h, xcb_configure_notify_event_t* ev) {
    (void)s;
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    if (ev->window == hot->frame) {
        hot->server.x = ev->x;
        hot->server.y = ev->y;
        LOG_DEBUG("Client %lx frame pos updated: %d,%d", h, ev->x, ev->y);
    } else if (ev->window == hot->xid) {
        hot->server.w = ev->width;
        hot->server.h = ev->height;
        LOG_DEBUG("Client %lx window size updated: %dx%d", h, ev->width, ev->height);
    }
}

void wm_handle_property_notify(server_t* s, handle_t h, xcb_property_notify_event_t* ev) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    if (ev->atom == atoms.WM_NAME || ev->atom == atoms._NET_WM_NAME) {
        hot->dirty |= DIRTY_TITLE;
    } else if (ev->atom == atoms.WM_HINTS) {
        hot->dirty |= DIRTY_HINTS | DIRTY_STATE;
    } else if (ev->atom == atoms.WM_NORMAL_HINTS) {
        hot->dirty |= DIRTY_HINTS | DIRTY_STATE;
    } else if (ev->atom == atoms._NET_WM_STRUT || ev->atom == atoms._NET_WM_STRUT_PARTIAL) {
        hot->dirty |= DIRTY_STRUT;
        s->root_dirty |= ROOT_DIRTY_WORKAREA;
    } else if (ev->atom == atoms._MOTIF_WM_HINTS) {
        hot->dirty |= DIRTY_HINTS | DIRTY_FRAME_STYLE;
    }
}

void wm_client_apply_state_set(server_t* s, handle_t h, const client_state_set_t* set) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot || !set) return;

    if (hot->maximized_horz != set->max_horz || hot->maximized_vert != set->max_vert) {
        wm_client_set_maximize(s, hot, set->max_horz, set->max_vert);
    }

    if (set->above) {
        if (!hot->state_above) wm_client_update_state(s, h, 1, atoms._NET_WM_STATE_ABOVE);
    } else if (hot->state_above) {
        wm_client_update_state(s, h, 0, atoms._NET_WM_STATE_ABOVE);
    }

    if (set->below && !set->above) {
        if (!hot->state_below) wm_client_update_state(s, h, 1, atoms._NET_WM_STATE_BELOW);
    } else if (hot->state_below) {
        wm_client_update_state(s, h, 0, atoms._NET_WM_STATE_BELOW);
    }

    if (set->urgent) {
        if (!(hot->flags & CLIENT_FLAG_URGENT)) wm_client_update_state(s, h, 1, atoms._NET_WM_STATE_DEMANDS_ATTENTION);
    } else if (hot->flags & CLIENT_FLAG_URGENT) {
        wm_client_update_state(s, h, 0, atoms._NET_WM_STATE_DEMANDS_ATTENTION);
    }

    if (set->fullscreen) {
        if (hot->layer != LAYER_FULLSCREEN) wm_client_update_state(s, h, 1, atoms._NET_WM_STATE_FULLSCREEN);
    } else if (hot->layer == LAYER_FULLSCREEN) {
        wm_client_update_state(s, h, 0, atoms._NET_WM_STATE_FULLSCREEN);
    }

    if (set->sticky != hot->sticky) {
        wm_client_toggle_sticky(s, h);
    }
}

// Dirty flush

static bool wm_client_is_hidden(const server_t* s, const client_hot_t* hot) {
    if (hot->state != STATE_MAPPED) return true;
    if (!hot->sticky && hot->desktop != (int32_t)s->current_desktop) return true;
    return false;
}

void wm_send_synthetic_configure(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
    uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;

    char buffer[32];
    memset(buffer, 0, 32);
    xcb_configure_notify_event_t* ev = (xcb_configure_notify_event_t*)buffer;

    ev->response_type = XCB_CONFIGURE_NOTIFY;
    ev->event = hot->xid;
    ev->window = hot->xid;
    ev->above_sibling = XCB_NONE;
    ev->x = hot->server.x + (int16_t)bw;
    ev->y = hot->server.y + (int16_t)th;
    ev->width = hot->server.w;
    ev->height = hot->server.h;
    ev->border_width = 0;
    ev->override_redirect = hot->override_redirect;

    xcb_send_event(s->conn, 0, hot->xid, XCB_EVENT_MASK_STRUCTURE_NOTIFY, buffer);
}

void wm_flush_dirty(server_t* s) {
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (!s->clients.hdr[i].live) continue;

        handle_t h = handle_make(i, s->clients.hdr[i].gen);
        client_hot_t* hot = server_chot(s, h);
        if (!hot) continue;

        if (hot->dirty == DIRTY_NONE) continue;
        if (hot->state == STATE_UNMANAGING || hot->state == STATE_DESTROYED) continue;

        if (hot->dirty & DIRTY_GEOM) {
            uint16_t bw = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.border_width;
            uint16_t th = (hot->flags & CLIENT_FLAG_UNDECORATED) ? 0 : s->config.theme.title_height;

            uint32_t frame_values[4];
            frame_values[0] = (uint32_t)hot->desired.x;
            frame_values[1] = (uint32_t)hot->desired.y;
            frame_values[2] = (uint32_t)(hot->desired.w + 2 * bw);
            frame_values[3] = (uint32_t)(hot->desired.h + th + bw);

            xcb_configure_window(
                s->conn, hot->frame,
                XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                frame_values);

            uint32_t client_values[4];
            client_values[0] = (uint32_t)bw;
            client_values[1] = (uint32_t)th;
            client_values[2] = (uint32_t)hot->desired.w;
            client_values[3] = (uint32_t)hot->desired.h;

            xcb_configure_window(
                s->conn, hot->xid,
                XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                client_values);

            // Set _NET_FRAME_EXTENTS
            uint32_t extents[4] = {bw, bw, th + bw, bw};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_FRAME_EXTENTS, XCB_ATOM_CARDINAL,
                                32, 4, extents);

            // Update server state immediately to ensure redraw uses correct geometry
            hot->server = hot->desired;

            wm_send_synthetic_configure(s, h);

            frame_redraw(s, h, FRAME_REDRAW_ALL);

            hot->pending = hot->desired;
            hot->pending_epoch++;

            hot->dirty &= ~DIRTY_GEOM;

            LOG_DEBUG("Flushed DIRTY_GEOM for %lx: %d,%d %dx%d", h, hot->pending.x, hot->pending.y, hot->pending.w,
                      hot->pending.h);
        }

        if (hot->dirty & DIRTY_TITLE) {
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_NAME, atoms.UTF8_STRING, 0, 1024).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_NAME,
                            wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_NAME, XCB_ATOM_STRING, 0, 1024).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms.WM_NAME,
                            wm_handle_reply);

            hot->dirty &= ~DIRTY_TITLE;
        }

        if (hot->dirty & DIRTY_HINTS) {
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms.WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 32).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms.WM_NORMAL_HINTS, wm_handle_reply);
            c = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_HINTS, atoms.WM_HINTS, 0, 32).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms.WM_HINTS,
                            wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms._MOTIF_WM_HINTS, atoms._MOTIF_WM_HINTS, 0, 5).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._MOTIF_WM_HINTS, wm_handle_reply);

            hot->dirty &= ~DIRTY_HINTS;
        }

        if (hot->dirty & DIRTY_STRUT) {
            uint32_t c =
                xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 0, 12).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h,
                            ((uint64_t)hot->xid << 32) | atoms._NET_WM_STRUT_PARTIAL, wm_handle_reply);

            c = xcb_get_property(s->conn, 0, hot->xid, atoms._NET_WM_STRUT, XCB_ATOM_CARDINAL, 0, 4).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms._NET_WM_STRUT,
                            wm_handle_reply);

            hot->dirty &= ~DIRTY_STRUT;
        }

        if (hot->dirty & DIRTY_FRAME_STYLE) {
            frame_redraw(s, h, FRAME_REDRAW_ALL);
            hot->dirty &= ~DIRTY_FRAME_STYLE;
        }

        if (hot->dirty & DIRTY_STACK) {
            stack_move_to_layer(s, h);
            hot->dirty &= ~DIRTY_STACK;
        }

        if (hot->dirty & DIRTY_STATE) {
            xcb_atom_t state_atoms[12];
            uint32_t count = 0;

            if (hot->layer == LAYER_FULLSCREEN) {
                state_atoms[count++] = atoms._NET_WM_STATE_FULLSCREEN;
            }
            if (hot->state_above) {
                state_atoms[count++] = atoms._NET_WM_STATE_ABOVE;
            }
            if (hot->state_below) {
                state_atoms[count++] = atoms._NET_WM_STATE_BELOW;
            }

            if (hot->flags & CLIENT_FLAG_URGENT) state_atoms[count++] = atoms._NET_WM_STATE_DEMANDS_ATTENTION;
            if (hot->sticky) state_atoms[count++] = atoms._NET_WM_STATE_STICKY;
            if (hot->maximized_horz) state_atoms[count++] = atoms._NET_WM_STATE_MAXIMIZED_HORZ;
            if (hot->maximized_vert) state_atoms[count++] = atoms._NET_WM_STATE_MAXIMIZED_VERT;
            if (wm_client_is_hidden(s, hot)) state_atoms[count++] = atoms._NET_WM_STATE_HIDDEN;
            if (hot->flags & CLIENT_FLAG_FOCUSED) state_atoms[count++] = atoms._NET_WM_STATE_FOCUSED;

            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_STATE, XCB_ATOM_ATOM, 32, count,
                                state_atoms);

            // Set _NET_WM_ALLOWED_ACTIONS
            xcb_atom_t actions[16];
            uint32_t num_actions = 0;
            actions[num_actions++] = atoms._NET_WM_ACTION_MOVE;
            actions[num_actions++] = atoms._NET_WM_ACTION_MINIMIZE;
            actions[num_actions++] = atoms._NET_WM_ACTION_STICK;
            actions[num_actions++] = atoms._NET_WM_ACTION_CHANGE_DESKTOP;
            actions[num_actions++] = atoms._NET_WM_ACTION_CLOSE;
            actions[num_actions++] = atoms._NET_WM_ACTION_ABOVE;
            actions[num_actions++] = atoms._NET_WM_ACTION_BELOW;

            bool fixed = (hot->hints.max_w > 0 && hot->hints.min_w == hot->hints.max_w && hot->hints.max_h > 0 &&
                          hot->hints.min_h == hot->hints.max_h);

            if (!fixed) {
                actions[num_actions++] = atoms._NET_WM_ACTION_RESIZE;
                actions[num_actions++] = atoms._NET_WM_ACTION_MAXIMIZE_HORZ;
                actions[num_actions++] = atoms._NET_WM_ACTION_MAXIMIZE_VERT;
                actions[num_actions++] = atoms._NET_WM_ACTION_FULLSCREEN;
            }

            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_ALLOWED_ACTIONS, XCB_ATOM_ATOM,
                                32, num_actions, actions);

            hot->dirty &= ~DIRTY_STATE;
        }
    }

    // Root properties

    if (s->root_dirty & ROOT_DIRTY_ACTIVE_WINDOW) {
        if (s->focused_client != HANDLE_INVALID) {
            client_hot_t* c = server_chot(s, s->focused_client);
            if (c) {
                xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW,
                                    32, 1, &c->xid);
            }
        } else {
            xcb_delete_property(s->conn, s->root, atoms._NET_ACTIVE_WINDOW);
        }
        s->root_dirty &= ~ROOT_DIRTY_ACTIVE_WINDOW;
    }

    if (s->root_dirty & (ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING)) {
        uint32_t count = 0;
        for (uint32_t i = 1; i < s->clients.cap; i++) {
            if (s->clients.hdr[i].live) count++;
        }

        xcb_window_t* wins = count ? malloc(count * sizeof(xcb_window_t)) : NULL;
        if (wins || count == 0) {
            if (s->root_dirty & ROOT_DIRTY_CLIENT_LIST) {
                if (count) {
                    uint32_t idx = 0;
                    for (uint32_t i = 1; i < s->clients.cap; i++) {
                        if (!s->clients.hdr[i].live) continue;
                        handle_t hh = handle_make(i, s->clients.hdr[i].gen);
                        client_hot_t* hot = server_chot(s, hh);
                        if (!hot) continue;
                        wins[idx++] = hot->xid;
                    }
                    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_CLIENT_LIST,
                                        XCB_ATOM_WINDOW, 32, idx, wins);
                } else {
                    xcb_delete_property(s->conn, s->root, atoms._NET_CLIENT_LIST);
                }
            }

            if (s->root_dirty & ROOT_DIRTY_CLIENT_LIST_STACKING) {
                if (count) {
                    uint32_t idx = 0;
                    for (int l = 0; l < LAYER_COUNT; l++) {
                        list_node_t* head = &s->layers[l];
                        for (list_node_t* node = head->next; node != head; node = node->next) {
                            client_hot_t* hot = (client_hot_t*)((char*)node - offsetof(client_hot_t, stacking_node));
                            wins[idx++] = hot->xid;
                        }
                    }
                    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_CLIENT_LIST_STACKING,
                                        XCB_ATOM_WINDOW, 32, idx, wins);
                } else {
                    xcb_delete_property(s->conn, s->root, atoms._NET_CLIENT_LIST_STACKING);
                }
            }

            free(wins);
        }

        s->root_dirty &= ~(ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING);
    }

    if (s->root_dirty & ROOT_DIRTY_WORKAREA) {
        rect_t wa;
        wm_compute_workarea(s, &wa);
        wm_publish_workarea(s, &wa);

        s->root_dirty &= ~ROOT_DIRTY_WORKAREA;
    }

    xcb_flush(s->conn);
}

// Focus & keys

void wm_cycle_focus(server_t* s, bool forward) {
    if (list_empty(&s->focus_history)) return;

    list_node_t* start_node = &s->focus_history;
    if (s->focused_client != HANDLE_INVALID) {
        client_hot_t* focused = server_chot(s, s->focused_client);
        if (focused) start_node = &focused->focus_node;
    }

    list_node_t* node = forward ? start_node->next : start_node->prev;
    while (node != start_node) {
        if (node == &s->focus_history) {
            node = forward ? node->next : node->prev;
            if (node == start_node) break;
        }

        client_hot_t* c = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));

        if (c->state == STATE_MAPPED && (c->desktop == (int32_t)s->current_desktop || c->sticky) &&
            c->type != WINDOW_TYPE_DOCK && c->type != WINDOW_TYPE_NOTIFICATION && c->type != WINDOW_TYPE_DESKTOP &&
            c->type != WINDOW_TYPE_MENU && c->type != WINDOW_TYPE_DROPDOWN_MENU && c->type != WINDOW_TYPE_POPUP_MENU &&
            c->type != WINDOW_TYPE_TOOLTIP) {
            wm_set_focus(s, c->self);
            stack_raise(s, c->self);
            return;
        }

        node = forward ? node->next : node->prev;
    }
}

void wm_setup_keys(server_t* s) {
    if (s->keysyms) xcb_key_symbols_free(s->keysyms);
    s->keysyms = xcb_key_symbols_alloc(s->conn);

    xcb_ungrab_key(s->conn, XCB_GRAB_ANY, s->root, XCB_MOD_MASK_ANY);

    // Grab keys from config
    for (size_t i = 0; i < s->config.key_bindings.length; i++) {
        key_binding_t* b = s->config.key_bindings.items[i];
        xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(s->keysyms, b->keysym);
        if (!keycodes) continue;

        for (xcb_keycode_t* k = keycodes; *k; k++) {
            xcb_grab_key(s->conn, 1, s->root, b->modifiers, *k, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        }

        free(keycodes);
    }

    xcb_flush(s->conn);
}

void wm_handle_key_press(server_t* s, xcb_key_press_event_t* ev) {
    if (!s->keysyms) return;

    xcb_keysym_t sym = xcb_key_symbols_get_keysym(s->keysyms, ev->detail, 0);

    if (s->menu.visible && sym == XK_Escape) {
        menu_hide(s);
        return;
    }

    uint32_t mods = clean_mods(ev->state);

    LOG_DEBUG("Key press: detail=%u state=%u clean=%u sym=%x", ev->detail, ev->state, mods, sym);

    for (size_t i = 0; i < s->config.key_bindings.length; i++) {
        key_binding_t* b = s->config.key_bindings.items[i];
        if (!b) continue;

        if (b->keysym != sym || b->modifiers != mods) continue;

        LOG_INFO("Matched key binding action %d", b->action);

        switch (b->action) {
            case ACTION_CLOSE:
                if (s->focused_client != HANDLE_INVALID) client_close(s, s->focused_client);
                break;
            case ACTION_FOCUS_NEXT:
                wm_cycle_focus(s, true);
                break;
            case ACTION_FOCUS_PREV:
                wm_cycle_focus(s, false);
                break;
            case ACTION_TERMINAL:
                spawn("st || xterm || x-terminal-emulator");
                break;
            case ACTION_EXEC:
                if (b->exec_cmd) spawn(b->exec_cmd);
                break;
            case ACTION_RESTART: {
                LOG_INFO("Restarting bbox...");
                char path[1024];
                ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
                if (len != -1) {
                    path[len] = '\0';
                    char* args[] = {path, NULL};
                    execv(path, args);
                }
                LOG_ERROR("Failed to restart: %s", strerror(errno));
                break;
            }
            case ACTION_EXIT:
                exit(0);
                break;
            case ACTION_WORKSPACE:
                if (b->exec_cmd) wm_switch_workspace(s, (uint32_t)atoi(b->exec_cmd));
                break;
            case ACTION_WORKSPACE_PREV:
                wm_switch_workspace_relative(s, -1);
                break;
            case ACTION_WORKSPACE_NEXT:
                wm_switch_workspace_relative(s, 1);
                break;
            case ACTION_MOVE_TO_WORKSPACE:
                if (b->exec_cmd && s->focused_client != HANDLE_INVALID) {
                    wm_client_move_to_workspace(s, s->focused_client, (uint32_t)atoi(b->exec_cmd), false);
                }
                break;
            case ACTION_MOVE_TO_WORKSPACE_FOLLOW:
                if (b->exec_cmd && s->focused_client != HANDLE_INVALID) {
                    wm_client_move_to_workspace(s, s->focused_client, (uint32_t)atoi(b->exec_cmd), true);
                }
                break;
            case ACTION_TOGGLE_STICKY:
                if (s->focused_client != HANDLE_INVALID) wm_client_toggle_sticky(s, s->focused_client);
                break;
            default:
                break;
        }
        return;
    }
}

// Helpers

static int wm_get_resize_dir(server_t* s, client_hot_t* hot, int16_t x, int16_t y) {
    if (hot->flags & CLIENT_FLAG_UNDECORATED) return RESIZE_NONE;

    uint16_t bw = s->config.theme.border_width;
    uint16_t th = s->config.theme.title_height;

    uint16_t frame_w = hot->server.w + 2 * bw;
    uint16_t frame_h = hot->server.h + th + bw;

    int dir = RESIZE_NONE;
    if (x < bw) dir |= RESIZE_LEFT;
    if (x >= frame_w - bw) dir |= RESIZE_RIGHT;
    if (y < bw) dir |= RESIZE_TOP;  // Top border (part of titlebar area technically)
    if (y >= frame_h - bw) dir |= RESIZE_BOTTOM;

    return dir;
}

static void wm_update_cursor(server_t* s, xcb_window_t win, int dir) {
    xcb_cursor_t c = s->cursor_left_ptr;
    if (dir == RESIZE_TOP)
        c = s->cursor_resize_top;
    else if (dir == RESIZE_BOTTOM)
        c = s->cursor_resize_bottom;
    else if (dir == RESIZE_LEFT)
        c = s->cursor_resize_left;
    else if (dir == RESIZE_RIGHT)
        c = s->cursor_resize_right;
    else if (dir == (RESIZE_TOP | RESIZE_LEFT))
        c = s->cursor_resize_top_left;
    else if (dir == (RESIZE_TOP | RESIZE_RIGHT))
        c = s->cursor_resize_top_right;
    else if (dir == (RESIZE_BOTTOM | RESIZE_LEFT))
        c = s->cursor_resize_bottom_left;
    else if (dir == (RESIZE_BOTTOM | RESIZE_RIGHT))
        c = s->cursor_resize_bottom_right;

    xcb_change_window_attributes(s->conn, win, XCB_CW_CURSOR, &c);
}

// Mouse interaction

void wm_handle_button_press(server_t* s, xcb_button_press_event_t* ev) {
    if (s->menu.visible) {
        menu_handle_button_press(s, ev);
        return;
    }

    // Root menu and workspace scroll on empty root clicks
    if (ev->event == s->root && ev->child == XCB_NONE) {
        if (ev->detail == 2) {
            menu_show_client_list(s, ev->root_x, ev->root_y);
            return;
        }
        if (ev->detail == 3) {
            menu_show(s, ev->root_x, ev->root_y);
            return;
        }
        if (ev->detail == 4) {
            wm_switch_workspace_relative(s, -1);
            return;
        }
        if (ev->detail == 5) {
            wm_switch_workspace_relative(s, 1);
            return;
        }
    }

    // Identify target client
    bool is_frame = false;
    handle_t h = server_get_client_by_frame(s, ev->event);
    if (h != HANDLE_INVALID) {
        is_frame = true;
    } else {
        h = server_get_client_by_window(s, ev->event);
    }
    if (h == HANDLE_INVALID) return;

    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    // Focus + optional raise on click
    if (s->focused_client != h && hot->type != WINDOW_TYPE_DOCK && hot->type != WINDOW_TYPE_DESKTOP &&
        hot->type != WINDOW_TYPE_NOTIFICATION && hot->type != WINDOW_TYPE_MENU &&
        hot->type != WINDOW_TYPE_DROPDOWN_MENU && hot->type != WINDOW_TYPE_POPUP_MENU &&
        hot->type != WINDOW_TYPE_TOOLTIP) {
        wm_set_focus(s, h);
        if (should_raise_on_click(hot, ev->detail)) stack_raise(s, h);
    }

    uint32_t mods = clean_mods(ev->state);

    bool start_move = false;
    bool start_resize = false;
    int resize_dir = RESIZE_NONE;

    // Alt+Button1 move, Alt+Button3 resize
    if (mods == XCB_MOD_MASK_1) {
        if (ev->detail == 1) start_move = true;
        if (ev->detail == 3) {
            start_resize = true;
            resize_dir = RESIZE_BOTTOM | RESIZE_RIGHT;  // Default to BR for Alt-Resize
        }
    } else if (is_frame && ev->detail == 1) {
        // Click on frame: check for buttons first
        frame_button_t btn = frame_get_button_at(s, h, ev->event_x, ev->event_y);
        if (btn != FRAME_BUTTON_NONE) {
            if (btn == FRAME_BUTTON_CLOSE) {
                client_close(s, h);
            } else if (btn == FRAME_BUTTON_MAXIMIZE) {
                wm_client_toggle_maximize(s, h);
            } else if (btn == FRAME_BUTTON_MINIMIZE) {
                wm_client_iconify(s, h);
            }
            xcb_allow_events(s->conn, XCB_ALLOW_ASYNC_POINTER, ev->time);
            return;
        }

        // Check for border resize
        // Use event_x/y which are relative to the event window (the frame)
        resize_dir = wm_get_resize_dir(s, hot, ev->event_x, ev->event_y);

        if (resize_dir != RESIZE_NONE) {
            start_resize = true;
        } else {
            // Click-drag on frame (titlebar/content) acts like move
            start_move = true;
        }
    }

    if (!start_move && !start_resize) {
        xcb_allow_events(s->conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
        return;
    }

    s->interaction_mode = start_move ? INTERACTION_MOVE : INTERACTION_RESIZE;
    s->interaction_resize_dir = resize_dir;
    s->interaction_window = hot->frame;

    s->interaction_start_x = hot->server.x;
    s->interaction_start_y = hot->server.y;
    s->interaction_start_w = hot->server.w;
    s->interaction_start_h = hot->server.h;

    s->interaction_pointer_x = ev->root_x;
    s->interaction_pointer_y = ev->root_y;

    xcb_cursor_t cursor = XCB_NONE;
    if (start_move) {
        cursor = s->cursor_move;
    } else if (start_resize) {
        if (resize_dir == RESIZE_TOP)
            cursor = s->cursor_resize_top;
        else if (resize_dir == RESIZE_BOTTOM)
            cursor = s->cursor_resize_bottom;
        else if (resize_dir == RESIZE_LEFT)
            cursor = s->cursor_resize_left;
        else if (resize_dir == RESIZE_RIGHT)
            cursor = s->cursor_resize_right;
        else if (resize_dir == (RESIZE_TOP | RESIZE_LEFT))
            cursor = s->cursor_resize_top_left;
        else if (resize_dir == (RESIZE_TOP | RESIZE_RIGHT))
            cursor = s->cursor_resize_top_right;
        else if (resize_dir == (RESIZE_BOTTOM | RESIZE_LEFT))
            cursor = s->cursor_resize_bottom_left;
        else if (resize_dir == (RESIZE_BOTTOM | RESIZE_RIGHT))
            cursor = s->cursor_resize_bottom_right;
    }

    xcb_grab_pointer(s->conn, 0, s->root,
                     XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, cursor, XCB_CURRENT_TIME);

    LOG_INFO("Started interactive %s for client %lx (dir=%d)", start_move ? "MOVE" : "RESIZE", h, resize_dir);

    xcb_allow_events(s->conn, XCB_ALLOW_ASYNC_POINTER, ev->time);
}

void wm_handle_button_release(server_t* s, xcb_button_release_event_t* ev) {
    if (s->menu.visible) {
        menu_handle_button_release(s, ev);
        return;
    }

    (void)ev;

    if (s->interaction_mode == INTERACTION_NONE) return;

    s->interaction_mode = INTERACTION_NONE;
    s->interaction_resize_dir = RESIZE_NONE;
    xcb_ungrab_pointer(s->conn, XCB_CURRENT_TIME);
    LOG_INFO("Ended interaction");
}

void wm_handle_motion_notify(server_t* s, xcb_motion_notify_event_t* ev) {
    if (s->menu.visible) {
        menu_handle_pointer_motion(s, ev->root_x, ev->root_y);
        return;
    }

    if (s->interaction_mode == INTERACTION_NONE) {
        // Handle cursor updates on frame hover
        handle_t h = server_get_client_by_frame(s, ev->event);
        if (h != HANDLE_INVALID) {
            client_hot_t* hot = server_chot(s, h);
            if (hot) {
                int dir = wm_get_resize_dir(s, hot, ev->event_x, ev->event_y);
                // Optimization: only update if changed
                // Initialize last_cursor_dir to -1 or something in client alloc?
                // client_manage_start zeroes the struct (via calloc in slotmap or explicit init?)
                // slotmap_alloc does NOT zero. client_manage_start does:
                // hot->state = STATE_NEW; ...
                // I need to init last_cursor_dir in client_manage_start or handle it here.
                // Let's assume initialized to 0 (RESIZE_NONE) is fine, but we might miss first update if it starts at
                // 0. Better to use a separate logic or just force it once? Actually, RESIZE_NONE is 0. If we start at
                // 0, and we are at 0, we do nothing. But we want to set it to LeftPtr initially. Let's rely on frame
                // creation setting cursor to None (inherit) or LeftPtr? Frame created with mask, but no cursor
                // attribute set? X default is "None" (inherit). So initial 0 (RESIZE_NONE) -> updates to LeftPtr. If
                // hot->last_cursor_dir matches, we skip. We need to ensure we initialize it to something invalid, e.g.
                // -1.

                if (hot->last_cursor_dir != dir) {
                    wm_update_cursor(s, hot->frame, dir);
                    hot->last_cursor_dir = dir;
                }
            }
        }
        return;
    }

    handle_t h = server_get_client_by_frame(s, s->interaction_window);
    client_hot_t* hot = server_chot(s, h);
    if (!hot) {
        s->interaction_mode = INTERACTION_NONE;
        xcb_ungrab_pointer(s->conn, XCB_CURRENT_TIME);
        return;
    }

    int dx = ev->root_x - s->interaction_pointer_x;
    int dy = ev->root_y - s->interaction_pointer_y;

    if (s->interaction_mode == INTERACTION_MOVE) {
        hot->desired.x = (int16_t)(s->interaction_start_x + dx);
        hot->desired.y = (int16_t)(s->interaction_start_y + dy);
        hot->dirty |= DIRTY_GEOM;
        return;
    }

    // Resize logic
    int new_w = s->interaction_start_w;
    int new_h = s->interaction_start_h;

    if (s->interaction_resize_dir & RESIZE_RIGHT) {
        new_w += dx;
    } else if (s->interaction_resize_dir & RESIZE_LEFT) {
        new_w -= dx;
    }

    if (s->interaction_resize_dir & RESIZE_BOTTOM) {
        new_h += dy;
    } else if (s->interaction_resize_dir & RESIZE_TOP) {
        new_h -= dy;
    }

    // Constrain size
    uint16_t w = (uint16_t)(new_w > 50 ? new_w : 50);
    uint16_t h_val = (uint16_t)(new_h > 50 ? new_h : 50);

    // Apply hints
    client_constrain_size(&hot->hints, hot->hints_flags, &w, &h_val);
    // Adjust position if resizing top/left
    // Determine effective delta
    int dw = (int)w - s->interaction_start_w;
    int dh = (int)h_val - s->interaction_start_h;

    if (s->interaction_resize_dir & RESIZE_LEFT) {
        hot->desired.x = (int16_t)(s->interaction_start_x - dw);
    } else {
        hot->desired.x = (int16_t)s->interaction_start_x;
    }

    if (s->interaction_resize_dir & RESIZE_TOP) {
        hot->desired.y = (int16_t)(s->interaction_start_y - dh);
    } else {
        hot->desired.y = (int16_t)s->interaction_start_y;
    }

    hot->desired.w = w;
    hot->desired.h = h_val;

    hot->dirty |= DIRTY_GEOM;
}

// EWMH client messages / state

void wm_client_update_state(server_t* s, handle_t h, uint32_t action, xcb_atom_t prop) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    if (prop == atoms._NET_WM_STATE_FOCUSED) return;

    if (prop == atoms._NET_WM_STATE_HIDDEN) {
        if (action == 2) {
            if (hot->state == STATE_MAPPED)
                wm_client_iconify(s, h);
            else
                wm_client_restore(s, h);
        } else if (action == 1) {
            wm_client_iconify(s, h);
        } else if (action == 0) {
            wm_client_restore(s, h);
        }
        return;
    }

    bool add = false;

    if (action == 1) {
        add = true;
    } else if (action == 0) {
        add = false;
    } else if (action == 2) {
        if (prop == atoms._NET_WM_STATE_FULLSCREEN)
            add = (hot->layer != LAYER_FULLSCREEN);
        else if (prop == atoms._NET_WM_STATE_ABOVE)
            add = !hot->state_above;
        else if (prop == atoms._NET_WM_STATE_BELOW)
            add = !hot->state_below;
        else if (prop == atoms._NET_WM_STATE_STICKY)
            add = !hot->sticky;
        else if (prop == atoms._NET_WM_STATE_DEMANDS_ATTENTION)
            add = !(hot->flags & CLIENT_FLAG_URGENT);
        else if (prop == atoms._NET_WM_STATE_MAXIMIZED_HORZ)
            add = !hot->maximized_horz;
        else if (prop == atoms._NET_WM_STATE_MAXIMIZED_VERT)
            add = !hot->maximized_vert;
        else
            return;
    } else {
        return;
    }

    if (prop == atoms._NET_WM_STATE_FULLSCREEN) {
        if (add && hot->layer != LAYER_FULLSCREEN) {
            hot->saved_geom = hot->server;
            hot->saved_layer = hot->layer;
            hot->saved_state_mask = hot->flags & CLIENT_FLAG_UNDECORATED;
            hot->saved_maximized_horz = hot->maximized_horz;
            hot->saved_maximized_vert = hot->maximized_vert;
            hot->maximized_horz = false;
            hot->maximized_vert = false;
            hot->layer = LAYER_FULLSCREEN;
            hot->flags |= CLIENT_FLAG_UNDECORATED;

            if (s->config.fullscreen_use_workarea) {
                hot->desired = s->workarea;
            } else {
                xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
                hot->desired.x = 0;
                hot->desired.y = 0;
                hot->desired.w = (uint16_t)screen->width_in_pixels;
                hot->desired.h = (uint16_t)screen->height_in_pixels;
            }

            hot->dirty |= DIRTY_GEOM | DIRTY_STATE | DIRTY_STACK;
        } else if (!add && hot->layer == LAYER_FULLSCREEN) {
            hot->layer = client_layer_from_state(hot);
            hot->desired = hot->saved_geom;
            hot->flags = (hot->flags & ~CLIENT_FLAG_UNDECORATED) | (hot->saved_state_mask & CLIENT_FLAG_UNDECORATED);
            hot->maximized_horz = hot->saved_maximized_horz;
            hot->maximized_vert = hot->saved_maximized_vert;
            hot->dirty |= DIRTY_GEOM | DIRTY_STATE | DIRTY_STACK;
        }
        return;
    }

    if (prop == atoms._NET_WM_STATE_ABOVE) {
        if (add) {
            hot->state_above = true;
            hot->state_below = false;
        } else {
            hot->state_above = false;
        }
        if (hot->layer != LAYER_FULLSCREEN) {
            uint8_t desired = client_layer_from_state(hot);
            if (hot->layer != desired) {
                hot->layer = desired;
                hot->dirty |= DIRTY_STACK;
            }
        }
        hot->dirty |= DIRTY_STATE;
        return;
    }

    if (prop == atoms._NET_WM_STATE_BELOW) {
        if (add) {
            hot->state_below = true;
            hot->state_above = false;
        } else {
            hot->state_below = false;
        }
        if (hot->layer != LAYER_FULLSCREEN) {
            uint8_t desired = client_layer_from_state(hot);
            if (hot->layer != desired) {
                hot->layer = desired;
                hot->dirty |= DIRTY_STACK;
            }
        }
        hot->dirty |= DIRTY_STATE;
        return;
    }

    if (prop == atoms._NET_WM_STATE_STICKY) {
        if (hot->sticky != add) wm_client_toggle_sticky(s, h);
        return;
    }

    if (prop == atoms._NET_WM_STATE_DEMANDS_ATTENTION) {
        if (add)
            hot->flags |= CLIENT_FLAG_URGENT;
        else
            hot->flags &= ~CLIENT_FLAG_URGENT;
        hot->dirty |= DIRTY_STATE | DIRTY_FRAME_STYLE;
        return;
    }

    if (prop == atoms._NET_WM_STATE_MAXIMIZED_HORZ || prop == atoms._NET_WM_STATE_MAXIMIZED_VERT) {
        bool new_horz = hot->maximized_horz;
        bool new_vert = hot->maximized_vert;

        if (prop == atoms._NET_WM_STATE_MAXIMIZED_HORZ) new_horz = add;
        if (prop == atoms._NET_WM_STATE_MAXIMIZED_VERT) new_vert = add;

        wm_client_set_maximize(s, hot, new_horz, new_vert);
        return;
    }
}

void wm_handle_client_message(server_t* s, xcb_client_message_event_t* ev) {
    if (ev->type == atoms._NET_CURRENT_DESKTOP) {
        uint32_t desktop = ev->data.data32[0];
        if (desktop >= s->desktop_count) {
            LOG_INFO("Client requested switch to desktop %u (out of range)", desktop);
            return;
        }
        LOG_INFO("Client requested switch to desktop %u", desktop);
        wm_switch_workspace(s, desktop);
        return;
    }

    if (ev->type == atoms._NET_NUMBER_OF_DESKTOPS) {
        uint32_t requested = ev->data.data32[0];
        if (requested == 0) requested = 1;
        if (requested != s->desktop_count) {
            LOG_INFO("Client requested %u desktops", requested);
            s->desktop_count = requested;
            if (s->current_desktop >= s->desktop_count) s->current_desktop = 0;

            for (uint32_t i = 1; i < s->clients.cap; i++) {
                if (!s->clients.hdr[i].live) continue;
                handle_t h = handle_make(i, s->clients.hdr[i].gen);
                client_hot_t* hot = server_chot(s, h);
                if (!hot || hot->sticky) continue;
                if (hot->desktop >= (int32_t)s->desktop_count) {
                    hot->desktop = (int32_t)s->current_desktop;
                    uint32_t prop_val = (uint32_t)hot->desktop;
                    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms._NET_WM_DESKTOP,
                                        XCB_ATOM_CARDINAL, 32, 1, &prop_val);
                }
            }
        }
        wm_publish_desktop_props(s);
        s->root_dirty |= ROOT_DIRTY_WORKAREA;
        return;
    }

    if (ev->type == atoms._NET_WM_DESKTOP) {
        handle_t h = server_get_client_by_window(s, ev->window);
        if (h != HANDLE_INVALID) {
            uint32_t desktop = ev->data.data32[0];
            if (desktop != 0xFFFFFFFFu && desktop >= s->desktop_count) {
                LOG_INFO("Client requested move to desktop %u (out of range)", desktop);
                return;
            }
            wm_client_move_to_workspace(s, h, desktop, false);
        }
        return;
    }

    if (ev->type == atoms._NET_ACTIVE_WINDOW) {
        handle_t h = server_get_client_by_window(s, ev->window);
        if (h != HANDLE_INVALID) {
            client_hot_t* hot = server_chot(s, h);
            if (!hot) return;
            if (!hot->sticky && hot->desktop >= 0 && (uint32_t)hot->desktop != s->current_desktop) {
                wm_switch_workspace(s, (uint32_t)hot->desktop);
            }
            if (hot->state == STATE_UNMAPPED) {
                wm_client_restore(s, h);
            }
            wm_set_focus(s, h);
            stack_raise(s, h);
        }
        return;
    }

    if (ev->type == atoms._NET_WM_STATE) {
        handle_t h = server_get_client_by_window(s, ev->window);
        if (h != HANDLE_INVALID) {
            uint32_t action = ev->data.data32[0];
            xcb_atom_t p1 = ev->data.data32[1];
            xcb_atom_t p2 = ev->data.data32[2];
            if (p1 != XCB_ATOM_NONE) wm_client_update_state(s, h, action, p1);
            if (p2 != XCB_ATOM_NONE) wm_client_update_state(s, h, action, p2);
        }
        return;
    }

    if (ev->type == atoms._NET_CLOSE_WINDOW) {
        handle_t h = server_get_client_by_window(s, ev->window);
        if (h != HANDLE_INVALID) {
            LOG_INFO("Client requested close via _NET_CLOSE_WINDOW");
            client_close(s, h);
        }
        return;
    }
}

// Placement / workspaces

void wm_place_window(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    if (hot->type == WINDOW_TYPE_DOCK || hot->type == WINDOW_TYPE_DESKTOP || hot->type == WINDOW_TYPE_NOTIFICATION ||
        hot->type == WINDOW_TYPE_MENU || hot->type == WINDOW_TYPE_DROPDOWN_MENU ||
        hot->type == WINDOW_TYPE_POPUP_MENU || hot->type == WINDOW_TYPE_TOOLTIP) {
        return;
    }

    // 1. Check rules/types for explicit placement
    if (hot->placement == PLACEMENT_CENTER) {
        hot->desired.x = (int16_t)(s->workarea.x + (s->workarea.w - hot->desired.w) / 2);
        hot->desired.y = (int16_t)(s->workarea.y + (s->workarea.h - hot->desired.h) / 2);
        return;
    } else if (hot->placement == PLACEMENT_MOUSE) {
        xcb_query_pointer_cookie_t cookie = xcb_query_pointer(s->conn, s->root);
        xcb_query_pointer_reply_t* reply = xcb_query_pointer_reply(s->conn, cookie, NULL);
        if (reply) {
            hot->desired.x = (int16_t)(reply->root_x - hot->desired.w / 2);
            hot->desired.y = (int16_t)(reply->root_y - hot->desired.h / 2);
            free(reply);
        }
    } else if (hot->transient_for != HANDLE_INVALID) {
        // Transients: center over parent
        client_hot_t* parent = server_chot(s, hot->transient_for);
        if (parent) {
            hot->desired.x = (int16_t)(parent->server.x + (parent->server.w - hot->desired.w) / 2);
            hot->desired.y = (int16_t)(parent->server.y + (parent->server.h - hot->desired.h) / 2);
            return;
        }
    }

    // Honor user/program position if specified (only if no rule applied)
    if (hot->placement == PLACEMENT_DEFAULT &&
        (hot->hints_flags & (XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION))) {
        return;
    }

    // Clamp to workarea
    if (hot->desired.x < s->workarea.x) hot->desired.x = s->workarea.x;
    if (hot->desired.y < s->workarea.y) hot->desired.y = s->workarea.y;

    if ((int32_t)hot->desired.x + (int32_t)hot->desired.w > (int32_t)s->workarea.x + (int32_t)s->workarea.w) {
        hot->desired.x = (int16_t)(s->workarea.x + s->workarea.w - hot->desired.w);
    }
    if ((int32_t)hot->desired.y + (int32_t)hot->desired.h > (int32_t)s->workarea.y + (int32_t)s->workarea.h) {
        hot->desired.y = (int16_t)(s->workarea.y + s->workarea.h - hot->desired.h);
    }

    if (hot->desired.x < s->workarea.x) hot->desired.x = s->workarea.x;
    if (hot->desired.y < s->workarea.y) hot->desired.y = s->workarea.y;
}

void wm_switch_workspace(server_t* s, uint32_t new_desktop) {
    if (s->desktop_count == 0) s->desktop_count = 1;
    if (new_desktop >= s->desktop_count) return;
    if (new_desktop == s->current_desktop) return;

    LOG_INFO("Switching workspace %u -> %u", s->current_desktop, new_desktop);

    s->current_desktop = new_desktop;

    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, 32, 1,
                        &new_desktop);

    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (!s->clients.hdr[i].live) continue;

        handle_t h = handle_make(i, s->clients.hdr[i].gen);
        client_hot_t* c = server_chot(s, h);
        if (!c) continue;

        if (c->state != STATE_MAPPED) continue;

        bool visible = (c->desktop == (int32_t)new_desktop) || c->sticky;
        if (visible) {
            xcb_map_window(s->conn, c->frame);
            uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                state_vals);
            c->dirty |= DIRTY_STATE;
        } else {
            xcb_unmap_window(s->conn, c->frame);
            uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                state_vals);
            c->dirty |= DIRTY_STATE;
        }
    }

    bool focused_visible = false;
    if (s->focused_client != HANDLE_INVALID) {
        client_hot_t* c = server_chot(s, s->focused_client);
        if (c && (c->desktop == (int32_t)new_desktop || c->sticky)) focused_visible = true;
    }

    if (!focused_visible) {
        handle_t new_focus = HANDLE_INVALID;
        for (list_node_t* node = s->focus_history.next; node != &s->focus_history; node = node->next) {
            client_hot_t* c = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));
            if (c->state == STATE_MAPPED && (c->desktop == (int32_t)new_desktop || c->sticky)) {
                new_focus = c->self;
                break;
            }
        }
        wm_set_focus(s, new_focus);
    }

    s->root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
    xcb_flush(s->conn);
}

void wm_switch_workspace_relative(server_t* s, int delta) {
    if (s->desktop_count == 0) s->desktop_count = 1;
    int32_t next = (int32_t)s->current_desktop + delta;
    if (next < 0) next = (int32_t)s->desktop_count - 1;
    if (next >= (int32_t)s->desktop_count) next = 0;
    wm_switch_workspace(s, (uint32_t)next);
}

void wm_client_move_to_workspace(server_t* s, handle_t h, uint32_t desktop, bool follow) {
    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    if (desktop != 0xFFFFFFFF && desktop >= s->desktop_count) {
        if (s->desktop_count == 1) {
            desktop = 0;
        } else {
            return;
        }
    }

    int32_t new_desk = (desktop == 0xFFFFFFFF) ? -1 : (int32_t)desktop;

    LOG_INFO("Moving client %u to desktop %d (follow=%d)", c->xid, new_desk, follow);

    c->desktop = new_desk;
    c->sticky = (new_desk == -1);

    if (follow && !c->sticky) {
        wm_switch_workspace(s, desktop);
        wm_set_focus(s, h);
    } else if (c->state == STATE_MAPPED) {
        bool visible = c->sticky || (c->desktop == (int32_t)s->current_desktop);
        if (visible) {
            xcb_map_window(s->conn, c->frame);
            uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                state_vals);
            c->dirty |= DIRTY_STATE;
        } else {
            xcb_unmap_window(s->conn, c->frame);
            uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                state_vals);
            c->dirty |= DIRTY_STATE;
        }

        if (!visible && s->focused_client == h) {
            handle_t new_focus = HANDLE_INVALID;
            for (list_node_t* node = s->focus_history.next; node != &s->focus_history; node = node->next) {
                client_hot_t* hc = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));
                if (hc->state == STATE_MAPPED && (hc->desktop == (int32_t)s->current_desktop || hc->sticky)) {
                    new_focus = hc->self;
                    break;
                }
            }
            wm_set_focus(s, new_focus);
        }
    }

    uint32_t prop_val = desktop;
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 32, 1,
                        &prop_val);

    c->dirty |= DIRTY_STATE;
    s->root_dirty |= ROOT_DIRTY_CLIENT_LIST | ROOT_DIRTY_CLIENT_LIST_STACKING | ROOT_DIRTY_ACTIVE_WINDOW;
    xcb_flush(s->conn);
}

void wm_client_toggle_sticky(server_t* s, handle_t h) {
    client_hot_t* c = server_chot(s, h);
    if (!c) return;

    c->sticky = !c->sticky;

    LOG_INFO("Client %u sticky toggled to %d", c->xid, c->sticky);

    if (c->state == STATE_MAPPED) {
        bool visible = c->sticky || (c->desktop == (int32_t)s->current_desktop);
        if (visible) {
            xcb_map_window(s->conn, c->frame);
            uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                state_vals);
            c->dirty |= DIRTY_STATE;
        } else {
            xcb_unmap_window(s->conn, c->frame);
            uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                state_vals);
            c->dirty |= DIRTY_STATE;
        }

        if (!visible && s->focused_client == h) {
            handle_t new_focus = HANDLE_INVALID;
            for (list_node_t* node = s->focus_history.next; node != &s->focus_history; node = node->next) {
                client_hot_t* hc = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));
                if (hc->state == STATE_MAPPED && (hc->desktop == (int32_t)s->current_desktop || hc->sticky)) {
                    new_focus = hc->self;
                    break;
                }
            }
            wm_set_focus(s, new_focus);
        }
    }

    uint32_t desktop = c->sticky ? 0xFFFFFFFFu : (uint32_t)c->desktop;
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms._NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 32, 1,
                        &desktop);

    c->dirty |= DIRTY_STATE;
    xcb_flush(s->conn);
}

void wm_client_refresh_title(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    client_cold_t* cold = server_ccold(s, h);
    if (!hot || !cold) return;

    cold->title = cold->base_title;

    hot->dirty |= DIRTY_TITLE | DIRTY_FRAME_STYLE;
}

void wm_client_toggle_maximize(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    bool want = !(hot->maximized_horz && hot->maximized_vert);
    wm_client_set_maximize(s, hot, want, want);
    LOG_INFO("Client %u maximized toggle: %d", hot->xid, want);
}

void wm_client_iconify(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot || hot->state != STATE_MAPPED) return;

    LOG_INFO("Iconifying client %u", hot->xid);

    hot->state = STATE_UNMAPPED;
    xcb_unmap_window(s->conn, hot->frame);
    stack_remove(s, h);

    if (s->focused_client == h) {
        handle_t next_h = HANDLE_INVALID;
        for (list_node_t* node = s->focus_history.next; node != &s->focus_history; node = node->next) {
            client_hot_t* cand = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));
            if (cand->state == STATE_MAPPED) {
                next_h = cand->self;
                break;
            }
        }
        wm_set_focus(s, next_h);
    }

    // Set WM_STATE to IconicState
    uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_ICONIC, XCB_NONE};
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2, state_vals);

    hot->dirty |= DIRTY_STATE;
}

void wm_client_restore(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot || hot->state == STATE_MAPPED) return;

    LOG_INFO("Restoring client %u", hot->xid);

    hot->state = STATE_MAPPED;
    xcb_map_window(s->conn, hot->xid);
    xcb_map_window(s->conn, hot->frame);

    uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2, state_vals);

    hot->dirty |= DIRTY_STATE;
    stack_raise(s, h);
}
