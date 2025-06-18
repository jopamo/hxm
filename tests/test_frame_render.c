#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "client.h"
#include "event.h"
#include "frame.h"
#include "hxm.h"
#include "render.h"
#include "slotmap.h"
#include "wm.h"
#include "xcb_utils.h"

// Stubs for image capture
extern uint32_t stub_last_image_w;
extern uint32_t stub_last_image_h;
extern uint8_t stub_last_image_data[200 * 1024];

static server_t s;
static client_hot_t* hot;
static client_cold_t* cold;
static handle_t h;

static void setup(void) {
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 32;

    // Initialize theme with distinct colors
    s.config.theme.border_width = 2;
    s.config.theme.title_height = 20;

    // Active: Red border, Blue title
    s.config.theme.window_active_border_color = 0xFFFF0000;
    s.config.theme.window_active_title.color = 0xFF0000FF;
    s.config.theme.window_active_title.flags = BG_SOLID;
    s.config.theme.window_active_label_text_color = 0xFFFFFFFF;  // White text

    // Inactive: Green border, Yellow title
    s.config.theme.window_inactive_border_color = 0xFF00FF00;
    s.config.theme.window_inactive_title.color = 0xFFFFFF00;
    s.config.theme.window_inactive_title.flags = BG_SOLID;
    s.config.theme.window_inactive_label_text_color = 0xFF000000;  // Black text

    slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
    void* cold_ptr = NULL;
    h = slotmap_alloc(&s.clients, (void**)&hot, &cold_ptr);
    cold = (client_cold_t*)cold_ptr;

    hot->self = h;
    hot->xid = 100;
    hot->frame = 101;
    hot->server.w = 200;
    hot->server.h = 100;
    hot->flags = 0;  // Not focused initially
    hot->dirty = DIRTY_FRAME_ALL;

    // Setup render context
    render_init(&hot->render_ctx);

    // Reset stubs
    stub_last_image_w = 0;
    stub_last_image_h = 0;
    memset(stub_last_image_data, 0, sizeof(stub_last_image_data));

    // Mock visual
    s.root_visual_type = xcb_get_visualtype(s.conn, 0);  // Stubs return a valid visual
}

static void teardown(void) {
    render_free(&hot->render_ctx);
    slotmap_destroy(&s.clients);
}

static void test_frame_render_no_icon(void) {
    printf("Testing frame render without icon...\n");
    setup();

    // No icon set
    hot->icon_surface = NULL;
    s.default_icon = NULL;

    // Simulate flush
    s.in_commit_phase = true;
    frame_flush(&s, h);

    // Verify image size
    assert(stub_last_image_w > 0);
    assert(stub_last_image_h > 0);

    // Check top-left pixel (border color) - Inactive -> Green (00FF00)
    uint32_t* pixels = (uint32_t*)stub_last_image_data;
    uint32_t pixel = pixels[0];

    assert(pixel == 0xFF00FF00);  // Green

    teardown();
    printf("PASS: Frame render no icon\n");
}

static void test_frame_render_active_color(void) {
    printf("Testing frame render active color...\n");
    setup();

    // Set focused
    hot->flags |= CLIENT_FLAG_FOCUSED;
    hot->dirty = DIRTY_FRAME_ALL;

    s.in_commit_phase = true;
    frame_flush(&s, h);

    uint32_t* pixels = (uint32_t*)stub_last_image_data;
    uint32_t pixel = pixels[0];

    // Active Border Color: 0xFFFF0000 (Red)
    assert(pixel == 0xFFFF0000);

    teardown();
    printf("PASS: Frame render active color\n");
}

static void test_frame_controls_position(void) {
    printf("Testing frame controls position...\n");
    setup();

    // Active window (Red border, Blue title)
    hot->flags |= CLIENT_FLAG_FOCUSED;
    hot->dirty = DIRTY_FRAME_ALL;
    hot->server.w = 100;  // Small width

    s.in_commit_phase = true;
    frame_flush(&s, h);

    uint32_t* pixels = (uint32_t*)stub_last_image_data;
    int stride_pixels = stub_last_image_w;

    // Rightmost "close" button x:
    // x = w - border - pad - size = 104 - 2 - 4 - 16 = 82.
    // y = (20 - 16)/2 = 2.
    // Pixel at (82, 2) should be button border -> White (0xFFFFFFFF)

    uint32_t pixel_btn_border = pixels[2 * stride_pixels + 82];

    // Because of anti-aliasing it might not be pure white, but it should definitely NOT be Blue (BG)
    assert(pixel_btn_border != 0xFF0000FF);

    // Check a pixel inside the button (84, 10) - Should be safe from 'X' glyph
    // 'X' is from (4,4) to (12,12) in local coords.
    // Local (2,8) is outside the X diagonal.
    uint32_t pixel_btn_inner = pixels[10 * stride_pixels + 84];

    if (pixel_btn_inner != 0xFF0000FF) {
        printf("Pixel at (84, 10) is 0x%08X, expected 0xFF0000FF\n", pixel_btn_inner);
    }
    assert(pixel_btn_inner == 0xFF0000FF);

    teardown();
    printf("PASS: Frame controls position\n");
}

int main(void) {
    test_frame_render_no_icon();
    test_frame_render_active_color();
    test_frame_controls_position();
    return 0;
}
