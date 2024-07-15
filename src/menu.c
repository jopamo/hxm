#include "menu.h"

#include <X11/keysym.h>
#include <errno.h>
#include <librsvg/rsvg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb_icccm.h>

#include "bbox.h"
#include "client.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

#define MENU_PADDING 4
#define MENU_ITEM_HEIGHT 24
#define MENU_WIDTH 240

static void menu_clear_items(server_t* s) {
    for (size_t i = 0; i < s->menu.items.length; i++) {
        menu_item_t* item = s->menu.items.items[i];
        if (item->label) free(item->label);
        if (item->cmd) free(item->cmd);
        if (item->icon_path) free(item->icon_path);
        if (item->icon_surface) cairo_surface_destroy(item->icon_surface);
        free(item);
    }
    small_vec_clear(&s->menu.items);
}

static cairo_surface_t* menu_load_icon(const char* path) {
    if (!path || path[0] == '\0') return NULL;

    if (strstr(path, ".svg")) {
        GError* error = NULL;
        RsvgHandle* handle = rsvg_handle_new_from_file(path, &error);
        if (!handle) {
            if (error) {
                LOG_WARN("Failed to load SVG icon %s: %s", path, error->message);
                g_error_free(error);
            }
            return NULL;
        }

        RsvgRectangle viewport = {0, 0, 16, 16};
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
        cairo_t* cr = cairo_create(surface);
        rsvg_handle_render_document(handle, cr, &viewport, &error);
        if (error) {
            LOG_WARN("Failed to render SVG icon %s: %s", path, error->message);
            g_error_free(error);
        }
        cairo_destroy(cr);
        g_object_unref(handle);
        return surface;
    } else {
        cairo_surface_t* surface = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surface);
            return NULL;
        }
        return surface;
    }
}

static void menu_add_item(server_t* s, const char* label, menu_action_t action, const char* cmd, handle_t client,
                          const char* icon_path) {
    menu_item_t* item = malloc(sizeof(menu_item_t));
    item->label = label ? strdup(label) : NULL;
    item->action = action;
    item->cmd = cmd ? strdup(cmd) : NULL;
    item->client = client;
    item->icon_path = icon_path ? strdup(icon_path) : NULL;
    item->icon_surface = menu_load_icon(icon_path);
    small_vec_push(&s->menu.items, item);

    // Resize menu height
    s->menu.h = s->menu.items.length * MENU_ITEM_HEIGHT + 2 * MENU_PADDING;
}

void menu_init(server_t* s) {
    s->menu.visible = false;
    s->menu.is_client_list = false;
    s->menu.selected_index = -1;
    s->menu.item_height = MENU_ITEM_HEIGHT;
    s->menu.w = MENU_WIDTH;
    s->menu.h = 2 * MENU_PADDING;
    small_vec_init(&s->menu.items);
    render_init(&s->menu.render_ctx);

    // Create window (Override Redirect)
    s->menu.window = xcb_generate_id(s->conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[] = {s->config.theme.menu_items.color, 1,
                         XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                             XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_KEY_PRESS};

    xcb_create_window(s->conn, XCB_COPY_FROM_PARENT, s->menu.window, s->root, 0, 0, s->menu.w, s->menu.h, 1,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, mask, values);
}

void menu_destroy(server_t* s) {
    xcb_destroy_window(s->conn, s->menu.window);
    render_free(&s->menu.render_ctx);

    menu_clear_items(s);
    small_vec_destroy(&s->menu.items);
}

static void menu_populate_root(server_t* s) {
    menu_clear_items(s);
    menu_add_item(s, "firefox-nightly-bin", MENU_ACTION_EXEC, "firefox-nightly-bin", HANDLE_INVALID,
                  "/usr/share/pixmaps/firefox.svg");
    menu_add_item(s, "brave-nightly-bin", MENU_ACTION_EXEC, "brave-nightly-bin", HANDLE_INVALID,
                  "/usr/share/pixmaps/brave.svg");
    menu_add_item(s, "google-chrome-unstable", MENU_ACTION_EXEC, "google-chrome-unstable", HANDLE_INVALID,
                  "/usr/share/pixmaps/chrome.svg");
    menu_add_item(s, "chromium", MENU_ACTION_EXEC, "chromium", HANDLE_INVALID, "/usr/share/pixmaps/chromium.png");
    menu_add_item(s, "pcmanfm-qt", MENU_ACTION_EXEC, "pcmanfm-qt", HANDLE_INVALID,
                  "/usr/share/icons/hicolor/scalable/apps/pcmanfm-qt.svg");
    menu_add_item(s, "keepassxc", MENU_ACTION_EXEC, "env QT_STYLE_OVERRIDE=Adwaita-Dark keepassxc", HANDLE_INVALID,
                  "/usr/share/icons/hicolor/256x256/apps/keepassxc.png");
    menu_add_item(s, "wireshark", MENU_ACTION_EXEC, "env QT_STYLE_OVERRIDE=Adwaita-Dark wireshark", HANDLE_INVALID,
                  "/usr/share/icons/hicolor/128x128/apps/org.wireshark.Wireshark.png");
    menu_add_item(s, "qalculate-gtk", MENU_ACTION_EXEC, "qalculate-gtk", HANDLE_INVALID,
                  "/usr/share/icons/hicolor/128x128/apps/qalculate.png");
    menu_add_item(s, "qbittorrent", MENU_ACTION_EXEC, "qbittorrent", HANDLE_INVALID,
                  "/usr/share/icons/hicolor/128x128/apps/qbittorrent.png");
    menu_add_item(s, "flameshot", MENU_ACTION_EXEC, "flameshot", HANDLE_INVALID,
                  "/usr/share/icons/hicolor/48x48/apps/flameshot.png");
    menu_add_item(s, "texxy", MENU_ACTION_EXEC, "texxy", HANDLE_INVALID,
                  "/usr/share/icons/hicolor/scalable/apps/texxy.svg");
    menu_add_item(s, "1term", MENU_ACTION_EXEC, "1term", HANDLE_INVALID,
                  "/usr/share/icons/hicolor/256x256/apps/1term.png");
    menu_add_item(s, "libreoffice-bin", MENU_ACTION_EXEC, "libreoffice-bin", HANDLE_INVALID,
                  "/usr/share/icons/hicolor/scalable/apps/libreoffice-main.svg");
    menu_add_item(s, "st", MENU_ACTION_EXEC, "st", HANDLE_INVALID, "/usr/share/icons/hicolor/256x256/apps/1term.png");

    menu_add_item(s, NULL, MENU_ACTION_SEPARATOR, NULL, HANDLE_INVALID, NULL);

    // Preferences (flattened for now)
    menu_add_item(s, "Preferences > Reconfigure", MENU_ACTION_RELOAD, NULL, HANDLE_INVALID, NULL);
    menu_add_item(s, "Preferences > Restart", MENU_ACTION_RESTART, NULL, HANDLE_INVALID, NULL);

    menu_add_item(s, NULL, MENU_ACTION_SEPARATOR, NULL, HANDLE_INVALID, NULL);

    // Monitor Control (flattened for now)
    menu_add_item(s, "Monitor > Disable eDP-1", MENU_ACTION_EXEC, "xrandr --output eDP-1 --off", HANDLE_INVALID, NULL);
    menu_add_item(s, "Monitor > Enable eDP-1", MENU_ACTION_EXEC, "xrandr --output eDP-1 --auto", HANDLE_INVALID, NULL);
    menu_add_item(s, "Monitor > Disable DP-1", MENU_ACTION_EXEC, "xrandr --output DP-1 --off", HANDLE_INVALID, NULL);
    menu_add_item(s, "Monitor > Enable DP-1", MENU_ACTION_EXEC, "xrandr --output DP-1 --auto", HANDLE_INVALID, NULL);

    menu_add_item(s, NULL, MENU_ACTION_SEPARATOR, NULL, HANDLE_INVALID, NULL);

    menu_add_item(s, "Exit", MENU_ACTION_EXEC, "bbox --exit", HANDLE_INVALID, NULL);
}

void menu_show(server_t* s, int16_t x, int16_t y) {
    if (s->menu.visible) {
        menu_hide(s);
        return;
    }

    s->menu.is_client_list = false;
    menu_populate_root(s);

    uint32_t values_h[] = {s->menu.h};
    xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_HEIGHT, values_h);

    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
    if (x + s->menu.w > screen->width_in_pixels) x = screen->width_in_pixels - s->menu.w;
    if (y + s->menu.h > screen->height_in_pixels) y = screen->height_in_pixels - s->menu.h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    s->menu.x = x;
    s->menu.y = y;
    s->menu.visible = true;
    s->menu.selected_index = -1;

    uint32_t values[] = {(uint32_t)x, (uint32_t)y};
    xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);

    xcb_map_window(s->conn, s->menu.window);

    xcb_grab_pointer(s->conn, 0, s->root,
                     XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_grab_keyboard(s->conn, 0, s->root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

    menu_handle_expose(s);
}

void menu_show_client_list(server_t* s, int16_t x, int16_t y) {
    if (s->menu.visible) {
        menu_hide(s);
        return;
    }

    s->menu.is_client_list = true;
    menu_clear_items(s);

    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (!s->clients.hdr[i].live) continue;
        handle_t h = handle_make(i, s->clients.hdr[i].gen);
        client_cold_t* cold = server_ccold(s, h);
        client_hot_t* hot = server_chot(s, h);
        if (!cold || !hot) continue;

        char label[256];
        if (hot->state == STATE_UNMAPPED) {
            snprintf(label, sizeof(label), "[%s]", cold->title ? cold->title : "Unnamed");
        } else {
            snprintf(label, sizeof(label), "%s", cold->title ? cold->title : "Unnamed");
        }
        menu_add_item(s, label, MENU_ACTION_RESTORE, NULL, h, NULL);
    }

    if (s->menu.items.length == 0) {
        menu_add_item(s, "(No windows)", MENU_ACTION_NONE, NULL, HANDLE_INVALID, NULL);
    }

    uint32_t values_h[] = {s->menu.h};
    xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_HEIGHT, values_h);

    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
    if (x + s->menu.w > screen->width_in_pixels) x = screen->width_in_pixels - s->menu.w;
    if (y + s->menu.h > screen->height_in_pixels) y = screen->height_in_pixels - s->menu.h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    s->menu.x = x;
    s->menu.y = y;
    s->menu.visible = true;
    s->menu.selected_index = -1;

    uint32_t values[] = {(uint32_t)x, (uint32_t)y};
    xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);

    xcb_map_window(s->conn, s->menu.window);

    xcb_grab_pointer(s->conn, 0, s->root,
                     XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_grab_keyboard(s->conn, 0, s->root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

    menu_handle_expose(s);
}

void menu_hide(server_t* s) {
    if (!s->menu.visible) return;
    s->menu.visible = false;
    xcb_unmap_window(s->conn, s->menu.window);
    xcb_ungrab_pointer(s->conn, XCB_CURRENT_TIME);
    xcb_ungrab_keyboard(s->conn, XCB_CURRENT_TIME);
}

static rgba_t u32_to_rgba(uint32_t c) {
    return (rgba_t){((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, 1.0};
}

void menu_handle_expose(server_t* s) { menu_handle_expose_region(s, NULL); }

void menu_handle_expose_region(server_t* s, const dirty_region_t* dirty) {
    if (s->menu.w == 0 || s->menu.h == 0) return;

    cairo_surface_t* target_surface = NULL;
    xcb_pixmap_t pixmap = XCB_NONE;

    if (s->is_test) {
        target_surface = cairo_image_surface_create((s->root_depth == 32) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
                                                    s->menu.w, s->menu.h);
    } else {
        pixmap = xcb_generate_id(s->conn);
        xcb_create_pixmap(s->conn, s->root_depth, pixmap, s->menu.window, s->menu.w, s->menu.h);
        target_surface = cairo_xcb_surface_create(s->conn, pixmap, s->root_visual_type, s->menu.w, s->menu.h);
    }

    cairo_t* cr = cairo_create(target_surface);
    if (dirty && dirty->valid) {
        dirty_region_t clip = *dirty;
        dirty_region_clamp(&clip, 0, 0, s->menu.w, s->menu.h);
        if (!clip.valid) {
            cairo_destroy(cr);
            if (s->is_test) {
                cairo_surface_destroy(target_surface);
            } else {
                cairo_surface_destroy(target_surface);
                xcb_free_pixmap(s->conn, pixmap);
            }
            return;
        }
        cairo_rectangle(cr, clip.x, clip.y, clip.w, clip.h);
        cairo_clip(cr);
    }

    rgba_t bg = u32_to_rgba(s->config.theme.menu_items.color);
    cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
    cairo_paint(cr);

    if (!s->menu.render_ctx.layout) {
        s->menu.render_ctx.layout = pango_cairo_create_layout(cr);
        PangoFontDescription* desc = pango_font_description_from_string("Sans 10");
        pango_layout_set_font_description(s->menu.render_ctx.layout, desc);
        pango_font_description_free(desc);
    } else {
        pango_cairo_update_layout(cr, s->menu.render_ctx.layout);
    }

    rgba_t fg = u32_to_rgba(s->config.theme.menu_items_text_color);
    rgba_t sel_bg = u32_to_rgba(s->config.theme.menu_items_active.color);
    rgba_t sel_fg = u32_to_rgba(s->config.theme.menu_items_active_text_color);

    for (size_t i = 0; i < s->menu.items.length; i++) {
        menu_item_t* item = s->menu.items.items[i];
        int16_t item_y = MENU_PADDING + i * MENU_ITEM_HEIGHT;

        if (item->action == MENU_ACTION_SEPARATOR) {
            cairo_set_source_rgba(cr, fg.r, fg.g, fg.b, 0.3);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, MENU_PADDING, item_y + MENU_ITEM_HEIGHT / 2.0);
            cairo_line_to(cr, s->menu.w - MENU_PADDING, item_y + MENU_ITEM_HEIGHT / 2.0);
            cairo_stroke(cr);
            continue;
        }

        bool selected = ((int)i == s->menu.selected_index);

        if (selected) {
            cairo_set_source_rgba(cr, sel_bg.r, sel_bg.g, sel_bg.b, sel_bg.a);
            cairo_rectangle(cr, 0, item_y, s->menu.w, MENU_ITEM_HEIGHT);
            cairo_fill(cr);
        }

        int text_x_offset = MENU_PADDING * 2;

        cairo_surface_t* icon = NULL;
        if (s->menu.is_client_list && item->client != HANDLE_INVALID) {
            client_hot_t* hot = server_chot(s, item->client);
            if (hot) icon = hot->icon_surface;
        } else {
            icon = item->icon_surface;
        }

        if (icon) {
            int icon_w = cairo_image_surface_get_width(icon);
            int icon_h = cairo_image_surface_get_height(icon);
            double target_size = MENU_ITEM_HEIGHT - 6;
            double scale = target_size / ((icon_w > icon_h) ? icon_w : icon_h);
            double draw_w = icon_w * scale;
            double draw_h = icon_h * scale;
            double icon_y = item_y + (MENU_ITEM_HEIGHT - draw_h) / 2.0;
            cairo_save(cr);
            cairo_translate(cr, text_x_offset, icon_y);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, icon, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            text_x_offset += (int)draw_w + 6;
        }

        rgba_t text_color = selected ? sel_fg : fg;
        cairo_set_source_rgba(cr, text_color.r, text_color.g, text_color.b, text_color.a);
        pango_layout_set_text(s->menu.render_ctx.layout, item->label ? item->label : "", -1);
        pango_layout_set_width(s->menu.render_ctx.layout, (s->menu.w - text_x_offset - MENU_PADDING) * PANGO_SCALE);
        pango_layout_set_ellipsize(s->menu.render_ctx.layout, PANGO_ELLIPSIZE_END);
        int text_h;
        pango_layout_get_pixel_size(s->menu.render_ctx.layout, NULL, &text_h);
        double text_y = item_y + (MENU_ITEM_HEIGHT - text_h) / 2.0;
        cairo_move_to(cr, text_x_offset, text_y);
        pango_cairo_show_layout(cr, s->menu.render_ctx.layout);
    }

    cairo_destroy(cr);

    if (s->is_test) {
        cairo_surface_flush(target_surface);
        xcb_gcontext_t gc = xcb_generate_id(s->conn);
        xcb_create_gc(s->conn, gc, s->menu.window, 0, NULL);
        xcb_put_image(s->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, s->menu.window, gc, (uint16_t)s->menu.w, (uint16_t)s->menu.h,
                      0, 0, 0, (uint8_t)s->root_depth,
                      (uint32_t)(cairo_image_surface_get_stride(target_surface) * s->menu.h),
                      cairo_image_surface_get_data(target_surface));
        xcb_free_gc(s->conn, gc);
        cairo_surface_destroy(target_surface);
    } else {
        cairo_surface_destroy(target_surface);
        xcb_gcontext_t gc = xcb_generate_id(s->conn);
        xcb_create_gc(s->conn, gc, s->menu.window, 0, NULL);
        xcb_copy_area(s->conn, pixmap, s->menu.window, gc, 0, 0, 0, 0, s->menu.w, s->menu.h);
        xcb_free_gc(s->conn, gc);
        xcb_free_pixmap(s->conn, pixmap);
    }
}

void menu_handle_pointer_motion(server_t* s, int16_t x, int16_t y) {
    int16_t local_x = x - s->menu.x;
    int16_t local_y = y - s->menu.y;

    if (local_x < 0 || local_x > s->menu.w || local_y < 0 || local_y > s->menu.h) {
        if (s->menu.selected_index != -1) {
            s->menu.selected_index = -1;
            menu_handle_expose(s);
        }
        return;
    }

    int32_t index = (local_y - MENU_PADDING) / MENU_ITEM_HEIGHT;
    if (index < 0 || index >= (int32_t)s->menu.items.length) {
        index = -1;
    } else {
        menu_item_t* item = s->menu.items.items[index];
        if (item->action == MENU_ACTION_SEPARATOR) index = -1;
    }

    if (index != s->menu.selected_index) {
        s->menu.selected_index = index;
        menu_handle_expose(s);
    }
}

static void spawn(const char* cmd) {
    pid_t p = fork();
    if (p < 0) return;

    if (p == 0) {
        pid_t p2 = fork();
        if (p2 < 0) _exit(1);
        if (p2 == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
            _exit(1);
        }
        _exit(0);
    }

    int status = 0;
    (void)waitpid(p, &status, 0);
}

void menu_handle_button_press(server_t* s, xcb_button_press_event_t* ev) {
    int16_t local_x = ev->root_x - s->menu.x;
    int16_t local_y = ev->root_y - s->menu.y;

    if (local_x < 0 || local_x > s->menu.w || local_y < 0 || local_y > s->menu.h) {
        menu_hide(s);
    }
}

static void menu_activate_selected(server_t* s);

void menu_handle_button_release(server_t* s, xcb_button_release_event_t* ev) {
    int16_t local_x = ev->root_x - s->menu.x;
    int16_t local_y = ev->root_y - s->menu.y;

    if (local_x >= 0 && local_x <= s->menu.w && local_y >= 0 && local_y <= s->menu.h) {
        int32_t index = (local_y - MENU_PADDING) / MENU_ITEM_HEIGHT;
        if (index >= 0 && index < (int32_t)s->menu.items.length) {
            menu_item_t* item = s->menu.items.items[index];
            if (item->action != MENU_ACTION_SEPARATOR) {
                s->menu.selected_index = index;
                menu_activate_selected(s);
            }
        }
    } else {
        menu_hide(s);
    }
}

static int menu_find_next_selectable(server_t* s, int start, int dir) {
    if (s->menu.items.length == 0) return -1;

    int i = start;
    for (size_t step = 0; step < s->menu.items.length; step++) {
        i += dir;
        if (i < 0) i = (int)s->menu.items.length - 1;
        if (i >= (int)s->menu.items.length) i = 0;

        menu_item_t* item = s->menu.items.items[i];
        if (!item) continue;
        if (item->action == MENU_ACTION_SEPARATOR) continue;
        if (item->action == MENU_ACTION_NONE) continue;
        return i;
    }
    return -1;
}

static int menu_find_first_selectable(server_t* s) {
    for (size_t i = 0; i < s->menu.items.length; i++) {
        menu_item_t* item = s->menu.items.items[i];
        if (!item) continue;
        if (item->action == MENU_ACTION_SEPARATOR) continue;
        if (item->action == MENU_ACTION_NONE) continue;
        return (int)i;
    }
    return -1;
}

static void menu_do_restore(server_t* s, handle_t client) {
    if (client == HANDLE_INVALID) return;

    client_hot_t* hot = server_chot(s, client);
    if (!hot) return;

    if (hot->state == STATE_UNMAPPED) {
        wm_client_restore(s, client);
    }

    wm_set_focus(s, client);
    stack_raise(s, client);
}

static void menu_activate_selected(server_t* s) {
    if (!s->menu.visible) return;
    if (s->menu.selected_index < 0 || s->menu.selected_index >= (int)s->menu.items.length) return;

    menu_item_t* item = s->menu.items.items[s->menu.selected_index];
    if (!item) return;
    if (item->action == MENU_ACTION_SEPARATOR) return;

    LOG_INFO("Menu Action: %d", item->action);

    menu_hide(s);

    switch (item->action) {
        case MENU_ACTION_EXEC:
            if (item->cmd) spawn(item->cmd);
            break;
        case MENU_ACTION_EXIT:
            exit(0);
            break;
        case MENU_ACTION_RESTART:
            spawn("bbox --restart");
            break;
        case MENU_ACTION_RELOAD:
            spawn("bbox --reconfigure");
            break;
        case MENU_ACTION_RESTORE:
            menu_do_restore(s, item->client);
            break;
        default:
            break;
    }
}

void menu_handle_key_press(server_t* s, xcb_key_press_event_t* ev) {
    if (!s->menu.visible) return;

    xcb_keysym_t sym = xcb_key_symbols_get_keysym(s->keysyms, ev->detail, 0);

    switch (sym) {
        case XK_Escape:
            menu_hide(s);
            return;

        case XK_Up: {
            int start = s->menu.selected_index;
            if (start < 0) start = menu_find_first_selectable(s);
            int next = menu_find_next_selectable(s, start, -1);
            if (next != s->menu.selected_index) {
                s->menu.selected_index = next;
                menu_handle_expose(s);
            }
            return;
        }

        case XK_Down: {
            int start = s->menu.selected_index;
            if (start < 0) start = menu_find_first_selectable(s);
            int next = menu_find_next_selectable(s, start, +1);
            if (next != s->menu.selected_index) {
                s->menu.selected_index = next;
                menu_handle_expose(s);
            }
            return;
        }

        case XK_Home: {
            int first = menu_find_first_selectable(s);
            if (first != s->menu.selected_index) {
                s->menu.selected_index = first;
                menu_handle_expose(s);
            }
            return;
        }

        case XK_Return:
        case XK_KP_Enter:
        case XK_space:
            if (s->menu.selected_index < 0) {
                s->menu.selected_index = menu_find_first_selectable(s);
            }
            menu_activate_selected(s);
            return;

        default:
            break;
    }
}
