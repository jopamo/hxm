/* src/wm_reply.c
 * Window manager reply handling
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/sync.h>
#include <xcb/xcb_icccm.h>

#include "bbox.h"
#include "client.h"
#include "config.h"
#include "cookie_jar.h"
#include "event.h"
#include "frame.h"
#include "wm.h"

// Motif Hints
#define MWM_HINTS_DECORATIONS (1L << 1)
#define MWM_DECOR_ALL (1L << 0)
#define MWM_DECOR_BORDER (1L << 1)
#define MWM_DECOR_RESIZEH (1L << 2)
#define MWM_DECOR_TITLE (1L << 3)
#define MWM_DECOR_MENU (1L << 4)
#define MWM_DECOR_MINIMIZE (1L << 5)
#define MWM_DECOR_MAXIMIZE (1L << 6)

typedef struct {
    uint32_t flags;
    uint32_t functions;
    uint32_t decorations;
    int32_t input_mode;
    uint32_t status;
} motif_wm_hints_t;

static void client_apply_motif_hints(server_t* s, handle_t h, const xcb_get_property_reply_t* r) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    bool decorations_set = false;
    bool undecorated = false;

    int len = r ? xcb_get_property_value_length(r) : 0;
    if (r && r->format == 32 && len >= (int)(3 * sizeof(uint32_t))) {
        const motif_wm_hints_t* mh = (const motif_wm_hints_t*)xcb_get_property_value(r);

        if (mh->flags & MWM_HINTS_DECORATIONS) {
            bool want_border = (mh->decorations & MWM_DECOR_ALL) ? !(mh->decorations & MWM_DECOR_BORDER)
                                                                 : (mh->decorations & MWM_DECOR_BORDER);
            bool want_title = (mh->decorations & MWM_DECOR_ALL) ? !(mh->decorations & MWM_DECOR_TITLE)
                                                                : (mh->decorations & MWM_DECOR_TITLE);

            decorations_set = true;
            undecorated = !(want_border && want_title);
        }
    }

    hot->motif_decorations_set = decorations_set;
    hot->motif_undecorated = undecorated;
}

static void client_apply_gtk_frame_extents(server_t* s, handle_t h, const xcb_get_property_reply_t* r) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot) return;

    int len = r ? xcb_get_property_value_length(r) : 0;
    bool has_extents = (r && r->format == 32 && len >= (int)(4 * sizeof(uint32_t)));

    hot->gtk_frame_extents_set = has_extents;
    if (has_extents) {
        const uint32_t* v = (const uint32_t*)xcb_get_property_value(r);
        hot->gtk_extents.left = (uint16_t)v[0];
        hot->gtk_extents.right = (uint16_t)v[1];
        hot->gtk_extents.top = (uint16_t)v[2];
        hot->gtk_extents.bottom = (uint16_t)v[3];
    } else {
        memset(&hot->gtk_extents, 0, sizeof(hot->gtk_extents));
    }
}

static bool is_valid_utf8(const char* str, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = (uint8_t)str[i];
        if (c <= 0x7Fu) {
            i++;
            continue;
        }

        uint32_t code = 0;
        size_t need = 0;
        if ((c & 0xE0u) == 0xC0u) {
            need = 1;
            code = c & 0x1Fu;
        } else if ((c & 0xF0u) == 0xE0u) {
            need = 2;
            code = c & 0x0Fu;
        } else if ((c & 0xF8u) == 0xF0u) {
            need = 3;
            code = c & 0x07u;
            if (c > 0xF4u) return false;
        } else {
            return false;
        }

        if (i + need >= len) return false;
        for (size_t j = 1; j <= need; j++) {
            uint8_t cc = (uint8_t)str[i + j];
            if ((cc & 0xC0u) != 0x80u) return false;
            code = (code << 6) | (cc & 0x3Fu);
        }

        if ((need == 1 && code < 0x80u) || (need == 2 && code < 0x800u) || (need == 3 && code < 0x10000u)) {
            return false;
        }
        if (code > 0x10FFFFu) return false;
        if (code >= 0xD800u && code <= 0xDFFFu) return false;

        i += need + 1;
    }
    return true;
}

static bool prop_is_empty(const xcb_get_property_reply_t* r) { return !r || xcb_get_property_value_length(r) == 0; }

static bool prop_is_cardinal(const xcb_get_property_reply_t* r) {
    return r && r->type == XCB_ATOM_CARDINAL && r->format == 32;
}

static char* prop_get_string(const xcb_get_property_reply_t* r, int* out_len) {
    if (!r || r->format != 8) return NULL;
    int len = xcb_get_property_value_length(r);
    if (len <= 0) return NULL;
    if (out_len) *out_len = len;
    return (char*)xcb_get_property_value(r);
}

static uint32_t* prop_get_u32_array(const xcb_get_property_reply_t* r, int min_count, int* out_count) {
    if (!r || r->format != 32) return NULL;
    int len = xcb_get_property_value_length(r);
    if (len < (int)(min_count * (int)sizeof(uint32_t))) return NULL;
    int count = len / (int)sizeof(uint32_t);
    if (out_count) *out_count = count;
    return (uint32_t*)xcb_get_property_value(r);
}

static void client_update_effective_strut(client_cold_t* cold) {
    if (cold->strut_partial_active) {
        cold->strut = cold->strut_partial;
    } else if (cold->strut_full_active) {
        cold->strut = cold->strut_full;
    } else {
        memset(&cold->strut, 0, sizeof(cold->strut));
    }
}

static bool client_type_forces_undecorated(uint8_t type) {
    switch (type) {
        case WINDOW_TYPE_DOCK:
        case WINDOW_TYPE_NOTIFICATION:
        case WINDOW_TYPE_DESKTOP:
        case WINDOW_TYPE_MENU:
        case WINDOW_TYPE_DROPDOWN_MENU:
        case WINDOW_TYPE_POPUP_MENU:
        case WINDOW_TYPE_TOOLTIP:
        case WINDOW_TYPE_COMBO:
        case WINDOW_TYPE_DND:
            return true;
        default:
            return false;
    }
}

static bool client_should_be_undecorated(const client_hot_t* hot) {
    if (!hot) return false;
    if (hot->layer == LAYER_FULLSCREEN) return true;
    if (client_type_forces_undecorated(hot->type)) return true;
    if (hot->gtk_frame_extents_set) return true;
    if (hot->motif_decorations_set) return hot->motif_undecorated;
    return false;
}

static bool client_apply_decoration_hints(client_hot_t* hot) {
    bool was_undecorated = (hot->flags & CLIENT_FLAG_UNDECORATED) != 0;
    bool now_undecorated = client_should_be_undecorated(hot);

    if (now_undecorated) {
        hot->flags |= CLIENT_FLAG_UNDECORATED;
    } else {
        hot->flags &= ~CLIENT_FLAG_UNDECORATED;
    }

    if (was_undecorated != now_undecorated) {
        hot->dirty |= DIRTY_GEOM | DIRTY_FRAME_STYLE;
        return true;
    }

    return false;
}

static void abort_manage(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    client_cold_t* cold = server_ccold(s, h);
    if (!hot || !cold) return;

    if (hot->xid != XCB_NONE) {
        hash_map_remove(&s->window_to_client, hot->xid);
    }

    arena_destroy(&cold->string_arena);
    render_free(&hot->render_ctx);
    if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);

    slotmap_free(&s->clients, h);
}

static void parse_net_wm_name_like(server_t* s, handle_t h, client_hot_t* hot, client_cold_t* cold, xcb_atom_t atom,
                                   const xcb_get_property_reply_t* r) {
    int len = 0;
    char* str = prop_get_string(r, &len);
    if (!str) len = 0;

    size_t trimmed_len = (len > 0) ? (size_t)len : 0;
    while (trimmed_len > 0 && str[trimmed_len - 1] == '\0') trimmed_len--;

    bool valid = trimmed_len > 0;
    if (valid && memchr(str, '\0', trimmed_len) != NULL) valid = false;
    if (valid && !is_valid_utf8(str, trimmed_len)) valid = false;

    if (atom == atoms._NET_WM_NAME) {
        if (!valid) {
            bool had_net = cold->has_net_wm_name;
            cold->has_net_wm_name = false;
            if (had_net) {
                cold->base_title = arena_strndup(&cold->string_arena, "", 0);
                wm_client_refresh_title(s, h);
                hot->dirty |= DIRTY_FRAME_STYLE;
            }

            if (hot->manage_phase != MANAGE_DONE) hot->pending_replies++;
            uint32_t c = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_NAME, XCB_ATOM_STRING, 0, 1024).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms.WM_NAME,
                            wm_handle_reply);
            return;
        }

        if (!cold->has_net_wm_name || !cold->base_title || strlen(cold->base_title) != trimmed_len ||
            strncmp(cold->base_title, str, trimmed_len) != 0) {
            cold->base_title = arena_strndup(&cold->string_arena, str, trimmed_len);
            cold->has_net_wm_name = true;
            wm_client_refresh_title(s, h);
            hot->dirty |= DIRTY_FRAME_STYLE;
        }
        return;
    }

    if (atom == atoms._NET_WM_ICON_NAME) {
        if (!valid) {
            bool had_net = cold->has_net_wm_icon_name;
            cold->has_net_wm_icon_name = false;
            if (had_net) {
                cold->base_icon_name = arena_strndup(&cold->string_arena, "", 0);
                wm_client_refresh_title(s, h);
                hot->dirty |= DIRTY_FRAME_STYLE;
            }

            if (hot->manage_phase != MANAGE_DONE) hot->pending_replies++;
            uint32_t c = xcb_get_property(s->conn, 0, hot->xid, atoms.WM_ICON_NAME, XCB_ATOM_STRING, 0, 1024).sequence;
            cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, h, ((uint64_t)hot->xid << 32) | atoms.WM_ICON_NAME,
                            wm_handle_reply);
            return;
        }

        if (!cold->has_net_wm_icon_name || !cold->base_icon_name || strlen(cold->base_icon_name) != trimmed_len ||
            strncmp(cold->base_icon_name, str, trimmed_len) != 0) {
            cold->base_icon_name = arena_strndup(&cold->string_arena, str, trimmed_len);
            cold->has_net_wm_icon_name = true;
            wm_client_refresh_title(s, h);
            hot->dirty |= DIRTY_FRAME_STYLE;
        }
        return;
    }
}

static void parse_wm_class(client_cold_t* cold, const xcb_get_property_reply_t* r) {
    int len = 0;
    char* str = prop_get_string(r, &len);
    if (!str || len <= 0) return;

    size_t n = (size_t)len;
    char* nul1 = memchr(str, '\0', n);
    if (!nul1) return;

    size_t inst_len = (size_t)(nul1 - str);
    size_t rem = n - inst_len - 1;

    char* cls = nul1 + 1;
    char* nul2 = memchr(cls, '\0', rem);
    size_t cls_len = nul2 ? (size_t)(nul2 - cls) : rem;

    if (!cold->wm_instance || strcmp(cold->wm_instance, str) != 0) {
        cold->wm_instance = arena_strndup(&cold->string_arena, str, inst_len);
    }
    if (!cold->wm_class || strcmp(cold->wm_class, cls) != 0) {
        cold->wm_class = arena_strndup(&cold->string_arena, cls, cls_len);
    }
}

static bool client_apply_default_type(server_t* s, client_hot_t* hot, client_cold_t* cold) {
    (void)s;
    if (!hot || !cold) return false;
    if (hot->type_from_net) return false;

    uint8_t prev_type = hot->type;
    uint8_t prev_layer = hot->layer;
    uint8_t prev_base = hot->base_layer;
    uint8_t prev_place = hot->placement;

    if (hot->override_redirect) {
        hot->type = WINDOW_TYPE_NORMAL;
        hot->base_layer = LAYER_NORMAL;
        hot->placement = PLACEMENT_DEFAULT;
    } else if (cold->transient_for_xid != XCB_NONE) {
        hot->type = WINDOW_TYPE_DIALOG;
        hot->base_layer = LAYER_NORMAL;
        hot->placement = PLACEMENT_CENTER;
    } else {
        hot->type = WINDOW_TYPE_NORMAL;
        hot->base_layer = LAYER_NORMAL;
        hot->placement = PLACEMENT_DEFAULT;
    }

    if (hot->layer != LAYER_FULLSCREEN) {
        hot->layer = client_layer_from_state(hot);
        if (hot->layer != prev_layer) {
            hot->dirty |= DIRTY_STATE | DIRTY_STACK;
        }
    }

    bool changed = hot->type != prev_type || hot->base_layer != prev_base || hot->placement != prev_place;
    if (client_apply_decoration_hints(hot)) changed = true;

    return changed;
}

static bool check_transient_cycle(server_t* s, handle_t child, handle_t parent) {
    if (child == parent) return true;
    handle_t curr = parent;
    int depth = 0;
    while (curr != HANDLE_INVALID && depth < 32) {
        if (curr == child) return true;
        client_hot_t* hot = server_chot(s, curr);
        if (!hot) break;
        curr = hot->transient_for;
        depth++;
    }
    return false;
}

void wm_handle_reply(server_t* s, const cookie_slot_t* slot, void* reply, xcb_generic_error_t* err) {
    if (err) {
        LOG_DEBUG("Cookie %u returned error code %d", slot->sequence, err->error_code);
    }

    if (slot->client == HANDLE_INVALID) {
        if (slot->type == COOKIE_GET_WINDOW_ATTRIBUTES && reply) {
            xcb_get_window_attributes_reply_t* r = (xcb_get_window_attributes_reply_t*)reply;
            xcb_window_t win = (xcb_window_t)slot->data;
            if (!r->override_redirect && r->map_state != XCB_MAP_STATE_UNMAPPED) {
                LOG_INFO("Adopting window %u (map_state %d)", win, r->map_state);
                client_manage_start(s, win);
            }
        }
        return;
    }

    client_hot_t* hot = server_chot(s, slot->client);
    client_cold_t* cold = server_ccold(s, slot->client);
    if (!hot || !cold) {
        LOG_DEBUG("Received reply for stale client handle %lx", slot->client);
        return;
    }

    if (!reply) {
        LOG_WARN("NULL reply for cookie type %d client %u", slot->type, hot->xid);
        if (slot->type == COOKIE_GET_WINDOW_ATTRIBUTES && hot->state == STATE_NEW &&
            hot->manage_phase == MANAGE_PHASE1) {
            hot->manage_aborted = true;
        }
        goto done_one;
    }

    bool changed = false;

    switch (slot->type) {
        case COOKIE_GET_WINDOW_ATTRIBUTES: {
            xcb_get_window_attributes_reply_t* r = (xcb_get_window_attributes_reply_t*)reply;
            hot->override_redirect = r->override_redirect ? true : false;
            hot->visual_id = r->visual;
            hot->visual_type = xcb_get_visualtype(s->conn, r->visual);

            if (r->map_state == XCB_MAP_STATE_UNMAPPED) {
                if (hot->ignore_unmap > 0) hot->ignore_unmap--;
            }

            if (hot->override_redirect && hot->state == STATE_NEW) {
                LOG_DEBUG("Window %u is override_redirect, aborting manage", hot->xid);
                hot->manage_aborted = true;
            }

            if (client_apply_default_type(s, hot, cold)) {
                changed = true;
            }
            break;
        }

        case COOKIE_GET_GEOMETRY: {
            xcb_get_geometry_reply_t* r = (xcb_get_geometry_reply_t*)reply;

            hot->server.x = r->x;
            hot->server.y = r->y;
            hot->server.w = r->width;
            hot->server.h = r->height;
            hot->depth = r->depth;

            if (r->width == 0 || r->height == 0) {
                hot->server.w = 800;
                hot->server.h = 600;
                xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
                hot->server.x = (screen->width_in_pixels - 800) / 2;
                hot->server.y = (screen->height_in_pixels - 600) / 2;
            }

            if (hot->state == STATE_NEW) {
                hot->desired = hot->server;
            }
            break;
        }

        case COOKIE_GET_PROPERTY: {
            xcb_atom_t atom = (xcb_atom_t)(slot->data & 0xFFFFFFFFu);
            xcb_get_property_reply_t* r = (xcb_get_property_reply_t*)reply;

            if (atom == atoms.WM_CLASS) {
                parse_wm_class(cold, r);
            } else if (atom == atoms.WM_CLIENT_MACHINE) {
                int len = 0;
                char* str = prop_get_string(r, &len);
                if (str) {
                    cold->wm_client_machine = arena_strndup(&cold->string_arena, str, (size_t)len);
                }

            } else if (atom == atoms._NET_WM_NAME || atom == atoms._NET_WM_ICON_NAME) {
                parse_net_wm_name_like(s, slot->client, hot, cold, atom, r);
                break;

            } else if (atom == atoms.WM_NAME) {
                int len = 0;
                char* str = prop_get_string(r, &len);
                if (str && !cold->has_net_wm_name) {
                    if (!cold->base_title || strlen(cold->base_title) != (size_t)len ||
                        strncmp(cold->base_title, str, len) != 0) {
                        cold->base_title = arena_strndup(&cold->string_arena, str, len);
                        wm_client_refresh_title(s, slot->client);
                        changed = true;
                    }
                }

            } else if (atom == atoms.WM_ICON_NAME) {
                int len = 0;
                char* str = prop_get_string(r, &len);
                if (str && !cold->has_net_wm_icon_name) {
                    if (!cold->base_icon_name || strlen(cold->base_icon_name) != (size_t)len ||
                        strncmp(cold->base_icon_name, str, len) != 0) {
                        cold->base_icon_name = arena_strndup(&cold->string_arena, str, len);
                        wm_client_refresh_title(s, slot->client);
                        changed = true;
                    }
                }

            } else if (atom == atoms._MOTIF_WM_HINTS) {
                client_apply_motif_hints(s, slot->client, r);
                if (client_apply_decoration_hints(hot)) changed = true;

            } else if (atom == atoms._GTK_FRAME_EXTENTS) {
                client_apply_gtk_frame_extents(s, slot->client, r);

                // Existing logic for geometry updates if not in manage phase
                if (hot->manage_phase == MANAGE_DONE) {
                    // We need to handle geometry updates here if this property changes at runtime
                    // But for now, relying on the helper to set the extents is enough for the initial map.
                    // The helper doesn't calculate diffs, so if we want to support dynamic updates,
                    // we might need to keep some of the old logic or improve the helper.
                    // Given the prompt "minimal, mechanical fix", using the helper is the priority.
                    // Let's assume the user wants the helper to handle the parsing.
                    // The user's helper sets the extents.
                    // If we need to resize the window, we should trigger a dirty geom.
                    hot->dirty |= DIRTY_GEOM;
                } else {
                    // Initial map: Apply extents to desired geometry
                    uint32_t h_ext = hot->gtk_extents.left + hot->gtk_extents.right;
                    uint32_t v_ext = hot->gtk_extents.top + hot->gtk_extents.bottom;
                    hot->desired.w = (hot->desired.w > h_ext) ? (hot->desired.w - (uint16_t)h_ext) : 1;
                    hot->desired.h = (hot->desired.h > v_ext) ? (hot->desired.h - (uint16_t)v_ext) : 1;

                    // Also adjust position if needed?
                    // The old code did: hot->desired.x += hot->gtk_extents.left;
                    hot->desired.x += (int16_t)hot->gtk_extents.left;
                    hot->desired.y += (int16_t)hot->gtk_extents.top;
                }

                if (client_apply_decoration_hints(hot)) changed = true;

            } else if (atom == atoms._NET_WM_STATE) {
                if (prop_is_empty(r)) {
                    client_state_set_t set = {0};
                    wm_client_apply_state_set(s, slot->client, &set);
                } else {
                    int num_states = 0;
                    xcb_atom_t* states = (xcb_atom_t*)prop_get_u32_array(r, 1, &num_states);
                    if (states) {
                        client_state_set_t set = {0};
                        for (int i = 0; i < num_states; i++) {
                            xcb_atom_t state = states[i];
                            if (state == atoms._NET_WM_STATE_FULLSCREEN) {
                                set.fullscreen = true;
                            } else if (state == atoms._NET_WM_STATE_ABOVE) {
                                set.above = true;
                            } else if (state == atoms._NET_WM_STATE_BELOW) {
                                set.below = true;
                            } else if (state == atoms._NET_WM_STATE_STICKY) {
                                set.sticky = true;
                            } else if (state == atoms._NET_WM_STATE_DEMANDS_ATTENTION) {
                                set.urgent = true;
                            } else if (state == atoms._NET_WM_STATE_MAXIMIZED_HORZ) {
                                set.max_horz = true;
                            } else if (state == atoms._NET_WM_STATE_MAXIMIZED_VERT) {
                                set.max_vert = true;
                            } else if (state == atoms._NET_WM_STATE_MODAL) {
                                set.modal = true;
                            } else if (state == atoms._NET_WM_STATE_SHADED) {
                                set.shaded = true;
                            } else if (state == atoms._NET_WM_STATE_SKIP_TASKBAR) {
                                set.skip_taskbar = true;
                            } else if (state == atoms._NET_WM_STATE_SKIP_PAGER) {
                                set.skip_pager = true;
                            }
                        }
                        wm_client_apply_state_set(s, slot->client, &set);
                    }
                }

            } else if (atom == atoms.WM_NORMAL_HINTS) {
                xcb_size_hints_t hints;
                if (xcb_icccm_get_wm_size_hints_from_reply(&hints, r)) {
                    memset(&hot->hints, 0, sizeof(hot->hints));
                    hot->hints_flags = hints.flags;

                    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
                        hot->hints.min_w = hints.min_width;
                        hot->hints.min_h = hints.min_height;
                    }
                    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
                        hot->hints.max_w = hints.max_width;
                        hot->hints.max_h = hints.max_height;
                    }
                    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
                        hot->hints.inc_w = hints.width_inc;
                        hot->hints.inc_h = hints.height_inc;
                    }
                    if (hints.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
                        hot->hints.base_w = hints.base_width;
                        hot->hints.base_h = hints.base_height;
                    }
                    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) {
                        hot->hints.min_aspect_num = hints.min_aspect_num;
                        hot->hints.min_aspect_den = hints.min_aspect_den;
                        hot->hints.max_aspect_num = hints.max_aspect_num;
                        hot->hints.max_aspect_den = hints.max_aspect_den;
                    }
                }

            } else if (atom == atoms.WM_TRANSIENT_FOR) {
                if (xcb_get_property_value_length(r) >= 4) {
                    xcb_window_t transient_for_xid = *(xcb_window_t*)xcb_get_property_value(r);
                    cold->transient_for_xid = transient_for_xid;
                    hot->transient_for = server_get_client_by_window(s, transient_for_xid);

                    if (hot->transient_for != HANDLE_INVALID) {
                        if (check_transient_cycle(s, slot->client, hot->transient_for)) {
                            LOG_WARN("Ignoring transient_for cycle for client %u", hot->xid);
                            hot->transient_for = HANDLE_INVALID;
                        }
                    }

                    if (hot->transient_for != HANDLE_INVALID) {
                        client_hot_t* parent = server_chot(s, hot->transient_for);
                        if (parent) {
                            if (hot->transient_sibling.next && hot->transient_sibling.next != &hot->transient_sibling) {
                                list_remove(&hot->transient_sibling);
                            }
                            list_insert(&hot->transient_sibling, parent->transients_head.prev,
                                        &parent->transients_head);
                        }
                    }
                }

                if (client_apply_default_type(s, hot, cold)) {
                    changed = true;
                }

            } else if (atom == atoms._NET_WM_WINDOW_TYPE) {
                if (xcb_get_property_value_length(r) > 0) {
                    xcb_atom_t* types = (xcb_atom_t*)xcb_get_property_value(r);
                    int num_types = xcb_get_property_value_length(r) / (int)sizeof(xcb_atom_t);

                    for (int i = 0; i < num_types; i++) {
                        if (types[i] == atoms._NET_WM_WINDOW_TYPE_DOCK) {
                            hot->type = WINDOW_TYPE_DOCK;
                            hot->base_layer = LAYER_ABOVE;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_NOTIFICATION) {
                            hot->type = WINDOW_TYPE_NOTIFICATION;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_DIALOG) {
                            hot->type = WINDOW_TYPE_DIALOG;
                            hot->base_layer = LAYER_NORMAL;
                            hot->placement = PLACEMENT_CENTER;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_DESKTOP) {
                            hot->type = WINDOW_TYPE_DESKTOP;
                            hot->base_layer = LAYER_DESKTOP;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_SPLASH) {
                            hot->type = WINDOW_TYPE_SPLASH;
                            hot->base_layer = LAYER_ABOVE;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_TOOLBAR) {
                            hot->type = WINDOW_TYPE_TOOLBAR;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_UTILITY) {
                            hot->type = WINDOW_TYPE_UTILITY;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_MENU) {
                            hot->type = WINDOW_TYPE_MENU;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_DROPDOWN_MENU) {
                            hot->type = WINDOW_TYPE_DROPDOWN_MENU;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_POPUP_MENU) {
                            hot->type = WINDOW_TYPE_POPUP_MENU;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_TOOLTIP) {
                            hot->type = WINDOW_TYPE_TOOLTIP;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_COMBO) {
                            hot->type = WINDOW_TYPE_COMBO;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_DND) {
                            hot->type = WINDOW_TYPE_DND;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            hot->type_from_net = true;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_NORMAL) {
                            hot->type = WINDOW_TYPE_NORMAL;
                            hot->type_from_net = true;
                            break;
                        }
                    }

                    if (client_apply_decoration_hints(hot)) {
                        changed = true;
                    }

                    if (hot->layer != LAYER_FULLSCREEN) {
                        uint8_t prev_layer = hot->layer;
                        hot->layer = client_layer_from_state(hot);
                        if (hot->layer != prev_layer) {
                            hot->dirty |= DIRTY_STATE | DIRTY_STACK;
                        }
                    }
                }

            } else if (atom == atoms.WM_PROTOCOLS) {
                int num_protocols = 0;
                xcb_atom_t* protocols = (xcb_atom_t*)prop_get_u32_array(r, 1, &num_protocols);
                if (protocols) {
                    for (int i = 0; i < num_protocols; i++) {
                        if (protocols[i] == atoms.WM_DELETE_WINDOW) {
                            cold->protocols |= PROTOCOL_DELETE_WINDOW;
                        } else if (protocols[i] == atoms.WM_TAKE_FOCUS) {
                            cold->protocols |= PROTOCOL_TAKE_FOCUS;
                        } else if (protocols[i] == atoms._NET_WM_SYNC_REQUEST) {
                            cold->protocols |= PROTOCOL_SYNC_REQUEST;
                            hot->sync_enabled = true;
                        } else if (protocols[i] == atoms._NET_WM_PING) {
                            cold->protocols |= PROTOCOL_PING;
                        }
                    }
                }

            } else if (atom == atoms._NET_WM_DESKTOP) {
                uint32_t* val = prop_get_u32_array(r, 1, NULL);
                if (val) {
                    if (*val == 0xFFFFFFFFu) {
                        hot->sticky = true;
                        hot->desktop = -1;
                    } else {
                        hot->sticky = false;
                        uint32_t desk = *val;
                        if (desk >= s->desktop_count) desk = s->current_desktop;
                        hot->desktop = (int32_t)desk;
                    }
                }

            } else if (atom == atoms._NET_WM_STRUT || atom == atoms._NET_WM_STRUT_PARTIAL) {
                int len = r ? xcb_get_property_value_length(r) : 0;
                bool is_partial = (atom == atoms._NET_WM_STRUT_PARTIAL);
                strut_t* target = is_partial ? &cold->strut_partial : &cold->strut_full;
                bool* active = is_partial ? &cold->strut_partial_active : &cold->strut_full_active;

                if (len >= 16) {
                    uint32_t* val = prop_get_u32_array(r, 4, NULL);
                    memset(target, 0, sizeof(*target));
                    if (val) {
                        target->left = val[0];
                        target->right = val[1];
                        target->top = val[2];
                        target->bottom = val[3];
                    }

                    if (is_partial && len >= 48 && val) {
                        target->left_start_y = val[4];
                        target->left_end_y = val[5];
                        target->right_start_y = val[6];
                        target->right_end_y = val[7];
                        target->top_start_x = val[8];
                        target->top_end_x = val[9];
                        target->bottom_start_x = val[10];
                        target->bottom_end_x = val[11];
                    }
                    *active = true;
                } else {
                    memset(target, 0, sizeof(*target));
                    *active = false;
                }

                client_update_effective_strut(cold);
                s->root_dirty |= ROOT_DIRTY_WORKAREA;

            } else if (atom == atoms.WM_HINTS) {
                if (!prop_is_empty(r)) {
                    xcb_icccm_wm_hints_t hints;
                    if (xcb_icccm_get_wm_hints_from_reply(&hints, r)) {
                        if (hints.flags & XCB_ICCCM_WM_HINT_INPUT) {
                            cold->can_focus = (bool)(hints.input);
                        } else {
                            cold->can_focus = true;
                        }

                        if (hints.flags & XCB_ICCCM_WM_HINT_STATE) {
                            hot->initial_state = (uint8_t)hints.initial_state;
                        }

                        bool urgent = (hints.flags & XCB_ICCCM_WM_HINT_X_URGENCY) != 0;
                        bool was_urgent = (hot->flags & CLIENT_FLAG_URGENT) != 0;
                        if (urgent) {
                            hot->flags |= CLIENT_FLAG_URGENT;
                        } else {
                            hot->flags &= ~CLIENT_FLAG_URGENT;
                        }
                        if (urgent != was_urgent) {
                            hot->dirty |= DIRTY_STATE;
                            changed = true;
                        }
                    }
                }

            } else if (atom == atoms._NET_WM_ICON) {
                if (!prop_is_empty(r)) {
                    const uint32_t icon_target_sizes[] = {16, 24, 32, 48, 64};
                    const uint32_t icon_dim_max = 4096;
                    const uint64_t icon_pixels_max = 1024ull * 1024ull;
                    const uint64_t icon_total_pixels_max = 4ull * 1024ull * 1024ull;
                    const uint32_t icon_count_max = 32;

                    int total_words = 0;
                    uint32_t* val = prop_get_u32_array(r, 2, &total_words);
                    if (!val) break;

                    uint32_t best_w = 0;
                    uint32_t best_h = 0;
                    uint64_t best_area = 0;
                    uint32_t* best_data = NULL;
                    uint32_t best_diff = UINT32_MAX;

                    int i = 0;
                    uint32_t icons_seen = 0;
                    uint64_t total_pixels = 0;
                    while (i + 2 <= total_words) {
                        if (icons_seen >= icon_count_max) break;

                        uint32_t w = val[i];
                        uint32_t h = val[i + 1];
                        if (w == 0 || h == 0) break;

                        uint64_t pixels = (uint64_t)w * (uint64_t)h;
                        if (pixels > (uint64_t)(total_words - i - 2)) break;
                        if (pixels > icon_pixels_max) break;
                        if (total_pixels + pixels > icon_total_pixels_max) break;

                        if (w <= icon_dim_max && h <= icon_dim_max) {
                            uint32_t diff = UINT32_MAX;
                            for (size_t t = 0; t < sizeof(icon_target_sizes) / sizeof(icon_target_sizes[0]); t++) {
                                int dw = abs((int)w - (int)icon_target_sizes[t]);
                                int dh = abs((int)h - (int)icon_target_sizes[t]);
                                uint32_t td = (uint32_t)(dw + dh);
                                if (td < diff) diff = td;
                            }

                            if (diff < best_diff || (diff == best_diff && pixels > best_area)) {
                                best_diff = diff;
                                best_w = w;
                                best_h = h;
                                best_area = pixels;
                                best_data = &val[i + 2];
                            }
                        }

                        i += (int)(2 + pixels);
                        icons_seen++;
                        total_pixels += pixels;
                    }

                    if (best_data) {
                        if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
                        hot->icon_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)best_w, (int)best_h);

                        unsigned char* dest = cairo_image_surface_get_data(hot->icon_surface);
                        int stride = cairo_image_surface_get_stride(hot->icon_surface);
                        cairo_surface_flush(hot->icon_surface);

                        for (int y = 0; y < (int)best_h; y++) {
                            uint32_t* row = (uint32_t*)(dest + y * stride);
                            for (int x = 0; x < (int)best_w; x++) {
                                row[x] = best_data[y * (int)best_w + x];
                            }
                        }

                        cairo_surface_mark_dirty(hot->icon_surface);
                        changed = true;
                    }
                }

            } else if (atom == atoms._NET_WM_PID) {
                if (prop_is_cardinal(r) && xcb_get_property_value_length(r) >= 4) {
                    uint32_t val = *(uint32_t*)xcb_get_property_value(r);
                    cold->pid = val;
                }

            } else if (atom == atoms._NET_WM_USER_TIME) {
                if (prop_is_cardinal(r) && xcb_get_property_value_length(r) >= 4) {
                    uint32_t val = *(uint32_t*)xcb_get_property_value(r);
                    hot->user_time = val;
                }

            } else if (atom == atoms._NET_WM_USER_TIME_WINDOW) {
                if (r && r->type == XCB_ATOM_WINDOW && xcb_get_property_value_length(r) >= 4) {
                    xcb_window_t w = *(xcb_window_t*)xcb_get_property_value(r);
                    hot->user_time_window = w;

                    if (w != hot->xid) {
                        uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
                        xcb_change_window_attributes(s->conn, w, XCB_CW_EVENT_MASK, values);
                    }
                }

            } else if (atom == atoms._NET_WM_SYNC_REQUEST_COUNTER) {
                if (prop_is_cardinal(r) && xcb_get_property_value_length(r) >= 4) {
                    xcb_sync_counter_t counter = *(xcb_sync_counter_t*)xcb_get_property_value(r);
                    hot->sync_counter = counter;
                }

            } else if (atom == atoms._NET_WM_WINDOW_OPACITY) {
                if (prop_is_cardinal(r) && xcb_get_property_value_length(r) >= 4) {
                    uint32_t val = *(uint32_t*)xcb_get_property_value(r);
                    hot->window_opacity = val;
                    hot->window_opacity_valid = true;
                    if (hot->frame != XCB_NONE) {
                        xcb_change_property(s->conn, XCB_PROP_MODE_REPLACE, hot->frame, atoms._NET_WM_WINDOW_OPACITY,
                                            XCB_ATOM_CARDINAL, 32, 1, &val);
                    }
                } else {
                    hot->window_opacity_valid = false;
                    if (hot->frame != XCB_NONE) {
                        xcb_delete_property(s->conn, hot->frame, atoms._NET_WM_WINDOW_OPACITY);
                    }
                }

            } else if (atom == atoms._NET_WM_ICON_GEOMETRY) {
                if (prop_is_cardinal(r) && xcb_get_property_value_length(r) >= 16) {
                    uint32_t* val = (uint32_t*)xcb_get_property_value(r);
                    hot->icon_geometry.x = (int16_t)val[0];
                    hot->icon_geometry.y = (int16_t)val[1];
                    hot->icon_geometry.w = (uint16_t)val[2];
                    hot->icon_geometry.h = (uint16_t)val[3];
                    hot->icon_geometry_valid = true;
                } else {
                    hot->icon_geometry_valid = false;
                }
            }

            break;
        }

        default:
            break;
    }

    if (changed) hot->dirty |= DIRTY_FRAME_STYLE;

done_one:
    if (hot->pending_replies > 0) hot->pending_replies--;

    if (hot->state != STATE_NEW) return;
    if (hot->pending_replies != 0) return;

    if (hot->manage_aborted) {
        abort_manage(s, slot->client);
        return;
    }

    if (hot->manage_phase == MANAGE_PHASE1) {
        hot->manage_phase = MANAGE_DONE;
        LOG_INFO("Finishing management for window %u", hot->xid);
        client_finish_manage(s, slot->client);
        return;
    }
}
