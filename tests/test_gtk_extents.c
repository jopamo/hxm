#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

// From xcb stubs
extern void xcb_stubs_reset(void);

extern xcb_window_t stub_last_config_window;
extern int32_t stub_last_config_x;
extern int32_t stub_last_config_y;
extern uint32_t stub_last_config_w;
extern uint32_t stub_last_config_h;
extern int stub_configure_window_count;

typedef struct stub_config_call {
    xcb_window_t win;
    uint16_t mask;
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t border_width;
    xcb_window_t sibling;
    uint32_t stack_mode;
} stub_config_call_t;

extern stub_config_call_t stub_config_calls[64];
extern int stub_config_calls_len;

static void reset_config_captures(void) {
    xcb_stubs_reset();
    // If you ever want to only reset configure info but not everything,
    // split a xcb_stubs_reset_config helper later
}

typedef struct test_server {
    server_t s;
    client_hot_t* created[16];
    size_t created_len;
} test_server_t;

static void test_server_init(test_server_t* ts) {
    memset(ts, 0, sizeof(*ts));

    ts->s.is_test = true;
    ts->s.conn = (xcb_connection_t*)malloc(1);
    assert(ts->s.conn);

    config_init_defaults(&ts->s.config);

    small_vec_init(&ts->s.active_clients);
    bool ok = slotmap_init(&ts->s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
    assert(ok);
}

static client_hot_t* test_client_add(test_server_t* ts, xcb_window_t xid, xcb_window_t frame) {
    void* hot_ptr = NULL;
    void* cold_ptr = NULL;

    handle_t h = slotmap_alloc(&ts->s.clients, &hot_ptr, &cold_ptr);
    assert(h != HANDLE_INVALID);
    assert(hot_ptr);
    assert(cold_ptr);

    client_hot_t* hot = (client_hot_t*)hot_ptr;
    memset(hot, 0, sizeof(*hot));

    hot->self = h;
    hot->xid = xid;
    hot->frame = frame;
    hot->state = STATE_MAPPED;

    // Default geometry
    hot->desired.x = 50;
    hot->desired.y = 50;
    hot->desired.w = 400;
    hot->desired.h = 300;

    small_vec_push(&ts->s.active_clients, handle_to_ptr(h));

    assert(ts->created_len < (sizeof(ts->created) / sizeof(ts->created[0])));
    ts->created[ts->created_len++] = hot;

    return hot;
}

static void test_server_destroy(test_server_t* ts) {
    // Free only what we created
    for (size_t i = 0; i < ts->created_len; i++) {
        client_hot_t* hot = ts->created[i];
        if (!hot) continue;

        render_free(&hot->render_ctx);
        if (hot->icon_surface) cairo_surface_destroy(hot->icon_surface);
        hot->icon_surface = NULL;
    }

    small_vec_destroy(&ts->s.active_clients);
    slotmap_destroy(&ts->s.clients);
    config_destroy(&ts->s.config);
    free(ts->s.conn);
    ts->s.conn = NULL;
}

static void assert_call_eq(const stub_config_call_t* c, xcb_window_t win, int32_t x, int32_t y, uint32_t w,
                           uint32_t h) {
    assert(c);
    assert(c->win == win);
    assert(c->x == x);
    assert(c->y == y);
    assert(c->w == w);
    assert(c->h == h);
}

static void test_gtk_extents_inflation_order_and_state(void) {
    test_server_t ts;
    test_server_init(&ts);

    // Even if we have border width, GTK extents should dominate for undecorated client
    ts.s.config.theme.border_width = 5;
    ts.s.config.theme.title_height = 20;

    client_hot_t* hot = test_client_add(&ts, 100, 200);

    // Set GTK extents
    hot->gtk_frame_extents_set = true;
    hot->gtk_extents.left = 10;
    hot->gtk_extents.right = 10;
    hot->gtk_extents.top = 20;
    hot->gtk_extents.bottom = 20;

    hot->dirty = DIRTY_GEOM;

    reset_config_captures();
    wm_flush_dirty(&ts.s, monotonic_time_ns());

    // Expected: CSD Window (Extents Respected)
    // Frame: Positioned at desired (logical) pos MINUS extents, Sized to content (includes shadow)
    const int32_t exp_frame_x = 50 - 10;
    const int32_t exp_frame_y = 50 - 20;
    const uint32_t exp_frame_w = 400;
    const uint32_t exp_frame_h = 300;

    // Client: Positioned inside frame at 0,0
    const int32_t exp_client_x = 0;
    const int32_t exp_client_y = 0;
    const uint32_t exp_client_w = 400;
    const uint32_t exp_client_h = 300;

    // Two configures: frame then client
    assert(stub_configure_window_count == 2);
    assert(stub_config_calls_len == 2);

    assert_call_eq(&stub_config_calls[0], 200, exp_frame_x, exp_frame_y, exp_frame_w, exp_frame_h);
    assert_call_eq(&stub_config_calls[1], 100, exp_client_x, exp_client_y, exp_client_w, exp_client_h);

    // Last call should be the client
    assert(stub_last_config_window == 100);
    assert(stub_last_config_x == exp_client_x);
    assert(stub_last_config_y == exp_client_y);
    assert(stub_last_config_w == exp_client_w);
    assert(stub_last_config_h == exp_client_h);

    // Server state should reflect frame geometry
    assert(hot->server.x == exp_frame_x);
    assert(hot->server.y == exp_frame_y);
    assert(hot->server.w == exp_client_w);
    assert(hot->server.h == exp_client_h);

    printf("test_gtk_extents_inflation_order_and_state passed\n");

    test_server_destroy(&ts);
}

static void test_no_gtk_extents_no_inflation(void) {
    test_server_t ts;
    test_server_init(&ts);

    ts.s.config.theme.border_width = 5;
    ts.s.config.theme.title_height = 20;  // Explicitly set default

    client_hot_t* hot = test_client_add(&ts, 101, 201);

    hot->gtk_frame_extents_set = false;
    memset(&hot->gtk_extents, 0, sizeof(hot->gtk_extents));

    hot->dirty = DIRTY_GEOM;

    reset_config_captures();
    wm_flush_dirty(&ts.s, monotonic_time_ns());

    const int32_t exp_frame_x = 50;
    const int32_t exp_frame_y = 50;
    const uint32_t exp_frame_w = 400 + 2 * ts.s.config.theme.border_width;
    const uint32_t exp_frame_h = 300 + ts.s.config.theme.title_height + ts.s.config.theme.border_width;
    const uint32_t exp_client_w = 400;
    const uint32_t exp_client_h = 300;

    assert(stub_configure_window_count == 2);
    assert(stub_config_calls_len == 2);

    assert_call_eq(&stub_config_calls[0], 201, exp_frame_x, exp_frame_y, exp_frame_w, exp_frame_h);
    assert_call_eq(&stub_config_calls[1], 101, (int32_t)ts.s.config.theme.border_width,
                   (int32_t)ts.s.config.theme.title_height, exp_client_w, exp_client_h);

    assert(hot->server.x == exp_frame_x);
    assert(hot->server.y == exp_frame_y);
    assert(hot->server.w == exp_client_w);
    assert(hot->server.h == exp_client_h);

    printf("test_no_gtk_extents_no_inflation passed\n");

    test_server_destroy(&ts);
}

static void test_not_dirty_no_configure(void) {
    test_server_t ts;
    test_server_init(&ts);

    client_hot_t* hot = test_client_add(&ts, 102, 202);

    hot->gtk_frame_extents_set = true;
    hot->gtk_extents.left = 7;
    hot->gtk_extents.right = 9;
    hot->gtk_extents.top = 11;
    hot->gtk_extents.bottom = 13;

    hot->dirty = 0;

    reset_config_captures();
    wm_flush_dirty(&ts.s, monotonic_time_ns());

    assert(stub_configure_window_count == 0);
    assert(stub_config_calls_len == 0);

    printf("test_not_dirty_no_configure passed\n");

    test_server_destroy(&ts);
}

static void test_idempotent_second_flush_does_nothing(void) {
    test_server_t ts;
    test_server_init(&ts);

    client_hot_t* hot = test_client_add(&ts, 103, 203);

    hot->gtk_frame_extents_set = true;
    hot->gtk_extents.left = 1;
    hot->gtk_extents.right = 2;
    hot->gtk_extents.top = 3;
    hot->gtk_extents.bottom = 4;

    hot->dirty = DIRTY_GEOM;

    reset_config_captures();
    wm_flush_dirty(&ts.s, monotonic_time_ns());

    assert(stub_configure_window_count == 2);
    assert(stub_config_calls_len == 2);

    reset_config_captures();
    wm_flush_dirty(&ts.s, monotonic_time_ns());

    assert(stub_configure_window_count == 0);
    assert(stub_config_calls_len == 0);

    printf("test_idempotent_second_flush_does_nothing passed\n");

    test_server_destroy(&ts);
}

static void test_two_clients_both_configured(void) {
    test_server_t ts;
    test_server_init(&ts);

    ts.s.config.theme.border_width = 5;
    ts.s.config.theme.title_height = 20;

    client_hot_t* a = test_client_add(&ts, 110, 210);
    client_hot_t* b = test_client_add(&ts, 111, 211);

    a->desired.x = 10;
    a->desired.y = 20;
    a->desired.w = 100;
    a->desired.h = 200;
    a->gtk_frame_extents_set = true;
    a->gtk_extents.left = 5;
    a->gtk_extents.right = 6;
    a->gtk_extents.top = 7;
    a->gtk_extents.bottom = 8;
    a->dirty = DIRTY_GEOM;

    b->desired.x = 30;
    b->desired.y = 40;
    b->desired.w = 300;
    b->desired.h = 400;
    b->gtk_frame_extents_set = false;
    memset(&b->gtk_extents, 0, sizeof(b->gtk_extents));
    b->dirty = DIRTY_GEOM;

    reset_config_captures();
    wm_flush_dirty(&ts.s, monotonic_time_ns());

    assert(stub_configure_window_count == 4);
    assert(stub_config_calls_len == 4);

    // Find paired calls frame->client for A and B without assuming inter-client order
    bool found_a = false;
    bool found_b = false;

    for (int i = 0; i + 1 < stub_config_calls_len; i++) {
        const stub_config_call_t* c0 = &stub_config_calls[i];
        const stub_config_call_t* c1 = &stub_config_calls[i + 1];

        if (!found_a && c0->win == 210 && c1->win == 110) {
            // Client A: Now CSD Window (Extents Respected)
            const int32_t fx = 10 - 5;
            const int32_t fy = 20 - 7;
            const uint32_t fw = 100;
            const uint32_t fh = 200;

            const int32_t cx = 0;
            const int32_t cy = 0;
            const uint32_t cw = 100;
            const uint32_t ch = 200;

            assert_call_eq(c0, 210, fx, fy, fw, fh);
            assert_call_eq(c1, 110, cx, cy, cw, ch);
            found_a = true;
        }

        if (!found_b && c0->win == 211 && c1->win == 111) {
            const int32_t fx = 30;
            const int32_t fy = 40;
            const uint32_t frame_w = 300 + 2 * ts.s.config.theme.border_width;
            const uint32_t frame_h = 400 + ts.s.config.theme.title_height + ts.s.config.theme.border_width;
            assert_call_eq(c0, 211, fx, fy, frame_w, frame_h);
            assert_call_eq(c1, 111, (int32_t)ts.s.config.theme.border_width, (int32_t)ts.s.config.theme.title_height,
                           300, 400);
            found_b = true;
        }
    }

    assert(found_a);
    assert(found_b);

    printf("test_two_clients_both_configured passed\n");

    test_server_destroy(&ts);
}

int main(void) {
    test_gtk_extents_inflation_order_and_state();
    test_no_gtk_extents_no_inflation();
    test_not_dirty_no_configure();
    test_idempotent_second_flush_does_nothing();
    test_two_clients_both_configured();

    printf("all gtk extents / flush dirty tests passed\n");
    return 0;
}
