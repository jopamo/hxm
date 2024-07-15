#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "bbox.h"
#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "frame.h"
#include "wm.h"

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

static void client_update_effective_strut(client_hot_t* hot) {
    if (hot->strut_partial_active) {
        hot->strut = hot->strut_partial;
    } else if (hot->strut_full_active) {
        hot->strut = hot->strut_full;
    } else {
        memset(&hot->strut, 0, sizeof(hot->strut));
    }
}

static void abort_manage(server_t* s, handle_t h) {
    client_hot_t* hot = server_chot(s, h);
    client_cold_t* cold = server_ccold(s, h);
    if (!hot || !cold) return;

    // Drop xid->handle mapping so later events don't resolve a dead handle
    if (hot->xid != XCB_NONE) {
        hash_map_remove(&s->window_to_client, hot->xid);
    }

    // Cold allocations
    arena_destroy(&cold->string_arena);
    render_free(&hot->render_ctx);
    if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);

    // Slot free
    slotmap_free(&s->clients, h);
}

void wm_handle_reply(server_t* s, const cookie_slot_t* slot, void* reply, xcb_generic_error_t* err) {
    if (err) {
        LOG_DEBUG("Cookie %u returned error code %d", slot->sequence, err->error_code);
    }
    if (slot->client == HANDLE_INVALID) {
        // Pre-management adoption probe
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
        // NULL replies should not auto-abort by default; just consume the pending count
        goto done_one;
    }

    bool changed = false;

    switch (slot->type) {
        case COOKIE_GET_WINDOW_ATTRIBUTES: {
            xcb_get_window_attributes_reply_t* r = (xcb_get_window_attributes_reply_t*)reply;
            hot->override_redirect = r->override_redirect ? true : false;
            hot->visual_id = r->visual;
            hot->visual_type = xcb_get_visualtype(s->conn, r->visual);
            if (hot->override_redirect && hot->state == STATE_NEW) {
                LOG_DEBUG("Window %u is override_redirect, aborting manage", hot->xid);
                hot->manage_aborted = true;
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

            // Avoid "fixing" legitimately small windows
            // Only clamp truly broken values
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
                int len = 0;
                char* str = prop_get_string(r, &len);
                if (str) {
                    char* instance = str;
                    char* class = (strlen(instance) + 1 < (size_t)len) ? instance + strlen(instance) + 1 : instance;

                    if (!cold->wm_instance || strcmp(cold->wm_instance, instance) != 0 || !cold->wm_class ||
                        strcmp(cold->wm_class, class) != 0) {
                        cold->wm_instance = arena_strndup(&cold->string_arena, instance, strlen(instance));
                        cold->wm_class = arena_strndup(&cold->string_arena, class, strlen(class));
                        LOG_DEBUG("Window %u WM_CLASS: %s, %s", hot->xid, cold->wm_instance, cold->wm_class);
                    }
                }
            } else if (atom == atoms._NET_WM_NAME) {
                int len = 0;
                char* str = prop_get_string(r, &len);
                if (!str) len = 0;
                size_t trimmed_len = (len > 0) ? (size_t)len : 0;
                while (trimmed_len > 0 && str[trimmed_len - 1] == '\0') trimmed_len--;

                bool valid = trimmed_len > 0;
                if (valid && memchr(str, '\0', trimmed_len) != NULL) valid = false;
                if (valid && !is_valid_utf8(str, trimmed_len)) valid = false;

                if (!valid) {
                    bool had_net = cold->has_net_wm_name;
                    cold->has_net_wm_name = false;
                    if (had_net) {
                        cold->title = arena_strndup(&cold->string_arena, "", 0);
                        changed = true;
                    }

                    uint32_t c =
                        xcb_get_property(s->conn, 0, hot->xid, atoms.WM_NAME, XCB_ATOM_STRING, 0, 1024).sequence;
                    cookie_jar_push(&s->cookie_jar, c, COOKIE_GET_PROPERTY, slot->client,
                                    ((uint64_t)hot->xid << 32) | atoms.WM_NAME, wm_handle_reply);
                } else {
                    if (!cold->has_net_wm_name || !cold->title || strlen(cold->title) != trimmed_len ||
                        strncmp(cold->title, str, trimmed_len) != 0) {
                        cold->title = arena_strndup(&cold->string_arena, str, trimmed_len);
                        cold->has_net_wm_name = true;
                        LOG_DEBUG("Window %u _NET_WM_NAME: %s", hot->xid, cold->title);
                        changed = true;
                    }
                }
            } else if (atom == atoms.WM_NAME) {
                int len = 0;
                char* str = prop_get_string(r, &len);
                if (str && !cold->has_net_wm_name) {
                    if (!cold->title || strlen(cold->title) != (size_t)len || strncmp(cold->title, str, len) != 0) {
                        cold->title = arena_strndup(&cold->string_arena, str, len);
                        LOG_DEBUG("Window %u WM_NAME: %s", hot->xid, cold->title);
                        changed = true;
                    }
                }
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
                            }
                        }
                        wm_client_apply_state_set(s, slot->client, &set);
                    }
                }
            } else if (atom == atoms.WM_NORMAL_HINTS) {
                xcb_size_hints_t hints;
                if (xcb_icccm_get_wm_size_hints_from_reply(&hints, r)) {
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
                    LOG_DEBUG("Window %u hints: min %dx%d max %dx%d inc %dx%d", hot->xid, hot->hints.min_w,
                              hot->hints.min_h, hot->hints.max_w, hot->hints.max_h, hot->hints.inc_w, hot->hints.inc_h);
                }
            } else if (atom == atoms.WM_TRANSIENT_FOR) {
                if (xcb_get_property_value_length(r) >= 4) {
                    xcb_window_t transient_for_xid = *(xcb_window_t*)xcb_get_property_value(r);
                    cold->transient_for_xid = transient_for_xid;
                    hot->transient_for = server_get_client_by_window(s, transient_for_xid);
                    if (hot->transient_for != HANDLE_INVALID) {
                        client_hot_t* parent = server_chot(s, hot->transient_for);
                        if (parent) {
                            // Remove from old parent if any (e.g. property changed)
                            if (hot->transient_sibling.next && hot->transient_sibling.next != &hot->transient_sibling) {
                                list_remove(&hot->transient_sibling);
                            }
                            list_insert(&hot->transient_sibling, parent->transients_head.prev,
                                        &parent->transients_head);
                        }
                    }
                    LOG_DEBUG("Window %u is transient for %u (handle %lx)", hot->xid, transient_for_xid,
                              hot->transient_for);
                }
            } else if (atom == atoms._NET_WM_WINDOW_TYPE) {
                if (xcb_get_property_value_length(r) > 0) {
                    xcb_atom_t* types = (xcb_atom_t*)xcb_get_property_value(r);
                    int num_types = xcb_get_property_value_length(r) / sizeof(xcb_atom_t);
                    for (int i = 0; i < num_types; i++) {
                        if (types[i] == atoms._NET_WM_WINDOW_TYPE_DOCK) {
                            hot->type = WINDOW_TYPE_DOCK;
                            hot->base_layer = LAYER_ABOVE;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_NOTIFICATION) {
                            hot->type = WINDOW_TYPE_NOTIFICATION;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_DIALOG) {
                            hot->type = WINDOW_TYPE_DIALOG;
                            hot->base_layer = LAYER_NORMAL;
                            hot->placement = PLACEMENT_CENTER;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_DESKTOP) {
                            hot->type = WINDOW_TYPE_DESKTOP;
                            hot->base_layer = LAYER_DESKTOP;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_SPLASH) {
                            hot->type = WINDOW_TYPE_SPLASH;
                            hot->base_layer = LAYER_ABOVE;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_TOOLBAR) {
                            hot->type = WINDOW_TYPE_TOOLBAR;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_UTILITY) {
                            hot->type = WINDOW_TYPE_UTILITY;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_MENU) {
                            hot->type = WINDOW_TYPE_MENU;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_DROPDOWN_MENU) {
                            hot->type = WINDOW_TYPE_DROPDOWN_MENU;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_POPUP_MENU) {
                            hot->type = WINDOW_TYPE_POPUP_MENU;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_TOOLTIP) {
                            hot->type = WINDOW_TYPE_TOOLTIP;
                            hot->base_layer = LAYER_OVERLAY;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_NORMAL) {
                            hot->type = WINDOW_TYPE_NORMAL;
                            break;
                        }
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
                            LOG_DEBUG("Window %u supports WM_DELETE_WINDOW", hot->xid);
                        } else if (protocols[i] == atoms.WM_TAKE_FOCUS) {
                            cold->protocols |= PROTOCOL_TAKE_FOCUS;
                            LOG_DEBUG("Window %u supports WM_TAKE_FOCUS", hot->xid);
                        }
                    }
                }
            } else if (atom == atoms._NET_WM_DESKTOP) {
                uint32_t* val = prop_get_u32_array(r, 1, NULL);
                if (val) {
                    if (*val == 0xFFFFFFFF) {
                        hot->sticky = true;
                        hot->desktop = -1;
                    } else {
                        hot->sticky = false;
                        uint32_t desk = *val;
                        if (desk >= s->desktop_count) {
                            desk = s->current_desktop;
                        }
                        hot->desktop = (int32_t)desk;
                    }
                    LOG_DEBUG("Window %u requested desktop %d (sticky=%d)", hot->xid, hot->desktop, hot->sticky);
                }
            } else if (atom == atoms._NET_WM_STRUT || atom == atoms._NET_WM_STRUT_PARTIAL) {
                int len = r ? xcb_get_property_value_length(r) : 0;
                bool is_partial = (atom == atoms._NET_WM_STRUT_PARTIAL);
                strut_t* target = is_partial ? &hot->strut_partial : &hot->strut_full;
                bool* active = is_partial ? &hot->strut_partial_active : &hot->strut_full_active;

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

                client_update_effective_strut(hot);
                s->root_dirty |= ROOT_DIRTY_WORKAREA;
                LOG_DEBUG("Window %u struts: L=%u R=%u T=%u B=%u", hot->xid, hot->strut.left, hot->strut.right,
                          hot->strut.top, hot->strut.bottom);
            } else if (atom == atoms.WM_HINTS) {
                if (!prop_is_empty(r)) {
                    xcb_icccm_wm_hints_t hints;
                    if (xcb_icccm_get_wm_hints_from_reply(&hints, r)) {
                        cold->can_focus = (bool)(hints.input);
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
                        LOG_DEBUG("Window %u WM_HINTS input: %d", hot->xid, cold->can_focus);
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
                                // X11 icon data is ARGB 32-bit words.
                                // Cairo ARGB32 is also ARGB but host-endian.
                                // Usually these match on little-endian systems.
                                row[x] = best_data[y * (int)best_w + x];
                            }
                        }
                        cairo_surface_mark_dirty(hot->icon_surface);
                        changed = true;
                    }
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

    if (hot->state == STATE_NEW && hot->pending_replies == 0) {
        if (hot->manage_aborted) {
            abort_manage(s, slot->client);
            return;
        }

        // Finish exactly once
        LOG_INFO("Finishing management for window %u", hot->xid);
        client_finish_manage(s, slot->client);
    }
}
