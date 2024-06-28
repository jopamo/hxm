#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "config.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

volatile sig_atomic_t g_reload_pending = 0;

void setup_server(server_t* s) {
    memset(s, 0, sizeof(server_t));
    s->is_test = true;
    s->root_depth = 24;
    s->root_visual_type = xcb_get_visualtype(NULL, 0);
    s->conn = xcb_connect(NULL, NULL);
    s->keysyms = xcb_key_symbols_alloc(s->conn);
    slotmap_init(&s->clients, 32, sizeof(client_hot_t), sizeof(client_cold_t));
    s->desktop_count = 4;
    s->current_desktop = 0;
    list_init(&s->focus_history);
    for (int i = 0; i < LAYER_COUNT; i++) list_init(&s->layers[i]);
    hash_map_init(&s->window_to_client);
    hash_map_init(&s->frame_to_client);
    config_init_defaults(&s->config);

    s->workarea.x = 0;
    s->workarea.y = 0;
    s->workarea.w = 1920;
    s->workarea.h = 1080;
}

void test_rules_matching() {
    server_t s;
    setup_server(&s);

    // Add a rule
    // rule = class:XTerm -> desktop:2, layer:above, focus:no
    config_load(&s.config, "/non/existent");  // Just to ensure defaults are there
    // Manually add rule
    app_rule_t* r = calloc(1, sizeof(*r));
    r->class_match = strdup("XTerm");
    r->type_match = -1;
    r->transient_match = -1;
    r->desktop = 2;
    r->layer = LAYER_ABOVE;
    r->focus = 0;
    r->placement = PLACEMENT_CENTER;
    small_vec_push(&s.config.rules, r);

    // Mock client
    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    client_cold_t* cold = (client_cold_t*)cold_ptr;
    hot->self = h;
    hot->xid = 101;
    hot->type = WINDOW_TYPE_NORMAL;
    hot->desktop = 0;
    hot->layer = LAYER_NORMAL;
    hot->focus_override = -1;
    hot->placement = PLACEMENT_DEFAULT;
    hot->desired.w = 400;
    hot->desired.h = 300;
    list_init(&hot->focus_node);
    list_init(&hot->stacking_node);
    list_init(&hot->transients_head);
    list_init(&hot->transient_sibling);

    cold->wm_class = strdup("XTerm");
    cold->wm_instance = strdup("xterm");

    // We need to call client_finish_manage, but it does a lot of X calls.
    // Actually, client_finish_manage calls client_apply_rules.
    // I can just test client_apply_rules if it was public, but it's static.
    // Wait, I can make it public or just test via client_finish_manage with stubs.

    // I'll call client_finish_manage.
    client_finish_manage(&s, h);

    assert(hot->desktop == 2);
    assert(hot->layer == LAYER_ABOVE);
    assert(hot->focus_override == 0);
    // Center placement: (1920-400)/2 = 760, (1080-300-20-4)/2 -> wait, title_height=20, border=2
    // geom.w + 2*border, geom.h + title + border
    // But hot->desired is just the client geom.
    // wm_place_window uses hot->desired.
    assert(hot->desired.x == (1920 - 400) / 2);
    assert(hot->desired.y == (1080 - 300) / 2);

    printf("test_rules_matching passed\n");

    // Cleanup
    free(cold->wm_class);
    free(cold->wm_instance);
    client_unmanage(&s, h);
    config_destroy(&s.config);
    slotmap_destroy(&s.clients);
    hash_map_destroy(&s.window_to_client);
    hash_map_destroy(&s.frame_to_client);
    xcb_key_symbols_free(s.keysyms);
    xcb_disconnect(s.conn);
}

int main() {
    test_rules_matching();
    return 0;
}
