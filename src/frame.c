#include "frame.h"

#include <X11/cursorfont.h>
#include <stdlib.h>
#include <string.h>

#include "bbox.h"
#include "client.h"
#include "event.h"
#include "render.h"

void frame_init_resources(server_t* s) {
    // Cursors
    xcb_font_t cursor_font = xcb_generate_id(s->conn);
    xcb_open_font(s->conn, cursor_font, strlen("cursor"), "cursor");

    struct {
        xcb_cursor_t* cursor;
        uint16_t id;
    } cursors[] = {
        {&s->cursor_left_ptr, XC_left_ptr},
        {&s->cursor_move, XC_fleur},
        {&s->cursor_resize_top, XC_top_side},
        {&s->cursor_resize_bottom, XC_bottom_side},
        {&s->cursor_resize_left, XC_left_side},
        {&s->cursor_resize_right, XC_right_side},
        {&s->cursor_resize_top_left, XC_top_left_corner},
        {&s->cursor_resize_top_right, XC_top_right_corner},
        {&s->cursor_resize_bottom_left, XC_bottom_left_corner},
        {&s->cursor_resize_bottom_right, XC_bottom_right_corner},
    };

    for (size_t i = 0; i < sizeof(cursors) / sizeof(cursors[0]); i++) {
        *cursors[i].cursor = xcb_generate_id(s->conn);
        xcb_create_glyph_cursor(s->conn, *cursors[i].cursor, cursor_font, cursor_font, cursors[i].id, cursors[i].id + 1,
                                0, 0, 0, 0xffff, 0xffff, 0xffff);
    }

    xcb_close_font(s->conn, cursor_font);
}

void frame_cleanup_resources(server_t* s) {
    xcb_free_cursor(s->conn, s->cursor_left_ptr);
    xcb_free_cursor(s->conn, s->cursor_move);
    xcb_free_cursor(s->conn, s->cursor_resize_top);
    xcb_free_cursor(s->conn, s->cursor_resize_bottom);
    xcb_free_cursor(s->conn, s->cursor_resize_left);
    xcb_free_cursor(s->conn, s->cursor_resize_right);
    xcb_free_cursor(s->conn, s->cursor_resize_top_left);
    xcb_free_cursor(s->conn, s->cursor_resize_top_right);
    xcb_free_cursor(s->conn, s->cursor_resize_bottom_left);
    xcb_free_cursor(s->conn, s->cursor_resize_bottom_right);
}

#define BUTTON_WIDTH 16
#define BUTTON_HEIGHT 16
#define BUTTON_PADDING 4

static xcb_rectangle_t get_button_rect(server_t* s, client_hot_t* hot, frame_button_t btn) {
    uint16_t frame_w = hot->server.w + 2 * s->config.theme.border_width;
    int16_t x = (int16_t)frame_w - BUTTON_PADDING - BUTTON_WIDTH;
    int16_t y = (int16_t)((s->config.theme.title_height - BUTTON_HEIGHT) / 2);

    if (btn == FRAME_BUTTON_MAXIMIZE) x -= (BUTTON_WIDTH + BUTTON_PADDING);
    if (btn == FRAME_BUTTON_MINIMIZE) x -= 2 * (BUTTON_WIDTH + BUTTON_PADDING);

    return (xcb_rectangle_t){x, y, BUTTON_WIDTH, BUTTON_HEIGHT};
}

void frame_redraw(server_t* s, handle_t h, uint32_t what) {
    (void)what;  // Currently we always redraw all with the new engine
    client_hot_t* hot = server_chot(s, h);
    client_cold_t* cold = server_ccold(s, h);
    if (!hot) return;

    if (hot->flags & CLIENT_FLAG_UNDECORATED) return;

    bool active = (hot->flags & CLIENT_FLAG_FOCUSED);

    uint16_t frame_w = hot->server.w + 2 * s->config.theme.border_width;
    uint16_t frame_h = hot->server.h + s->config.theme.title_height + s->config.theme.border_width;

    // Call the Golden Path Engine
    xcb_visualtype_t* visual = hot->visual_type;
    if (!visual) {
        if (hot->depth == s->root_depth) {
            visual = s->root_visual_type;
        } else {
            // Try to find ANY visual for this depth
            xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(s->conn));
            for (; screen_iter.rem && !visual; xcb_screen_next(&screen_iter)) {
                xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen_iter.data);
                for (; depth_iter.rem && !visual; xcb_depth_next(&depth_iter)) {
                    if (depth_iter.data->depth == hot->depth) {
                        xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
                        if (visual_iter.rem) {
                            visual = visual_iter.data;
                        }
                    }
                }
            }
        }
    }

    if (!visual) {
        LOG_WARN("No visual found for client %u (depth %u), skipping redraw", hot->xid, hot->depth);
        return;
    }

    render_frame(s->conn, hot->frame, visual, &hot->render_ctx, (int)hot->depth, s->is_test, cold ? cold->title : "",
                 active, frame_w, frame_h, &s->config.theme, hot->icon_surface);
}

frame_button_t frame_get_button_at(server_t* s, handle_t h, int16_t x, int16_t y) {
    client_hot_t* hot = server_chot(s, h);
    if (!hot || (hot->flags & CLIENT_FLAG_UNDECORATED)) return FRAME_BUTTON_NONE;

    frame_button_t btns[] = {FRAME_BUTTON_CLOSE, FRAME_BUTTON_MAXIMIZE, FRAME_BUTTON_MINIMIZE};
    for (int i = 0; i < 3; i++) {
        xcb_rectangle_t br = get_button_rect(s, hot, btns[i]);
        if (x >= br.x && x < br.x + br.width && y >= br.y && y < br.y + br.height) {
            return btns[i];
        }
    }

    return FRAME_BUTTON_NONE;
}
