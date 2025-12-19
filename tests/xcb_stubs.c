#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

xcb_get_window_attributes_cookie_t xcb_get_window_attributes(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    (void)window;
    return (xcb_get_window_attributes_cookie_t){0};
}

xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t* c, xcb_drawable_t drawable) {
    (void)c;
    (void)drawable;
    return (xcb_get_geometry_cookie_t){0};
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
    return (xcb_get_property_cookie_t){0};
}

int xcb_flush(xcb_connection_t* c) {
    (void)c;
    return 1;
}

static xcb_screen_t stub_screen = {.root = 1, .width_in_pixels = 1920, .height_in_pixels = 1080};
static xcb_visualtype_t stub_visual = {.visual_id = 1};

xcb_visualtype_t* xcb_get_visualtype(xcb_connection_t* conn, xcb_visualid_t visual_id) {
    (void)conn;
    (void)visual_id;
    return &stub_visual;
}

xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t* R) {
    (void)R;
    xcb_screen_iterator_t i;
    memset(&i, 0, sizeof(i));
    i.data = &stub_screen;
    return i;
}

const xcb_setup_t* xcb_get_setup(xcb_connection_t* c) {
    (void)c;
    return NULL;
}

xcb_void_cookie_t xcb_change_window_attributes(xcb_connection_t* c, xcb_window_t window, uint32_t value_mask,
                                               const void* value_list) {
    (void)c;
    (void)window;
    (void)value_mask;
    (void)value_list;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_change_property(xcb_connection_t* c, uint8_t mode, xcb_window_t window, xcb_atom_t property,
                                      xcb_atom_t type, uint8_t format, uint32_t data_len, const void* data) {
    (void)c;
    (void)mode;
    (void)window;
    (void)property;
    (void)type;
    (void)format;
    (void)data_len;
    (void)data;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_set_input_focus(xcb_connection_t* c, uint8_t revert_to, xcb_window_t focus,
                                      xcb_timestamp_t time) {
    (void)c;
    (void)revert_to;
    (void)focus;
    (void)time;
    return (xcb_void_cookie_t){0};
}

// Global counters for testing
int stub_map_window_count = 0;
int stub_unmap_window_count = 0;
xcb_window_t stub_last_mapped_window = 0;
xcb_window_t stub_last_unmapped_window = 0;

int stub_send_event_count = 0;
xcb_window_t stub_last_send_event_destination = 0;
char stub_last_event[32];  // Enough for ClientMessage

int stub_kill_client_count = 0;
uint32_t stub_last_kill_client_resource = 0;
int stub_grab_button_count = 0;

xcb_void_cookie_t xcb_map_window(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    stub_map_window_count++;
    stub_last_mapped_window = window;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_unmap_window(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    stub_unmap_window_count++;
    stub_last_unmapped_window = window;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_configure_window(xcb_connection_t* c, xcb_window_t window, uint16_t value_mask,
                                       const void* value_list) {
    (void)c;
    (void)window;
    (void)value_mask;
    (void)value_list;
    return (xcb_void_cookie_t){0};
}

xcb_connection_t* xcb_connect(const char* displayname, int* screenp) {
    (void)displayname;
    (void)screenp;
    return (xcb_connection_t*)malloc(1);  // Return dummy non-null
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

xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t* c, uint8_t only_if_exists, uint16_t name_len,
                                         const char* name) {
    (void)c;
    (void)only_if_exists;
    (void)name_len;
    (void)name;
    return (xcb_intern_atom_cookie_t){0};
}

static xcb_atom_t stub_atom_counter = 1;

xcb_intern_atom_reply_t* xcb_intern_atom_reply(xcb_connection_t* c, xcb_intern_atom_cookie_t cookie,
                                               xcb_generic_error_t** e) {
    (void)c;
    (void)cookie;
    (void)e;
    xcb_intern_atom_reply_t* reply = calloc(1, sizeof(xcb_intern_atom_reply_t));
    reply->atom = stub_atom_counter++;
    return reply;
}

xcb_font_t xcb_generate_id(xcb_connection_t* c) {
    (void)c;
    return 1;
}

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

xcb_void_cookie_t xcb_set_selection_owner(xcb_connection_t* c, xcb_window_t owner, xcb_atom_t selection,
                                          xcb_timestamp_t time) {
    (void)c;
    (void)owner;
    (void)selection;
    (void)time;
    return (xcb_void_cookie_t){0};
}

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

xcb_void_cookie_t xcb_delete_property(xcb_connection_t* c, xcb_window_t window, xcb_atom_t property) {
    (void)c;
    (void)window;
    (void)property;
    return (xcb_void_cookie_t){0};
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
    return NULL;  // No children
}

xcb_window_t* xcb_query_tree_children(const xcb_query_tree_reply_t* R) {
    (void)R;
    return NULL;
}

int xcb_query_tree_children_length(const xcb_query_tree_reply_t* R) {
    (void)R;
    return 0;
}

// xcb_keysyms mock
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
    (void)keysym;
    return NULL;
}

#include <X11/keysym.h>

xcb_keysym_t xcb_key_symbols_get_keysym(xcb_key_symbols_t* syms, xcb_keycode_t keycode, int col) {
    (void)syms;
    (void)col;
    if (keycode == 9) return XK_Escape;
    return 0;
}

xcb_void_cookie_t xcb_grab_key(xcb_connection_t* c, uint8_t owner_events, xcb_window_t grab_window, uint16_t modifiers,
                               xcb_keycode_t key, uint8_t pointer_mode, uint8_t keyboard_mode) {
    (void)c;
    (void)owner_events;
    (void)grab_window;
    (void)modifiers;
    (void)key;
    (void)pointer_mode;
    (void)keyboard_mode;
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
    (void)cursor;
    (void)time;
    return (xcb_grab_pointer_cookie_t){0};
}

xcb_void_cookie_t xcb_ungrab_pointer(xcb_connection_t* c, xcb_timestamp_t time) {
    (void)c;
    (void)time;
    return (xcb_void_cookie_t){0};
}

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

xcb_void_cookie_t xcb_change_save_set(xcb_connection_t* c, uint8_t mode, xcb_window_t window) {
    (void)c;
    (void)mode;
    (void)window;
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

xcb_void_cookie_t xcb_destroy_window(xcb_connection_t* c, xcb_window_t window) {
    (void)c;
    (void)window;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_send_event(xcb_connection_t* c, uint8_t propagate, xcb_window_t destination, uint32_t event_mask,
                                 const char* event) {
    (void)c;
    (void)propagate;
    stub_send_event_count++;
    stub_last_send_event_destination = destination;
    if (event) memcpy(stub_last_event, event, 32);
    (void)event_mask;
    return (xcb_void_cookie_t){0};
}

xcb_void_cookie_t xcb_kill_client(xcb_connection_t* c, uint32_t resource) {
    (void)c;
    stub_kill_client_count++;
    stub_last_kill_client_resource = resource;
    return (xcb_void_cookie_t){0};
}

int xcb_poll_for_reply(xcb_connection_t* c, unsigned int request, void** reply, xcb_generic_error_t** error) {
    (void)c;
    (void)request;
    (void)reply;
    (void)error;
    return 0;  // No reply ready
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