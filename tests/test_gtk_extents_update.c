#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "cookie_jar.h"
#include "event.h"
#include "wm.h"
#include "xcb_utils.h"

// Stubs needed for linking
extern xcb_atom_t stub_last_prop_atom;

void test_gtk_extents_late_update() {
    atoms._GTK_FRAME_EXTENTS = 1;

    server_t s;
    memset(&s, 0, sizeof(s));
    s.conn = (xcb_connection_t*)malloc(1);
    config_init_defaults(&s.config);

    if (!slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t))) return;

    void *hot_ptr = NULL, *cold_ptr = NULL;
    handle_t h = slotmap_alloc(&s.clients, &hot_ptr, &cold_ptr);
    client_hot_t* hot = (client_hot_t*)hot_ptr;
    hot->self = h;
    hot->xid = 100;
    hot->state = STATE_MAPPED;
    hot->manage_phase = MANAGE_DONE;

    // Initial State: Decorated (No Extents)
    // Conceptually, X window includes only content.
    // Frame is at 100,100.
    // Content is at 100,100 800x600.
    hot->desired.x = 100;
    hot->desired.y = 100;
    hot->desired.w = 800;
    hot->desired.h = 600;
    hot->gtk_frame_extents_set = false;
    memset(&hot->gtk_extents, 0, sizeof(hot->gtk_extents));
    hot->dirty = DIRTY_NONE;

    // Simulate property reply: Extents = 10, 10, 10, 10
    // Reply structure:
    // xcb_get_property_reply_t header
    // then value
    struct {
        xcb_get_property_reply_t rep;
        uint32_t val[4];
    } reply_struct;
    memset(&reply_struct, 0, sizeof(reply_struct));
    reply_struct.rep.response_type = XCB_PROPERTY_NOTIFY;  // Irrelevant for handle_reply
    reply_struct.rep.format = 32;
    reply_struct.rep.type = XCB_ATOM_CARDINAL;
    reply_struct.rep.value_len = 4;  // 4 items (32-bit)
    reply_struct.rep.length = 4;
    reply_struct.val[0] = 10;
    reply_struct.val[1] = 10;
    reply_struct.val[2] = 10;
    reply_struct.val[3] = 10;

    cookie_slot_t slot;
    slot.client = h;
    slot.type = COOKIE_GET_PROPERTY;
    slot.data = (uint64_t)atoms._GTK_FRAME_EXTENTS;  // Low 32 bits

    // Call handler
    wm_handle_reply(&s, &slot, &reply_struct.rep, NULL);

    // Verify
    // desired should shift to "Content" inside the Shadow.
    // X += 10 -> 110
    // Y += 10 -> 110
    // W -= 20 -> 780
    // H -= 20 -> 580

    assert(hot->gtk_frame_extents_set == true);
    assert(hot->gtk_extents.left == 10);

    printf("Desired X: %d (expected 110)\n", hot->desired.x);
    printf("Desired W: %d (expected 780)\n", hot->desired.w);

    assert(hot->desired.x == 110);
    assert(hot->desired.y == 110);
    assert(hot->desired.w == 780);
    assert(hot->desired.h == 580);
    assert(hot->dirty & DIRTY_GEOM);

    printf("test_gtk_extents_late_update (No -> Yes) passed\n");

    // Reverse: Yes -> No
    // Reset dirty
    hot->dirty = DIRTY_NONE;

    // Reply with 0 length (Deleted or Empty)
    struct {
        xcb_get_property_reply_t rep;
    } empty_reply;
    memset(&empty_reply, 0, sizeof(empty_reply));
    empty_reply.rep.format = 0;  // Invalid/Empty
    empty_reply.rep.length = 0;

    wm_handle_reply(&s, &slot, &empty_reply.rep, NULL);

    assert(hot->gtk_frame_extents_set == false);

    // desired should revert
    // X -= 10 -> 100
    // W += 20 -> 800

    printf("Desired X: %d (expected 100)\n", hot->desired.x);
    printf("Desired W: %d (expected 800)\n", hot->desired.w);

    assert(hot->desired.x == 100);
    assert(hot->desired.y == 100);
    assert(hot->desired.w == 800);
    assert(hot->desired.h == 600);
    assert(hot->dirty & DIRTY_GEOM);

    printf("test_gtk_extents_late_update (Yes -> No) passed\n");

    render_free(&hot->render_ctx);
    slotmap_destroy(&s.clients);
    config_destroy(&s.config);
    free(s.conn);
}

int main() {
    test_gtk_extents_late_update();
    return 0;
}
