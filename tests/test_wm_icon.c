#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

volatile sig_atomic_t g_reload_pending = 0;

void test_wm_icon() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);
    s.conn = (xcb_connection_t*)malloc(1);

    atoms._NET_WM_ICON = 99;

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr, *cold_ptr;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->xid = 123;
    hot->state = STATE_NEW;
    hot->pending_replies = 10;
    arena_init(&cold->string_arena, 512);

    // Mock reply for _NET_WM_ICON
    // Data is CARDINAL array: width, height, pixels...
    // Let's make a 2x2 icon
    struct {
        xcb_get_property_reply_t reply;
        uint32_t data[2 + 2 * 2];
    } mock_r;
    memset(&mock_r, 0, sizeof(mock_r));
    mock_r.reply.format = 32;
    mock_r.reply.type = XCB_ATOM_CARDINAL;
    mock_r.reply.value_len = 6;   // 2 dims + 4 pixels
    mock_r.data[0] = 2;           // w
    mock_r.data[1] = 2;           // h
    mock_r.data[2] = 0xFF0000FF;  // ARGB Red
    mock_r.data[3] = 0xFF00FF00;  // ARGB Green
    mock_r.data[4] = 0xFFFF0000;  // ARGB Blue
    mock_r.data[5] = 0xFFFFFFFF;  // ARGB White

    cookie_slot_t slot;
    slot.type = COOKIE_GET_PROPERTY;
    slot.client = h;
    slot.data = ((uint64_t)123 << 32) | atoms._NET_WM_ICON;

    wm_handle_reply(&s, &slot, &mock_r, NULL);

    assert(hot->icon_surface != NULL);
    assert(cairo_image_surface_get_width(hot->icon_surface) == 2);
    assert(cairo_image_surface_get_height(hot->icon_surface) == 2);

    unsigned char* data = cairo_image_surface_get_data(hot->icon_surface);
    // Cairo uses native endian ARGB32.
    // If little endian, 0xFF0000FF -> B=FF, G=00, R=00, A=FF.
    // Wait, Cairo CAIRO_FORMAT_ARGB32 is pre-multiplied alpha, usually in host byte order.
    // On Little Endian: BGRA in memory.
    // 0xFF0000FF in u32 is ...
    // Let's just check the u32 value matches, assuming no endian swap in copy logic (which I verified).
    uint32_t* p = (uint32_t*)data;
    assert(p[0] == 0xFF0000FF);
    assert(p[1] == 0xFF00FF00);
    assert(p[2] == 0xFFFF0000);
    assert(p[3] == 0xFFFFFFFF);

    printf("test_wm_icon passed\n");

    arena_destroy(&cold->string_arena);
    for (uint32_t i = 1; i < s.clients.cap; i++) {
        if (s.clients.hdr[i].live) {
            handle_t h = handle_make(i, s.clients.hdr[i].gen);
            client_hot_t* hot = server_chot(&s, h);
            if (hot) {
                render_free(&hot->render_ctx);
                if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
            }
        }
    }
    slotmap_destroy(&s.clients);
    free(s.conn);
}

int main() {
    test_wm_icon();
    return 0;
}
