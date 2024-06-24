#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"

void test_adoption_logic() {
    server_t s;
    memset(&s, 0, sizeof(s));
    s.conn = (xcb_connection_t*)0xDEADBEEF;  // Just not NULL
    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;
    hash_map_init(&s.window_to_client);

    // Mock reply for adoption check
    xcb_get_window_attributes_reply_t mock_attr;
    memset(&mock_attr, 0, sizeof(mock_attr));
    mock_attr.override_redirect = 0;
    mock_attr.map_state = XCB_MAP_STATE_VIEWABLE;

    cookie_slot_t slot;
    slot.type = COOKIE_GET_WINDOW_ATTRIBUTES;
    slot.client = HANDLE_INVALID;
    slot.data = 999;  // window xid

    // This should trigger client_manage_start for window 999
    wm_handle_reply(&s, &slot, &mock_attr);

    // Check if window 999 is now being managed (should be in window_to_client map)
    handle_t h = server_get_client_by_window(&s, 999);
    assert(h != HANDLE_INVALID);

    client_hot_t* hot = server_chot(&s, h);
    assert(hot != NULL);
    assert(hot->xid == 999);
    assert(hot->state == STATE_NEW);

    printf("test_adoption_logic passed\n");

    slotmap_destroy(&s.clients);
    hash_map_destroy(&s.window_to_client);
}

int main() {
    test_adoption_logic();
    return 0;
}
