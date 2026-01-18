#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <xcb/xcb.h>

#include "client.h"
#include "config.h"
#include "event.h"
#include "hxm.h"
#include "render.h"
#include "wm.h"
#include "xcb_utils.h"

extern void xcb_stubs_reset(void);
extern uint32_t stub_last_image_w;

static void setup_server(server_t* s) {
    memset(s, 0, sizeof(*s));
    s->is_test = true;
    s->conn = xcb_connect(NULL, NULL);
    atoms_init(s->conn);

    s->root = 1;
    s->root_visual = 1;
    s->root_depth = 24;
    s->root_visual_type = xcb_get_visualtype(s->conn, 0);

    config_init_defaults(&s->config);
    s->config.theme.border_width = 2;
    s->config.theme.title_height = 18;

    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    list_init(&s->focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s->layers[i]);

    slotmap_init(&s->clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
    small_vec_init(&s->active_clients);
    arena_init(&s->tick_arena, 4096);

    s->in_commit_phase = true;
}

static void cleanup_server(server_t* s) {
    for (uint32_t i = 1; i < s->clients.cap; i++) {
        if (!s->clients.hdr[i].live) continue;
        handle_t h = handle_make(i, s->clients.hdr[i].gen);
        client_hot_t* hot = server_chot(s, h);
        client_cold_t* cold = server_ccold(s, h);
        if (cold) arena_destroy(&cold->string_arena);
        if (hot) render_free(&hot->render_ctx);
    }
    slotmap_destroy(&s->clients);
    small_vec_destroy(&s->active_clients);
    hash_map_destroy(&s->window_to_client);
    hash_map_destroy(&s->frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_destroy(&s->layers[i]);
    arena_destroy(&s->tick_arena);
    config_destroy(&s->config);
    xcb_disconnect(s->conn);
}

static handle_t add_client(server_t* s, xcb_window_t win, xcb_window_t frame) {
    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s->clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    memset(hot, 0, sizeof(*hot));
    memset(cold, 0, sizeof(*cold));

    render_init(&hot->render_ctx);
    arena_init(&cold->string_arena, 128);

    hot->self = h;
    hot->xid = win;
    hot->frame = frame;
    hot->state = STATE_MAPPED;
    hot->type = WINDOW_TYPE_NORMAL;
    hot->layer = LAYER_NORMAL;
    hot->base_layer = LAYER_NORMAL;
    hot->desired = (rect_t){10, 20, 120, 80};
    hot->server = hot->desired;
    hot->stacking_index = -1;
    hot->stacking_layer = -1;
    list_init(&hot->focus_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    hash_map_insert(&s->window_to_client, win, handle_to_ptr(h));
    hash_map_insert(&s->frame_to_client, frame, handle_to_ptr(h));
    small_vec_push(&s->active_clients, handle_to_ptr(h));
    return h;
}

static void test_frame_damage_triggers_flush(void) {
    printf("Testing frame damage triggers wm_flush_dirty...\n");
    server_t s;
    setup_server(&s);

    handle_t h = add_client(&s, 100, 101);
    client_hot_t* hot = server_chot(&s, h);
    assert(hot);

    hot->dirty = DIRTY_NONE;
    hot->frame_damage = dirty_region_make(0, 0, 12, 10);

    xcb_stubs_reset();
    stub_last_image_w = 0;

    bool flushed = wm_flush_dirty(&s, monotonic_time_ns());
    assert(flushed);
    assert(stub_last_image_w > 0);
    assert(!hot->frame_damage.valid);

    cleanup_server(&s);
    printf("PASS: frame damage triggers flush\n");
}

int main(void) {
    test_frame_damage_triggers_flush();
    printf("All wm_dirty frame damage tests passed\n");
    return 0;
}
