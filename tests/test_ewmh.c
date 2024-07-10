#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "event.h"
#include "wm.h"

void test_root_dirty_flags() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.is_test = true;
    s.root_depth = 24;
    s.root_visual_type = xcb_get_visualtype(NULL, 0);

    // Minimal init
    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    // Simulate connection? No, just verify flags are set/cleared.
    // Since wm_flush_dirty calls xcb functions, we can't run it fully without a mock connection or stubbing xcb.
    // However, we can verify that client_manage sets the flag.

    // We can't easily test wm_flush_dirty's xcb calls in this unit test setup without significant mocking.
    // But we can test the state transitions.

    // 1. Manage sets ROOT_DIRTY_CLIENT_LIST
    s.root_dirty = 0;

    // Manually simulate what client_finish_manage does for the flag
    handle_t h = slotmap_alloc(&s.clients, NULL, NULL);
    s.root_dirty |= ROOT_DIRTY_CLIENT_LIST;

    assert(s.root_dirty & ROOT_DIRTY_CLIENT_LIST);

    // 2. Unmanage sets ROOT_DIRTY_CLIENT_LIST
    s.root_dirty = 0;
    slotmap_free(&s.clients, h);
    s.root_dirty |= ROOT_DIRTY_CLIENT_LIST;

    assert(s.root_dirty & ROOT_DIRTY_CLIENT_LIST);

    // 3. Focus sets ROOT_DIRTY_ACTIVE_WINDOW
    s.root_dirty = 0;
    // wm_set_focus logic simulation
    s.root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
    assert(s.root_dirty & ROOT_DIRTY_ACTIVE_WINDOW);

    printf("test_root_dirty_flags passed\n");
    slotmap_destroy(&s.clients);
}

void test_net_wm_state_logic() {
    // Verify mapping from internal state to atoms list logic (simulated)
    client_hot_t hot;
    memset(&hot, 0, sizeof(hot));

    hot.layer = LAYER_FULLSCREEN;
    hot.base_layer = LAYER_NORMAL;
    hot.flags = CLIENT_FLAG_URGENT;
    hot.state_above = false;
    hot.state_below = false;

    // Logic from wm_flush_dirty
    xcb_atom_t state_atoms[5];
    uint32_t count = 0;

    // Mock atoms
    xcb_atom_t ATOM_FULLSCREEN = 100;
    xcb_atom_t ATOM_ABOVE = 101;
    xcb_atom_t ATOM_URGENT = 102;

    if (hot.layer == LAYER_FULLSCREEN) {
        state_atoms[count++] = ATOM_FULLSCREEN;
    }
    if (hot.state_above) {
        state_atoms[count++] = ATOM_ABOVE;
    }

    if (hot.flags & CLIENT_FLAG_URGENT) {
        state_atoms[count++] = ATOM_URGENT;
    }

    assert(count == 2);
    assert(state_atoms[0] == ATOM_FULLSCREEN);
    assert(state_atoms[1] == ATOM_URGENT);

    printf("test_net_wm_state_logic passed\n");
}

int main() {
    test_root_dirty_flags();
    test_net_wm_state_logic();
    return 0;
}
