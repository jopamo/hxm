/* snap_preview.c - Preview window management */

#include "snap_preview.h"

#include <stdlib.h>
#include <xcb/xcb.h>

#include "hxm.h"

void snap_preview_init(server_t* s) {
    if (!s || !s->conn) return;
    if (s->snap_preview_win != XCB_WINDOW_NONE) return;

    s->snap_preview_win = xcb_generate_id(s->conn);

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
    uint32_t values[3];
    values[0] = 0;                                   /* background pixel */
    values[1] = s->snap_preview_color & 0x00FFFFFFu; /* border pixel */
    values[2] = 1;                                   /* override_redirect */

    xcb_create_window(s->conn, s->root_depth, s->snap_preview_win, s->root, 0, 0, 1, 1, s->snap_preview_border_px,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, mask, values);

    s->snap_preview_mapped = false;
}

void snap_preview_destroy(server_t* s) {
    if (!s || !s->conn) return;
    if (s->snap_preview_win != XCB_WINDOW_NONE) {
        xcb_destroy_window(s->conn, s->snap_preview_win);
        s->snap_preview_win = XCB_WINDOW_NONE;
        s->snap_preview_mapped = false;
    }
}

void snap_preview_apply(server_t* s, const rect_t* rect, bool show) {
    if (!s || s->snap_preview_win == XCB_WINDOW_NONE) return;

    if (!show || !rect) {
        if (s->snap_preview_mapped) {
            xcb_unmap_window(s->conn, s->snap_preview_win);
            s->snap_preview_mapped = false;
        }
        return;
    }

    uint32_t values[5];
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                    XCB_CONFIG_WINDOW_STACK_MODE;
    values[0] = (uint32_t)rect->x;
    values[1] = (uint32_t)rect->y;
    values[2] = rect->w;
    values[3] = rect->h;
    values[4] = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(s->conn, s->snap_preview_win, mask, values);

    if (!s->snap_preview_mapped) {
        xcb_map_window(s->conn, s->snap_preview_win);
        s->snap_preview_mapped = true;
    }
}
