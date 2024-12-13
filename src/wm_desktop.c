/* src/wm_desktop.c
 * Workspace and desktop management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include "client.h"
#include "event.h"
#include "frame.h"
#include "hxm.h"
#include "wm.h"
#include "wm_internal.h"

static bool wm_should_hide_for_show_desktop(const client_hot_t* hot) {
    if (!hot) return false;
    return hot->type != WINDOW_TYPE_DOCK && hot->type != WINDOW_TYPE_DESKTOP;
}

void wm_set_showing_desktop(server_t* s, bool show) {
    if (!s) return;
    if (s->showing_desktop == show) return;
    s->showing_desktop = show;
    TRACE_LOG("showing_desktop set=%d", show);

    uint32_t val = show ? 1u : 0u;
    xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, s->root, atoms._NET_SHOWING_DESKTOP, XCB_ATOM_CARDINAL, 32, 1,
                        &val);

    if (show) {
        for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
            if (!slotmap_is_used_idx(&s->clients, i)) continue;
            handle_t h = slotmap_handle_at(&s->clients, i);
            client_hot_t* hot = (client_hot_t*)slotmap_hot_at(&s->clients, i);
            if (hot->state != STATE_MAPPED) continue;
            if (!wm_should_hide_for_show_desktop(hot)) continue;
            hot->show_desktop_hidden = true;
            TRACE_LOG("showing_desktop hide h=%lx xid=%u", h, hot->xid);
            wm_client_iconify(s, h);
        }
        wm_set_focus(s, HANDLE_INVALID);
    } else {
        for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
            if (!slotmap_is_used_idx(&s->clients, i)) continue;
            handle_t h = slotmap_handle_at(&s->clients, i);
            client_hot_t* hot = (client_hot_t*)slotmap_hot_at(&s->clients, i);
            if (!hot->show_desktop_hidden) continue;
            hot->show_desktop_hidden = false;
            if (hot->state == STATE_UNMAPPED) {
                TRACE_LOG("showing_desktop restore h=%lx xid=%u", h, hot->xid);
                wm_client_restore(s, h);
            }
        }
    }
}

void wm_publish_desktop_props(server_t* s) {
    xcb_connection_t* conn = s->conn;
    xcb_window_t root = s->root;

    if (s->desktop_count == 0) s->desktop_count = 1;
    if (s->current_desktop >= s->desktop_count) s->current_desktop = 0;

    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, 32, 1,
                        &s->desktop_count);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, 32, 1,
                        &s->current_desktop);

    xcb_window_t* vroots = calloc(s->desktop_count, sizeof(*vroots));
    if (vroots) {
        for (uint32_t i = 0; i < s->desktop_count; i++) {
            vroots[i] = root;
        }
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_VIRTUAL_ROOTS, XCB_ATOM_WINDOW, 32,
                            s->desktop_count, vroots);
        free(vroots);
    }

    bool publish_names = s->config.desktop_names && s->config.desktop_names_count > 0;
    if (!publish_names) {
        xcb_get_property_cookie_t ck =
            xcb_get_property(conn, 0, root, atoms._NET_DESKTOP_NAMES, atoms.UTF8_STRING, 0, 1024);
        xcb_get_property_reply_t* r = xcb_get_property_reply(conn, ck, NULL);
        if (!r || xcb_get_property_value_length(r) == 0) {
            publish_names = true;
        }
        if (r) free(r);
    }

    if (publish_names) {
        uint32_t name_count = (s->config.desktop_names && s->config.desktop_names_count > 0)
                                  ? s->config.desktop_names_count
                                  : s->desktop_count;
        size_t name_bytes = 0;
        for (uint32_t i = 0; i < name_count; i++) {
            const char* name = NULL;
            char fallback[32];
            if (s->config.desktop_names && i < s->config.desktop_names_count) {
                name = s->config.desktop_names[i];
            }
            if (!name) {
                if (s->config.desktop_names && s->config.desktop_names_count > 0) {
                    name = "";
                } else {
                    snprintf(fallback, sizeof(fallback), "%u", i + 1);
                    name = fallback;
                }
            }
            name_bytes += strlen(name) + 1;
        }

        char* buf = malloc(name_bytes);
        if (buf) {
            size_t offset = 0;
            for (uint32_t i = 0; i < name_count; i++) {
                const char* name = NULL;
                char fallback[32];
                if (s->config.desktop_names && i < s->config.desktop_names_count) {
                    name = s->config.desktop_names[i];
                }
                if (!name) {
                    if (s->config.desktop_names && s->config.desktop_names_count > 0) {
                        name = "";
                    } else {
                        snprintf(fallback, sizeof(fallback), "%u", i + 1);
                        name = fallback;
                    }
                }
                size_t len = strlen(name) + 1;
                memcpy(buf + offset, name, len);
                offset += len;
            }
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms._NET_DESKTOP_NAMES, atoms.UTF8_STRING, 8,
                                (uint32_t)name_bytes, buf);
            free(buf);
        }
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

    for (uint32_t i = 1; i < slotmap_capacity(&s->clients); i++) {
        if (!slotmap_is_used_idx(&s->clients, i)) continue;

        client_hot_t* c = (client_hot_t*)slotmap_hot_at(&s->clients, i);
        if (c->state != STATE_MAPPED) continue;
        client_cold_t* cold = (client_cold_t*)slotmap_cold_at(&s->clients, i);
        if (!cold) continue;

        if ((int32_t)cold->strut.left > l) l = (int32_t)cold->strut.left;
        if ((int32_t)((int32_t)screen->width_in_pixels - (int32_t)cold->strut.right) < r)
            r = (int32_t)screen->width_in_pixels - (int32_t)cold->strut.right;
        if ((int32_t)cold->strut.top > t) t = (int32_t)cold->strut.top;
        if ((int32_t)((int32_t)screen->height_in_pixels - (int32_t)cold->strut.bottom) < b)
            b = (int32_t)screen->height_in_pixels - (int32_t)cold->strut.bottom;
    }

    out->x = (int16_t)l;
    out->y = (int16_t)t;
    out->w = (uint16_t)((r > l) ? (r - l) : 0);
    out->h = (uint16_t)((b > t) ? (b - t) : 0);
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
            TRACE_LOG("switch_workspace map h=%lx xid=%u desktop=%d sticky=%d", h, c->xid, c->desktop, c->sticky);
            xcb_map_window(s->conn, c->frame);
            uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                state_vals);
            c->dirty |= DIRTY_STATE;
        } else {
            TRACE_LOG("switch_workspace unmap h=%lx xid=%u desktop=%d sticky=%d", h, c->xid, c->desktop, c->sticky);
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
            TRACE_LOG("move_workspace map h=%lx xid=%u desktop=%d sticky=%d", h, c->xid, c->desktop, c->sticky);
            xcb_map_window(s->conn, c->frame);
            uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                state_vals);
            c->dirty |= DIRTY_STATE;
        } else {
            TRACE_LOG("move_workspace unmap h=%lx xid=%u desktop=%d sticky=%d", h, c->xid, c->desktop, c->sticky);
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
            TRACE_LOG("toggle_sticky map h=%lx xid=%u desktop=%d sticky=%d", h, c->xid, c->desktop, c->sticky);
            xcb_map_window(s->conn, c->frame);
            uint32_t state_vals[] = {XCB_ICCCM_WM_STATE_NORMAL, XCB_NONE};
            xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, c->xid, atoms.WM_STATE, atoms.WM_STATE, 32, 2,
                                state_vals);
            c->dirty |= DIRTY_STATE;
        } else {
            TRACE_LOG("toggle_sticky unmap h=%lx xid=%u desktop=%d sticky=%d", h, c->xid, c->desktop, c->sticky);
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
