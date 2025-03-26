#include <X11/keysym.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/randr.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

/*
 * XCB stubs for unit tests
 *
 * Goals
 * - Deterministic XIDs
 * - Non-null setup/screen data
 * - Safe replies for *_reply calls
 * - Record side effects for assertions
 */

#define STUB_MAX_MAPPED 256
#define STUB_MAX_PROP_BYTES 4096
#define STUB_MAX_CONFIG_CALLS 64
#define STUB_MAX_PROP_CALLS 128
#define STUB_MAX_EVENTS 2048
#define STUB_MAX_ATTR_REQUESTS 256
#define STUB_MAX_KEY_GRABS 256

// XID generator
static uint32_t stub_xid_counter = 100;
static uint32_t stub_cookie_seq = 1;

// Extension data reply
static xcb_query_extension_reply_t stub_ext_reply;

// Setup/screen/visual
static xcb_setup_t stub_setup;
static xcb_screen_t stub_screen = {
    .root = 1,
    .width_in_pixels = 1920,
    .height_in_pixels = 1080,
};
static xcb_visualtype_t stub_visual = {
    .visual_id = 1,
};

// QueryTree children override
static xcb_window_t stub_query_tree_children[STUB_MAX_MAPPED];
static int stub_query_tree_children_len = 0;

// Selection owner
static xcb_window_t stub_selection_owner = XCB_NONE;

// Atom allocator
static xcb_atom_t stub_atom_counter = 1;

// Property capture for assertions
xcb_window_t stub_last_prop_window = 0;
xcb_atom_t stub_last_prop_atom = 0;
xcb_atom_t stub_last_prop_type = 0;
uint32_t stub_last_prop_len = 0;
uint8_t stub_last_prop_data[STUB_MAX_PROP_BYTES];

typedef struct stub_prop_call {
    xcb_window_t window;
    xcb_atom_t atom;
    xcb_atom_t type;
    uint8_t format;
    uint32_t len;
    uint8_t data[STUB_MAX_PROP_BYTES];
    bool deleted;
} stub_prop_call_t;

stub_prop_call_t stub_prop_calls[STUB_MAX_PROP_CALLS];
int stub_prop_calls_len = 0;

typedef struct stub_attr_request {
    uint32_t seq;
    xcb_window_t window;
} stub_attr_request_t;

static stub_attr_request_t stub_attr_requests[STUB_MAX_ATTR_REQUESTS];
static int stub_attr_requests_len = 0;

// Map/unmap capture
int stub_map_window_count = 0;
int stub_unmap_window_count = 0;
xcb_window_t stub_last_mapped_window = 0;
xcb_window_t stub_last_unmapped_window = 0;
int stub_set_input_focus_count = 0;
xcb_window_t stub_last_input_focus_window = 0;
uint8_t stub_last_input_focus_revert = 0;

xcb_window_t stub_mapped_windows[STUB_MAX_MAPPED];
int stub_mapped_windows_len = 0;

// Configure capture
xcb_window_t stub_last_config_window = 0;
uint16_t stub_last_config_mask = 0;
int32_t stub_last_config_x = 0;
int32_t stub_last_config_y = 0;
uint32_t stub_last_config_w = 0;
uint32_t stub_last_config_h = 0;
xcb_window_t stub_last_config_sibling = 0;
uint32_t stub_last_config_stack_mode = 0;
uint32_t stub_last_config_border_width = 0;
int stub_configure_window_count = 0;

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

stub_config_call_t stub_config_calls[STUB_MAX_CONFIG_CALLS];
int stub_config_calls_len = 0;

// Send event capture
int stub_send_event_count = 0;
xcb_window_t stub_last_send_event_destination = 0;
char stub_last_event[32];

// Kill client capture
int stub_kill_client_count = 0;
uint32_t stub_last_kill_client_resource = 0;
int stub_destroy_window_count = 0;
xcb_window_t stub_last_destroyed_window = 0;

// Grab button capture
int stub_grab_button_count = 0;
int stub_grab_key_count = 0;
int stub_ungrab_key_count = 0;
int stub_grab_pointer_count = 0;
int stub_ungrab_pointer_count = 0;
uint16_t stub_last_grab_key_mods = 0;
xcb_keycode_t stub_last_grab_keycode = 0;
xcb_cursor_t stub_last_grab_pointer_cursor = XCB_NONE;
int stub_install_colormap_count = 0;
xcb_colormap_t stub_last_installed_colormap = XCB_NONE;
int stub_save_set_insert_count = 0;
int stub_save_set_delete_count = 0;
xcb_window_t stub_last_save_set_window = XCB_NONE;
int stub_sync_await_count = 0;

// Optional reply hook for cookie draining
int (*stub_poll_for_reply_hook)(xcb_connection_t* c, unsigned int request, void** reply,
                                xcb_generic_error_t** error) = NULL;

// Event queues for poll stubs.
static xcb_generic_event_t* stub_queued_events[STUB_MAX_EVENTS];
static size_t stub_queued_head = 0;
static size_t stub_queued_len = 0;
static xcb_generic_event_t* stub_events[STUB_MAX_EVENTS];
static size_t stub_event_head = 0;
static size_t stub_event_len = 0;

// Public reset helper for tests
void xcb_stubs_reset(void) {
    stub_xid_counter = 100;
    stub_cookie_seq = 1;

    memset(&stub_ext_reply, 0, sizeof(stub_ext_reply));
    stub_ext_reply.present = 1;

    stub_selection_owner = XCB_NONE;
    stub_atom_counter = 1;

    stub_last_prop_window = 0;
    stub_last_prop_atom = 0;
    stub_last_prop_type = 0;
    stub_last_prop_len = 0;
    memset(stub_last_prop_data, 0, sizeof(stub_last_prop_data));
    stub_prop_calls_len = 0;
    memset(stub_prop_calls, 0, sizeof(stub_prop_calls));
    stub_attr_requests_len = 0;
    memset(stub_attr_requests, 0, sizeof(stub_attr_requests));
    stub_query_tree_children_len = 0;
    memset(stub_query_tree_children, 0, sizeof(stub_query_tree_children));

    stub_map_window_count = 0;
    stub_unmap_window_count = 0;
    stub_last_mapped_window = 0;
    stub_last_unmapped_window = 0;
    stub_set_input_focus_count = 0;
    stub_last_input_focus_window = 0;
    stub_last_input_focus_revert = 0;
    stub_mapped_windows_len = 0;
    memset(stub_mapped_windows, 0, sizeof(stub_mapped_windows));

    stub_last_config_window = 0;
    stub_last_config_mask = 0;
    stub_last_config_x = 0;
    stub_last_config_y = 0;
    stub_last_config_w = 0;
    stub_last_config_h = 0;
    stub_last_config_sibling = 0;
    stub_last_config_stack_mode = 0;
    stub_last_config_border_width = 0;
    stub_configure_window_count = 0;

    stub_config_calls_len = 0;
    memset(stub_config_calls, 0, sizeof(stub_config_calls));

    stub_send_event_count = 0;
    stub_last_send_event_destination = 0;
    memset(stub_last_event, 0, sizeof(stub_last_event));

    stub_kill_client_count = 0;
    stub_last_kill_client_resource = 0;
    stub_destroy_window_count = 0;
    stub_last_destroyed_window = 0;

    stub_grab_button_count = 0;
    stub_grab_key_count = 0;
    stub_ungrab_key_count = 0;
    stub_grab_pointer_count = 0;
    stub_ungrab_pointer_count = 0;
    stub_last_grab_key_mods = 0;
    stub_last_grab_keycode = 0;
    stub_last_grab_pointer_cursor = XCB_NONE;
    stub_install_colormap_count = 0;
    stub_last_installed_colormap = XCB_NONE;
    stub_save_set_insert_count = 0;
    stub_save_set_delete_count = 0;
    stub_last_save_set_window = XCB_NONE;
    stub_sync_await_count = 0;

    stub_poll_for_reply_hook = NULL;

    for (size_t i = 0; i < stub_queued_len; i++) {
        size_t idx = (stub_queued_head + i) % STUB_MAX_EVENTS;
        free(stub_queued_events[idx]);
        stub_queued_events[idx] = NULL;
    }
    for (size_t i = 0; i < stub_event_len; i++) {
        size_t idx = (stub_event_head + i) % STUB_MAX_EVENTS;
        free(stub_events[idx]);
        stub_events[idx] = NULL;
    }
    stub_queued_head = 0;
    stub_queued_len = 0;
    stub_event_head = 0;
    stub_event_len = 0;
}

// Basic connection lifecycle
xcb_connection_t* xcb_connect(const char* displayname, int* screenp) {
    (void)displayname;
    if (screenp) *screenp = 0;
    return (xcb_connection_t*)malloc(1);
}

void xcb_disconnect(xcb_connection_t* c) { free(c); }

int xcb_connection_has_error(xcb_connection_t* c) {
    (void)c;
    return 0;
}

int xcb_get_file_descriptor(xcb_connection_t* c) {
    (void)c;
    return -1;
}

int xcb_flush(xcb_connection_t* c) {
    (void)c;
    return 1;
}

// Setup/screen helpers
const xcb_setup_t* xcb_get_setup(xcb_connection_t* c) {
    (void)c;
    return &stub_setup;
}

xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t* R) {
    (void)R;
    xcb_screen_iterator_t it;
    memset(&it, 0, sizeof(it));
    it.data = &stub_screen;
    it.rem = 1;
    return it;
}

xcb_visualtype_t* xcb_get_visualtype(xcb_connection_t* conn, xcb_visualid_t visual_id) {
    (void)conn;
    (void)visual_id;
    return &stub_visual;
}

// Extension presence
const xcb_query_extension_reply_t* xcb_get_extension_data(xcb_connection_t* c, xcb_extension_t* ext) {
    (void)c;
    (void)ext;

    if (stub_ext_reply.present == 0) {
        stub_ext_reply.present = 1;
    }
    return &stub_ext_reply;
}

// XID generation
xcb_font_t xcb_generate_id(xcb_connection_t* c) {
    (void)c;
    return stub_xid_counter++;
}

// Atoms
xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t* c, uint8_t only_if_exists, uint16_t name_len,
                                         const char* name) {
    (void)c;
    (void)only_if_exists;
    (void)name_len;
    (void)name;
    return (xcb_intern_atom_cookie_t){0};
}

xcb_intern_atom_reply_t* xcb_intern_atom_reply(xcb_connection_t* c, xcb_intern_atom_cookie_t cookie,
                                               xcb_generic_error_t** e) {
    (void)c;
    (void)cookie;
    (void)e;

    xcb_intern_atom_reply_t* r = (xcb_intern_atom_reply_t*)calloc(1, sizeof(*r));
    r->atom = stub_atom_counter++;
    return r;
}

// Window creation and attributes
xcb_void_cookie_t xcb_create_window(xcb_connection_t* c, uint8_t depth, xcb_window_t wid, xcb_window_t parent,
                                    int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t border_width,
                                    uint16_t _class, xcb_visualid_t visual, uint32_t value_mask,
                                    const void* value_list) {
    (void)c;
    (void)depth;
    (void)wid;
    (void)parent;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)border_width;
    (void)_class;
    (void)visual;
    (void)value_mask;
    (void)value_list;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_create_window_checked(xcb_connection_t* c, uint8_t depth, xcb_window_t wid, xcb_window_t parent,
                                            int16_t x, int16_t y, uint16_t width, uint16_t height,
                                            uint16_t border_width, uint16_t _class, xcb_visualid_t visual,
                                            uint32_t value_mask, const void* value_list) {
    return xcb_create_window(c, depth, wid, parent, x, y, width, height, border_width, _class, visual, value_mask,
                             value_list);
}

xcb_void_cookie_t xcb_change_window_attributes(xcb_connection_t* c, xcb_window_t window, uint32_t value_mask,
                                               const void* value_list) {
    (void)c;
    (void)window;
    (void)value_mask;
    (void)value_list;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_change_window_attributes_checked(xcb_connection_t* c, xcb_window_t window, uint32_t value_mask,
                                                       const void* value_list) {
    return xcb_change_window_attributes(c, window, value_mask, value_list);
}

xcb_generic_error_t* xcb_request_check(xcb_connection_t* c, xcb_void_cookie_t cookie) {
    if (stub_poll_for_reply_hook) {
        void* reply = NULL;
        xcb_generic_error_t* err = NULL;
        if (stub_poll_for_reply_hook(c, cookie.sequence, &reply, &err)) {
            if (reply) free(reply);
            return err;
        }
    }
    (void)c;
    (void)cookie;
    return NULL;
}

xcb_void_cookie_t xcb_destroy_window(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    stub_destroy_window_count++;
    stub_last_destroyed_window = window;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_destroy_window_checked(xcb_connection_t* c, xcb_window_t window) {
    return xcb_destroy_window(c, window);
}

xcb_void_cookie_t xcb_map_window(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    stub_map_window_count++;
    stub_last_mapped_window = window;

    if (stub_mapped_windows_len < STUB_MAX_MAPPED) {
        stub_mapped_windows[stub_mapped_windows_len++] = window;
    }

    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_map_window_checked(xcb_connection_t* c, xcb_window_t window) { return xcb_map_window(c, window); }

xcb_void_cookie_t xcb_unmap_window(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    stub_unmap_window_count++;
    stub_last_unmapped_window = window;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_unmap_window_checked(xcb_connection_t* c, xcb_window_t window) {
    return xcb_unmap_window(c, window);
}

xcb_void_cookie_t xcb_configure_window(xcb_connection_t* c, xcb_window_t window, uint16_t value_mask,
                                       const void* value_list) {
    (void)c;

    stub_configure_window_count++;
    stub_last_config_window = window;
    stub_last_config_mask = value_mask;

    // Default fields for this call
    stub_last_config_sibling = 0;
    stub_last_config_stack_mode = 0;
    stub_last_config_border_width = 0;

    // Strict per-call decoding
    stub_last_config_x = 0;
    stub_last_config_y = 0;
    stub_last_config_w = 0;
    stub_last_config_h = 0;

    const uint32_t* values = (const uint32_t*)value_list;
    int i = 0;

    if (value_mask & XCB_CONFIG_WINDOW_X) stub_last_config_x = (int32_t)values[i++];
    if (value_mask & XCB_CONFIG_WINDOW_Y) stub_last_config_y = (int32_t)values[i++];
    if (value_mask & XCB_CONFIG_WINDOW_WIDTH) stub_last_config_w = values[i++];
    if (value_mask & XCB_CONFIG_WINDOW_HEIGHT) stub_last_config_h = values[i++];
    if (value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) stub_last_config_border_width = values[i++];
    if (value_mask & XCB_CONFIG_WINDOW_SIBLING) stub_last_config_sibling = values[i++];
    if (value_mask & XCB_CONFIG_WINDOW_STACK_MODE) stub_last_config_stack_mode = values[i++];

    // Record history for order-sensitive tests
    if (stub_config_calls_len < STUB_MAX_CONFIG_CALLS) {
        stub_config_call_t* call = &stub_config_calls[stub_config_calls_len++];

        call->win = window;
        call->mask = value_mask;

        call->x = stub_last_config_x;
        call->y = stub_last_config_y;
        call->w = stub_last_config_w;
        call->h = stub_last_config_h;

        call->border_width = stub_last_config_border_width;
        call->sibling = stub_last_config_sibling;
        call->stack_mode = stub_last_config_stack_mode;
    }

    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_configure_window_checked(xcb_connection_t* c, xcb_window_t window, uint16_t value_mask,
                                               const void* value_list) {
    return xcb_configure_window(c, window, value_mask, value_list);
}

// Optional helpers for tests
int stub_config_calls_count(void) { return stub_config_calls_len; }

const stub_config_call_t* stub_config_call_at(int idx) {
    if (idx < 0 || idx >= stub_config_calls_len) return NULL;
    return &stub_config_calls[idx];
}

// Properties
xcb_void_cookie_t xcb_change_property(xcb_connection_t* c, uint8_t mode, xcb_window_t window, xcb_atom_t property,
                                      xcb_atom_t type, uint8_t format, uint32_t data_len, const void* data) {
    (void)c;
    (void)mode;

    stub_last_prop_window = window;
    stub_last_prop_atom = property;
    stub_last_prop_type = type;
    stub_last_prop_len = data_len;

    uint32_t byte_len = 0;
    if (format == 8)
        byte_len = data_len;
    else if (format == 16)
        byte_len = data_len * 2;
    else if (format == 32)
        byte_len = data_len * 4;

    if (byte_len > STUB_MAX_PROP_BYTES) byte_len = STUB_MAX_PROP_BYTES;
    if (byte_len && data) memcpy(stub_last_prop_data, data, byte_len);

    if (stub_prop_calls_len < STUB_MAX_PROP_CALLS) {
        stub_prop_call_t* call = &stub_prop_calls[stub_prop_calls_len++];
        call->window = window;
        call->atom = property;
        call->type = type;
        call->format = format;
        call->len = data_len;
        call->deleted = false;
        if (byte_len && data) {
            memcpy(call->data, data, byte_len);
        } else {
            memset(call->data, 0, sizeof(call->data));
        }
    }

    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_change_property_checked(xcb_connection_t* c, uint8_t mode, xcb_window_t window,
                                              xcb_atom_t property, xcb_atom_t type, uint8_t format, uint32_t data_len,
                                              const void* data) {
    return xcb_change_property(c, mode, window, property, type, format, data_len, data);
}

xcb_void_cookie_t xcb_delete_property(xcb_connection_t* c, xcb_window_t window, xcb_atom_t property) {
    (void)c;
    stub_last_prop_window = window;
    stub_last_prop_atom = property;
    stub_last_prop_type = XCB_ATOM_NONE;
    stub_last_prop_len = 0;
    if (stub_prop_calls_len < STUB_MAX_PROP_CALLS) {
        stub_prop_call_t* call = &stub_prop_calls[stub_prop_calls_len++];
        call->window = window;
        call->atom = property;
        call->type = XCB_ATOM_NONE;
        call->format = 0;
        call->len = 0;
        call->deleted = true;
        memset(call->data, 0, sizeof(call->data));
    }
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_delete_property_checked(xcb_connection_t* c, xcb_window_t window, xcb_atom_t property) {
    return xcb_delete_property(c, window, property);
}

// Queries and replies
xcb_get_window_attributes_cookie_t xcb_get_window_attributes(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    xcb_get_window_attributes_cookie_t cookie;
    cookie.sequence = stub_cookie_seq++;
    if (stub_attr_requests_len < STUB_MAX_ATTR_REQUESTS) {
        stub_attr_requests[stub_attr_requests_len++] = (stub_attr_request_t){cookie.sequence, window};
    }
    return cookie;
}

xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t* c, xcb_drawable_t drawable) {
    (void)c;
    (void)drawable;
    xcb_get_geometry_cookie_t cookie;
    cookie.sequence = stub_cookie_seq++;
    return cookie;
}

xcb_get_property_cookie_t xcb_get_property(xcb_connection_t* c, uint8_t _delete, xcb_window_t window,
                                           xcb_atom_t property, xcb_atom_t type, uint32_t long_offset,
                                           uint32_t long_len) {
    (void)c;
    (void)_delete;
    (void)window;
    (void)property;
    (void)type;
    (void)long_offset;
    (void)long_len;
    xcb_get_property_cookie_t cookie;
    cookie.sequence = stub_cookie_seq++;
    return cookie;
}

xcb_get_property_reply_t* xcb_get_property_reply(xcb_connection_t* c, xcb_get_property_cookie_t cookie,
                                                 xcb_generic_error_t** e) {
    (void)c;
    (void)cookie;
    (void)e;

    // Return empty "no property" reply
    xcb_get_property_reply_t* r = (xcb_get_property_reply_t*)calloc(1, sizeof(*r));
    r->type = XCB_ATOM_NONE;
    r->format = 0;
    r->value_len = 0;
    return r;
}

xcb_query_tree_cookie_t xcb_query_tree(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    (void)window;
    return (xcb_query_tree_cookie_t){0};
}

xcb_query_tree_reply_t* xcb_query_tree_reply(xcb_connection_t* c, xcb_query_tree_cookie_t cookie,
                                             xcb_generic_error_t** e) {
    (void)c;
    (void)cookie;
    (void)e;

    // Valid reply; children are provided by stub helpers.
    return (xcb_query_tree_reply_t*)calloc(1, sizeof(xcb_query_tree_reply_t));
}

xcb_window_t* xcb_query_tree_children(const xcb_query_tree_reply_t* R) {
    (void)R;
    return stub_query_tree_children;
}

int xcb_query_tree_children_length(const xcb_query_tree_reply_t* R) {
    (void)R;
    return stub_query_tree_children_len;
}

void xcb_stubs_set_query_tree_children(const xcb_window_t* children, int len) {
    if (len < 0) len = 0;
    if (len > (int)(sizeof(stub_query_tree_children) / sizeof(stub_query_tree_children[0]))) {
        len = (int)(sizeof(stub_query_tree_children) / sizeof(stub_query_tree_children[0]));
    }
    memset(stub_query_tree_children, 0, sizeof(stub_query_tree_children));
    for (int i = 0; i < len; i++) {
        stub_query_tree_children[i] = children[i];
    }
    stub_query_tree_children_len = len;
}

bool xcb_stubs_attr_request_window(uint32_t seq, xcb_window_t* out_window) {
    for (int i = 0; i < stub_attr_requests_len; i++) {
        if (stub_attr_requests[i].seq == seq) {
            if (out_window) *out_window = stub_attr_requests[i].window;
            return true;
        }
    }
    return false;
}

// Input focus and grabs
xcb_void_cookie_t xcb_set_input_focus(xcb_connection_t* c, uint8_t revert_to, xcb_window_t focus,
                                      xcb_timestamp_t time) {
    (void)c;
    stub_set_input_focus_count++;
    stub_last_input_focus_window = focus;
    stub_last_input_focus_revert = revert_to;
    (void)time;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_set_input_focus_checked(xcb_connection_t* c, uint8_t revert_to, xcb_window_t focus,
                                              xcb_timestamp_t time) {
    return xcb_set_input_focus(c, revert_to, focus, time);
}

// Optional, if your WM uses this
xcb_void_cookie_t xcb_map_subwindows(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    (void)window;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_map_subwindows_checked(xcb_connection_t* c, xcb_window_t window) {
    return xcb_map_subwindows(c, window);
}

xcb_void_cookie_t xcb_grab_key(xcb_connection_t* c, uint8_t owner_events, xcb_window_t grab_window, uint16_t modifiers,
                               xcb_keycode_t key, uint8_t pointer_mode, uint8_t keyboard_mode) {
    (void)c;
    (void)owner_events;
    (void)grab_window;
    stub_grab_key_count++;
    stub_last_grab_key_mods = modifiers;
    stub_last_grab_keycode = key;
    (void)pointer_mode;
    (void)keyboard_mode;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_ungrab_key(xcb_connection_t* c, xcb_keycode_t key, xcb_window_t grab_window, uint16_t modifiers) {
    (void)c;
    (void)key;
    (void)grab_window;
    (void)modifiers;
    stub_ungrab_key_count++;
    return (xcb_void_cookie_t){0};
}

xcb_grab_pointer_cookie_t xcb_grab_pointer(xcb_connection_t* c, uint8_t owner_events, xcb_window_t grab_window,
                                           uint16_t event_mask, uint8_t pointer_mode, uint8_t keyboard_mode,
                                           xcb_window_t confine_to, xcb_cursor_t cursor, xcb_timestamp_t time) {
    (void)c;
    (void)owner_events;
    (void)grab_window;
    (void)event_mask;
    (void)pointer_mode;
    (void)keyboard_mode;
    (void)confine_to;
    stub_grab_pointer_count++;
    stub_last_grab_pointer_cursor = cursor;
    (void)time;
    return (xcb_grab_pointer_cookie_t){0};
}

xcb_grab_pointer_reply_t* xcb_grab_pointer_reply(xcb_connection_t* c, xcb_grab_pointer_cookie_t cookie,
                                                 xcb_generic_error_t** e) {
    (void)c;
    (void)cookie;
    (void)e;
    xcb_grab_pointer_reply_t* r = calloc(1, sizeof(*r));
    r->status = XCB_GRAB_STATUS_SUCCESS;
    return r;
}

xcb_void_cookie_t xcb_ungrab_pointer(xcb_connection_t* c, xcb_timestamp_t time) {
    (void)c;
    (void)time;
    stub_ungrab_pointer_count++;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_grab_button(xcb_connection_t* c, uint8_t owner_events, xcb_window_t grab_window,
                                  uint16_t event_mask, uint8_t pointer_mode, uint8_t keyboard_mode,
                                  xcb_window_t confine_to, xcb_cursor_t cursor, uint8_t button, uint16_t modifiers) {
    (void)c;
    (void)owner_events;
    (void)grab_window;
    (void)event_mask;
    (void)pointer_mode;
    (void)keyboard_mode;
    (void)confine_to;
    (void)cursor;
    (void)button;
    (void)modifiers;

    stub_grab_button_count++;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_allow_events(xcb_connection_t* c, uint8_t mode, xcb_timestamp_t time) {
    (void)c;
    (void)mode;
    (void)time;
    return (xcb_void_cookie_t){0};
}

xcb_grab_keyboard_cookie_t xcb_grab_keyboard(xcb_connection_t* c, uint8_t owner_events, xcb_window_t grab_window,
                                             xcb_timestamp_t time, uint8_t pointer_mode, uint8_t keyboard_mode) {
    (void)c;
    (void)owner_events;
    (void)grab_window;
    (void)time;
    (void)pointer_mode;
    (void)keyboard_mode;
    return (xcb_grab_keyboard_cookie_t){0};
}

xcb_void_cookie_t xcb_ungrab_keyboard(xcb_connection_t* c, xcb_timestamp_t time) {
    (void)c;
    (void)time;
    return (xcb_void_cookie_t){0};
}

// Save-set and reparenting
xcb_void_cookie_t xcb_change_save_set(xcb_connection_t* c, uint8_t mode, xcb_window_t window) {
    (void)c;
    if (mode == XCB_SET_MODE_INSERT) {
        stub_save_set_insert_count++;
    } else if (mode == XCB_SET_MODE_DELETE) {
        stub_save_set_delete_count++;
    }
    stub_last_save_set_window = window;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_reparent_window(xcb_connection_t* c, xcb_window_t window, xcb_window_t parent, int16_t x,
                                      int16_t y) {
    (void)c;
    (void)window;
    (void)parent;
    (void)x;
    (void)y;
    return (xcb_void_cookie_t){0};
}

// Event send and kill
xcb_void_cookie_t xcb_send_event(xcb_connection_t* c, uint8_t propagate, xcb_window_t destination, uint32_t event_mask,
                                 const char* event) {
    (void)c;
    (void)propagate;
    (void)event_mask;

    stub_send_event_count++;
    stub_last_send_event_destination = destination;

    if (event) memcpy(stub_last_event, event, sizeof(stub_last_event));
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_sync_await(xcb_connection_t* c, uint32_t wait_list_len,
                                 const xcb_sync_waitcondition_t* wait_list) {
    (void)c;
    (void)wait_list_len;
    (void)wait_list;
    stub_sync_await_count++;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_kill_client(xcb_connection_t* c, uint32_t resource) {
    (void)c;
    stub_kill_client_count++;
    stub_last_kill_client_resource = resource;
    return (xcb_void_cookie_t){0};
}

// Cookie draining hook support
int xcb_poll_for_reply(xcb_connection_t* c, unsigned int request, void** reply, xcb_generic_error_t** error) {
    if (stub_poll_for_reply_hook) {
        return stub_poll_for_reply_hook(c, request, reply, error);
    }

    (void)c;
    (void)request;
    (void)reply;
    (void)error;
    return 0;
}

// Cursor/font stubs
xcb_void_cookie_t xcb_open_font(xcb_connection_t* c, xcb_font_t fid, uint16_t name_len, const char* name) {
    (void)c;
    (void)fid;
    (void)name_len;
    (void)name;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_create_glyph_cursor(xcb_connection_t* c, xcb_cursor_t cid, xcb_font_t source_font,
                                          xcb_font_t mask_font, uint16_t source_char, uint16_t mask_char,
                                          uint16_t fore_red, uint16_t fore_green, uint16_t fore_blue, uint16_t back_red,
                                          uint16_t back_green, uint16_t back_blue) {
    (void)c;
    (void)cid;
    (void)source_font;
    (void)mask_font;
    (void)source_char;
    (void)mask_char;
    (void)fore_red;
    (void)fore_green;
    (void)fore_blue;
    (void)back_red;
    (void)back_green;
    (void)back_blue;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_close_font(xcb_connection_t* c, xcb_font_t font) {
    (void)c;
    (void)font;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_free_cursor(xcb_connection_t* c, xcb_cursor_t cursor) {
    (void)c;
    (void)cursor;
    return (xcb_void_cookie_t){0};
}

// Selection owner
xcb_void_cookie_t xcb_set_selection_owner(xcb_connection_t* c, xcb_window_t owner, xcb_atom_t selection,
                                          xcb_timestamp_t time) {
    (void)c;
    (void)selection;
    (void)time;
    stub_selection_owner = owner;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_set_selection_owner_checked(xcb_connection_t* c, xcb_window_t owner, xcb_atom_t selection,
                                                  xcb_timestamp_t time) {
    return xcb_set_selection_owner(c, owner, selection, time);
}

xcb_get_selection_owner_cookie_t xcb_get_selection_owner(xcb_connection_t* c, xcb_atom_t selection) {
    (void)c;
    (void)selection;
    return (xcb_get_selection_owner_cookie_t){0};
}

xcb_get_selection_owner_reply_t* xcb_get_selection_owner_reply(xcb_connection_t* c,
                                                               xcb_get_selection_owner_cookie_t cookie,
                                                               xcb_generic_error_t** e) {
    (void)c;
    (void)cookie;
    (void)e;

    xcb_get_selection_owner_reply_t* r = (xcb_get_selection_owner_reply_t*)calloc(1, sizeof(*r));
    r->owner = stub_selection_owner;
    return r;
}

// Stub helpers for selection ownership
void xcb_stubs_set_selection_owner(xcb_window_t owner) { stub_selection_owner = owner; }

xcb_window_t xcb_stubs_get_selection_owner(void) { return stub_selection_owner; }

// Event queue helpers
static bool stub_events_push(xcb_generic_event_t** buf, size_t* head, size_t* len, xcb_generic_event_t* ev) {
    if (*len >= STUB_MAX_EVENTS) return false;
    size_t idx = (*head + *len) % STUB_MAX_EVENTS;
    buf[idx] = ev;
    (*len)++;
    return true;
}

static xcb_generic_event_t* stub_events_pop(xcb_generic_event_t** buf, size_t* head, size_t* len) {
    if (*len == 0) return NULL;
    xcb_generic_event_t* ev = buf[*head];
    buf[*head] = NULL;
    *head = (*head + 1) % STUB_MAX_EVENTS;
    (*len)--;
    return ev;
}

bool xcb_stubs_enqueue_queued_event(xcb_generic_event_t* ev) {
    return stub_events_push(stub_queued_events, &stub_queued_head, &stub_queued_len, ev);
}

bool xcb_stubs_enqueue_event(xcb_generic_event_t* ev) {
    return stub_events_push(stub_events, &stub_event_head, &stub_event_len, ev);
}

size_t xcb_stubs_queued_event_len(void) { return stub_queued_len; }

size_t xcb_stubs_event_len(void) { return stub_event_len; }

xcb_generic_event_t* xcb_poll_for_queued_event(xcb_connection_t* c) {
    (void)c;
    return stub_events_pop(stub_queued_events, &stub_queued_head, &stub_queued_len);
}

xcb_generic_event_t* xcb_poll_for_event(xcb_connection_t* c) {
    (void)c;
    return stub_events_pop(stub_events, &stub_event_head, &stub_event_len);
}

// GC and drawing
xcb_void_cookie_t xcb_create_gc(xcb_connection_t* c, xcb_gcontext_t cid, xcb_drawable_t drawable, uint32_t value_mask,
                                const void* value_list) {
    (void)c;
    (void)cid;
    (void)drawable;
    (void)value_mask;
    (void)value_list;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_free_gc(xcb_connection_t* c, xcb_gcontext_t gc) {
    (void)c;
    (void)gc;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_poly_fill_rectangle(xcb_connection_t* c, xcb_drawable_t drawable, xcb_gcontext_t gc,
                                          uint32_t rectangles_len, const xcb_rectangle_t* rectangles) {
    (void)c;
    (void)drawable;
    (void)gc;
    (void)rectangles_len;
    (void)rectangles;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_image_text_8(xcb_connection_t* c, uint8_t string_len, xcb_drawable_t drawable, xcb_gcontext_t gc,
                                   int16_t x, int16_t y, const char* string) {
    (void)c;
    (void)string_len;
    (void)drawable;
    (void)gc;
    (void)x;
    (void)y;
    (void)string;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_poly_line(xcb_connection_t* c, uint8_t coordinate_mode, xcb_drawable_t drawable,
                                xcb_gcontext_t gc, uint32_t points_len, const xcb_point_t* points) {
    (void)c;
    (void)coordinate_mode;
    (void)drawable;
    (void)gc;
    (void)points_len;
    (void)points;
    return (xcb_void_cookie_t){0};
}

// Pixmaps and blits
xcb_void_cookie_t xcb_create_pixmap(xcb_connection_t* c, uint8_t depth, xcb_pixmap_t pid, xcb_drawable_t drawable,
                                    uint16_t width, uint16_t height) {
    (void)c;
    (void)depth;
    (void)pid;
    (void)drawable;
    (void)width;
    (void)height;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_free_pixmap(xcb_connection_t* c, xcb_pixmap_t pixmap) {
    (void)c;
    (void)pixmap;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_copy_area(xcb_connection_t* c, xcb_drawable_t src_drawable, xcb_drawable_t dst_drawable,
                                xcb_gcontext_t gc, int16_t src_x, int16_t src_y, int16_t dst_x, int16_t dst_y,
                                uint16_t width, uint16_t height) {
    (void)c;
    (void)src_drawable;
    (void)dst_drawable;
    (void)gc;
    (void)src_x;
    (void)src_y;
    (void)dst_x;
    (void)dst_y;
    (void)width;
    (void)height;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_put_image(xcb_connection_t* c, uint8_t format, xcb_drawable_t drawable, xcb_gcontext_t gc,
                                uint16_t width, uint16_t height, int16_t dst_x, int16_t dst_y, uint8_t left_pad,
                                uint8_t depth, uint32_t data_len, const uint8_t* data) {
    (void)c;
    (void)format;
    (void)drawable;
    (void)gc;
    (void)width;
    (void)height;
    (void)dst_x;
    (void)dst_y;
    (void)left_pad;
    (void)depth;
    (void)data_len;
    (void)data;
    return (xcb_void_cookie_t){0};
}

// xcb-keysyms minimal mocks
typedef struct xcb_key_symbols_t {
    int dummy;
} xcb_key_symbols_t;

xcb_key_symbols_t* xcb_key_symbols_alloc(xcb_connection_t* c) {
    (void)c;
    return (xcb_key_symbols_t*)malloc(sizeof(xcb_key_symbols_t));
}

void xcb_key_symbols_free(xcb_key_symbols_t* syms) { free(syms); }

xcb_keycode_t* xcb_key_symbols_get_keycode(xcb_key_symbols_t* syms, xcb_keysym_t keysym) {
    (void)syms;
    xcb_keycode_t* codes = calloc(2, sizeof(*codes));
    if (!codes) return NULL;
    codes[0] = (xcb_keycode_t)(keysym ? (keysym & 0xFF) : 42);
    codes[1] = 0;
    return codes;
}

xcb_keysym_t xcb_key_symbols_get_keysym(xcb_key_symbols_t* syms, xcb_keycode_t keycode, int col) {
    (void)syms;
    (void)col;
    if (keycode == 9) return XK_Escape;
    return 0;
}

// Colormap stubs
xcb_void_cookie_t xcb_install_colormap(xcb_connection_t* c, xcb_colormap_t cmap) {
    (void)c;
    stub_install_colormap_count++;
    stub_last_installed_colormap = cmap;
    (void)cmap;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_install_colormap_checked(xcb_connection_t* c, xcb_colormap_t cmap) {
    return xcb_install_colormap(c, cmap);
}

xcb_void_cookie_t xcb_create_colormap(xcb_connection_t* c, uint8_t alloc, xcb_colormap_t mid, xcb_window_t window,
                                      xcb_visualid_t visual) {
    (void)c;
    (void)alloc;
    (void)mid;
    (void)window;
    (void)visual;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_create_colormap_checked(xcb_connection_t* c, uint8_t alloc, xcb_colormap_t mid,
                                              xcb_window_t window, xcb_visualid_t visual) {
    return xcb_create_colormap(c, alloc, mid, window, visual);
}

xcb_void_cookie_t xcb_free_colormap(xcb_connection_t* c, xcb_colormap_t cmap) {
    (void)c;
    (void)cmap;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_free_colormap_checked(xcb_connection_t* c, xcb_colormap_t cmap) {
    return xcb_free_colormap(c, cmap);
}

// RandR stubs
xcb_randr_get_screen_resources_current_cookie_t xcb_randr_get_screen_resources_current(xcb_connection_t* c,
                                                                                       xcb_window_t window) {
    (void)c;
    (void)window;
    return (xcb_randr_get_screen_resources_current_cookie_t){stub_cookie_seq++};
}

xcb_randr_get_screen_resources_current_reply_t* xcb_randr_get_screen_resources_current_reply(
    xcb_connection_t* c, xcb_randr_get_screen_resources_current_cookie_t cookie, xcb_generic_error_t** e) {
    (void)c;
    (void)cookie;
    (void)e;
    xcb_randr_get_screen_resources_current_reply_t* r = calloc(1, sizeof(*r));
    r->num_crtcs = 0;
    return r;
}

xcb_randr_crtc_t* xcb_randr_get_screen_resources_current_crtcs(
    const xcb_randr_get_screen_resources_current_reply_t* R) {
    (void)R;
    return NULL;
}

int xcb_randr_get_screen_resources_current_crtcs_length(const xcb_randr_get_screen_resources_current_reply_t* R) {
    return (int)R->num_crtcs;
}

xcb_randr_get_crtc_info_cookie_t xcb_randr_get_crtc_info(xcb_connection_t* c, xcb_randr_crtc_t crtc,
                                                         xcb_timestamp_t config_timestamp) {
    (void)c;
    (void)crtc;
    (void)config_timestamp;
    return (xcb_randr_get_crtc_info_cookie_t){stub_cookie_seq++};
}

xcb_randr_get_crtc_info_reply_t* xcb_randr_get_crtc_info_reply(xcb_connection_t* c,
                                                               xcb_randr_get_crtc_info_cookie_t cookie,
                                                               xcb_generic_error_t** e) {
    (void)c;
    (void)cookie;
    (void)e;
    return NULL;
}

xcb_void_cookie_t xcb_randr_select_input(xcb_connection_t* c, xcb_window_t window, uint16_t enable) {
    (void)c;
    (void)window;
    (void)enable;
    return (xcb_void_cookie_t){0};
}

xcb_randr_query_version_cookie_t xcb_randr_query_version(xcb_connection_t* c, uint32_t major_version,
                                                         uint32_t minor_version) {
    (void)c;
    (void)major_version;
    (void)minor_version;
    return (xcb_randr_query_version_cookie_t){stub_cookie_seq++};
}

xcb_randr_query_version_reply_t* xcb_randr_query_version_reply(xcb_connection_t* c,
                                                               xcb_randr_query_version_cookie_t cookie,
                                                               xcb_generic_error_t** e) {
    (void)c;
    (void)cookie;
    (void)e;
    xcb_randr_query_version_reply_t* r = calloc(1, sizeof(*r));
    r->major_version = 1;
    r->minor_version = 5;
    return r;
}
