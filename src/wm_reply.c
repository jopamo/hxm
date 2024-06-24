#include <stdlib.h>
#include <string.h>
#include <xcb/xcb_icccm.h>

#include "bbox.h"
#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "frame.h"
#include "wm.h"

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

void wm_handle_reply(server_t* s, cookie_slot_t* slot, void* reply) {
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
                if (xcb_get_property_value_length(r) > 0) {
                    int len = xcb_get_property_value_length(r);
                    char* str = (char*)xcb_get_property_value(r);
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
                if (xcb_get_property_value_length(r) > 0) {
                    int len = xcb_get_property_value_length(r);
                    char* str = (char*)xcb_get_property_value(r);
                    if (!cold->has_net_wm_name || !cold->title || strlen(cold->title) != (size_t)len ||
                        strncmp(cold->title, str, len) != 0) {
                        cold->title = arena_strndup(&cold->string_arena, str, len);
                        cold->has_net_wm_name = true;
                        LOG_DEBUG("Window %u _NET_WM_NAME: %s", hot->xid, cold->title);
                        changed = true;
                    }
                }
            } else if (atom == atoms.WM_NAME) {
                if (xcb_get_property_value_length(r) > 0 && !cold->has_net_wm_name) {
                    int len = xcb_get_property_value_length(r);
                    char* str = (char*)xcb_get_property_value(r);
                    if (!cold->title || strlen(cold->title) != (size_t)len || strncmp(cold->title, str, len) != 0) {
                        cold->title = arena_strndup(&cold->string_arena, str, len);
                        LOG_DEBUG("Window %u WM_NAME: %s", hot->xid, cold->title);
                        changed = true;
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
                            hot->layer = LAYER_ABOVE;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_NOTIFICATION) {
                            hot->type = WINDOW_TYPE_NOTIFICATION;
                            hot->layer = LAYER_OVERLAY;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_DIALOG) {
                            hot->type = WINDOW_TYPE_DIALOG;
                            hot->layer = LAYER_NORMAL;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_DESKTOP) {
                            hot->type = WINDOW_TYPE_DESKTOP;
                            hot->layer = LAYER_DESKTOP;
                            hot->flags |= CLIENT_FLAG_UNDECORATED;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_SPLASH) {
                            hot->type = WINDOW_TYPE_SPLASH;
                            hot->layer = LAYER_ABOVE;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_TOOLBAR) {
                            hot->type = WINDOW_TYPE_TOOLBAR;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_UTILITY) {
                            hot->type = WINDOW_TYPE_UTILITY;
                            break;
                        } else if (types[i] == atoms._NET_WM_WINDOW_TYPE_NORMAL) {
                            hot->type = WINDOW_TYPE_NORMAL;
                            break;
                        }
                    }
                }
            } else if (atom == atoms.WM_PROTOCOLS) {
                if (xcb_get_property_value_length(r) > 0) {
                    xcb_atom_t* protocols = (xcb_atom_t*)xcb_get_property_value(r);
                    int num_protocols = xcb_get_property_value_length(r) / sizeof(xcb_atom_t);
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
                if (xcb_get_property_value_length(r) >= 4) {
                    uint32_t val = *(uint32_t*)xcb_get_property_value(r);
                    hot->desktop = (int32_t)val;
                    if (val == 0xFFFFFFFF) {
                        hot->sticky = true;
                    } else {
                        hot->sticky = false;
                    }
                    LOG_DEBUG("Window %u requested desktop %d (sticky=%d)", hot->xid, hot->desktop, hot->sticky);
                }
            } else if (atom == atoms._NET_WM_STRUT || atom == atoms._NET_WM_STRUT_PARTIAL) {
                if (xcb_get_property_value_length(r) >= 16) {
                    uint32_t* val = (uint32_t*)xcb_get_property_value(r);
                    hot->strut.left = val[0];
                    hot->strut.right = val[1];
                    hot->strut.top = val[2];
                    hot->strut.bottom = val[3];

                    if (atom == atoms._NET_WM_STRUT_PARTIAL && xcb_get_property_value_length(r) >= 48) {
                        hot->strut.left_start_y = val[4];
                        hot->strut.left_end_y = val[5];
                        hot->strut.right_start_y = val[6];
                        hot->strut.right_end_y = val[7];
                        hot->strut.top_start_x = val[8];
                        hot->strut.top_end_x = val[9];
                        hot->strut.bottom_start_x = val[10];
                        hot->strut.bottom_end_x = val[11];
                    }

                    s->root_dirty |= ROOT_DIRTY_WORKAREA;
                    LOG_DEBUG("Window %u struts: L=%u R=%u T=%u B=%u", hot->xid, hot->strut.left, hot->strut.right,
                              hot->strut.top, hot->strut.bottom);
                }
            } else if (atom == atoms.WM_HINTS) {
                if (xcb_get_property_value_length(r) >= 4) {
                    xcb_icccm_wm_hints_t hints;
                    if (xcb_icccm_get_wm_hints_from_reply(&hints, r)) {
                        cold->can_focus = (bool)(hints.input);
                        LOG_DEBUG("Window %u WM_HINTS input: %d", hot->xid, cold->can_focus);
                    }
                }
            } else if (atom == atoms._NET_WM_ICON) {
                if (xcb_get_property_value_length(r) >= 8) {
                    uint32_t* val = (uint32_t*)xcb_get_property_value(r);
                    int total_words = xcb_get_property_value_length(r) / 4;

                    // Find best icon (closest to 16x16)
                    int best_w = 0, best_h = 0;
                    uint32_t* best_data = NULL;
                    int min_diff = 10000;

                    int i = 0;
                    while (i + 2 <= total_words) {
                        int w = (int)val[i];
                        int h = (int)val[i + 1];
                        int size = w * h;
                        if (i + 2 + size > total_words) break;

                        int diff = abs(w - 16) + abs(h - 16);
                        if (diff < min_diff) {
                            min_diff = diff;
                            best_w = w;
                            best_h = h;
                            best_data = &val[i + 2];
                        }
                        i += 2 + size;
                    }

                    if (best_data) {
                        if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
                        hot->icon_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_w, best_h);

                        unsigned char* dest = cairo_image_surface_get_data(hot->icon_surface);
                        int stride = cairo_image_surface_get_stride(hot->icon_surface);
                        cairo_surface_flush(hot->icon_surface);

                        for (int y = 0; y < best_h; y++) {
                            uint32_t* row = (uint32_t*)(dest + y * stride);
                            for (int x = 0; x < best_w; x++) {
                                // X11 icon data is ARGB 32-bit words.
                                // Cairo ARGB32 is also ARGB but host-endian.
                                // Usually these match on little-endian systems.
                                row[x] = best_data[y * best_w + x];
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
