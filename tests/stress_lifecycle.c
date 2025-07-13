#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"

extern void xcb_stubs_reset(void);

static void stress_cleanup_visitor(void* hot_ptr, void* cold_ptr, handle_t h, void* user) {
    (void)h;
    (void)user;
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    if (cold) arena_destroy(&cold->string_arena);
    if (hot) render_free(&hot->render_ctx);
}

void test_rapid_lifecycle(void) {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.conn = xcb_connect(NULL, NULL);
    atoms_init(s.conn);

    slotmap_init(&s.clients, 1024, sizeof(client_hot_t), sizeof(client_cold_t));
    hash_map_init(&s.window_to_client);
    hash_map_init(&s.frame_to_client);
    list_init(&s.focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_init(&s.layers[i]);
    small_vec_init(&s.active_clients);
    arena_init(&s.tick_arena, 64 * 1024);
    cookie_jar_init(&s.cookie_jar);
    config_init_defaults(&s.config);

    const int ITERATIONS = 50;
    const int WINDOWS_PER_ITER = 10;

    for (int i = 0; i < ITERATIONS; i++) {
        handle_t handles[WINDOWS_PER_ITER];

        for (int j = 0; j < WINDOWS_PER_ITER; j++) {
            xcb_window_t win = 1000 + i * 100 + j;
            client_manage_start(&s, win);
            handles[j] = server_get_client_by_window(&s, win);
            assert(handles[j] != HANDLE_INVALID);
        }

        // Simulate all replies arriving
        for (int j = 0; j < WINDOWS_PER_ITER; j++) {
            client_hot_t* hot = server_chot(&s, handles[j]);
            hot->pending_replies = 0;
            hot->state = STATE_READY;
        }

        // Flush (frames windows)
        wm_flush_dirty(&s, monotonic_time_ns());

        // Unmanage half
        for (int j = 0; j < WINDOWS_PER_ITER / 2; j++) {
            client_unmanage(&s, handles[j]);
        }

        wm_flush_dirty(&s, monotonic_time_ns());
    }

    printf("test_rapid_lifecycle passed\n");

    // Cleanup
    slotmap_for_each_used(&s.clients, stress_cleanup_visitor, &s);
    slotmap_destroy(&s.clients);
    hash_map_destroy(&s.window_to_client);
    hash_map_destroy(&s.frame_to_client);
    for (int i = 0; i < LAYER_COUNT; i++) small_vec_destroy(&s.layers[i]);
    small_vec_destroy(&s.active_clients);
    arena_destroy(&s.tick_arena);
    cookie_jar_destroy(&s.cookie_jar);
    config_destroy(&s.config);
    free(s.conn);
}

int main(void) {
    test_rapid_lifecycle();
    return 0;
}
