#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static void die(const char* msg) {
    fprintf(stderr, "x_test_client: %s\n", msg);
    exit(1);
}

static xcb_atom_t get_atom(xcb_connection_t* conn, const char* name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, NULL);
    if (!reply) die("failed to intern atom");
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

static uint32_t parse_u32(const char* str) {
    errno = 0;
    char* end = NULL;
    unsigned long val = strtoul(str, &end, 0);
    if (errno != 0 || !end || *end != '\0' || val > UINT32_MAX) {
        die("invalid numeric argument");
    }
    return (uint32_t)val;
}

static void print_cardinal_json(uint32_t* vals, int count) {
    printf("{\"values\":[");
    for (int i = 0; i < count; i++) {
        if (i > 0) printf(",");
        printf("%" PRIu32, vals[i]);
    }
    printf("]}\n");
}

static void usage(void) {
    fprintf(stderr,
            "Usage:\n"
            "  x_test_client get-atom <name>\n"
            "  x_test_client create-window\n"
            "  x_test_client map-window <window>\n"
            "  x_test_client get-root-cardinals <atom>\n"
            "  x_test_client get-window-cardinals <window> <atom>\n"
            "  x_test_client set-window-cardinals <window> <atom> <v...>\n"
            "  x_test_client set-window-atoms <window> <atom> <atom...>\n"
            "  x_test_client set-window-string <window> <atom> <type> <value>\n"
            "  x_test_client set-window-empty <window> <atom> <type>\n"
            "  x_test_client delete-window-prop <window> <atom>\n"
            "  x_test_client send-client-message <window> <atom> <d0> <d1> <d2> <d3> <d4>\n");
    exit(2);
}

int main(int argc, char** argv) {
    if (argc < 2) usage();

    xcb_connection_t* conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) die("cannot connect to X server");

    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    if (!screen) die("no screen");
    xcb_window_t root = screen->root;

    const char* cmd = argv[1];

    if (strcmp(cmd, "get-atom") == 0) {
        if (argc != 3) usage();
        xcb_atom_t atom = get_atom(conn, argv[2]);
        printf("%" PRIu32 "\n", atom);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "create-window") == 0) {
        xcb_window_t win = xcb_generate_id(conn);
        uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE};
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, root, 0, 0, 100, 100, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual, mask, values);
        xcb_flush(conn);
        printf("%" PRIu32 "\n", win);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "map-window") == 0) {
        if (argc != 3) usage();
        xcb_window_t win = (xcb_window_t)parse_u32(argv[2]);
        xcb_map_window(conn, win);
        xcb_flush(conn);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "get-root-cardinals") == 0) {
        if (argc != 3) usage();
        xcb_atom_t atom = get_atom(conn, argv[2]);
        xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, root, atom, XCB_ATOM_CARDINAL, 0, UINT32_MAX);
        xcb_get_property_reply_t* rep = xcb_get_property_reply(conn, ck, NULL);
        if (!rep) die("property reply failed");
        int len = xcb_get_property_value_length(rep) / 4;
        uint32_t* vals = (uint32_t*)xcb_get_property_value(rep);
        print_cardinal_json(vals, len);
        free(rep);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "get-window-cardinals") == 0) {
        if (argc != 4) usage();
        xcb_window_t win = (xcb_window_t)parse_u32(argv[2]);
        xcb_atom_t atom = get_atom(conn, argv[3]);
        xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, win, atom, XCB_ATOM_CARDINAL, 0, UINT32_MAX);
        xcb_get_property_reply_t* rep = xcb_get_property_reply(conn, ck, NULL);
        if (!rep) die("property reply failed");
        int len = xcb_get_property_value_length(rep) / 4;
        uint32_t* vals = (uint32_t*)xcb_get_property_value(rep);
        print_cardinal_json(vals, len);
        free(rep);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "set-window-cardinals") == 0) {
        if (argc < 4) usage();
        xcb_window_t win = (xcb_window_t)parse_u32(argv[2]);
        xcb_atom_t atom = get_atom(conn, argv[3]);
        int count = argc - 4;
        uint32_t* vals = NULL;
        if (count > 0) {
            vals = calloc((size_t)count, sizeof(uint32_t));
            if (!vals) die("alloc failed");
            for (int i = 0; i < count; i++) {
                vals[i] = parse_u32(argv[4 + i]);
            }
        }
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atom, XCB_ATOM_CARDINAL, 32, count, vals);
        free(vals);
        xcb_flush(conn);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "set-window-atoms") == 0) {
        if (argc < 5) usage();
        xcb_window_t win = (xcb_window_t)parse_u32(argv[2]);
        xcb_atom_t atom = get_atom(conn, argv[3]);
        int count = argc - 4;
        xcb_atom_t* vals = calloc((size_t)count, sizeof(xcb_atom_t));
        if (!vals) die("alloc failed");
        for (int i = 0; i < count; i++) {
            vals[i] = get_atom(conn, argv[4 + i]);
        }
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atom, XCB_ATOM_ATOM, 32, count, vals);
        free(vals);
        xcb_flush(conn);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "set-window-string") == 0) {
        if (argc != 6) usage();
        xcb_window_t win = (xcb_window_t)parse_u32(argv[2]);
        xcb_atom_t atom = get_atom(conn, argv[3]);
        xcb_atom_t type = get_atom(conn, argv[4]);
        const char* value = argv[5];
        uint32_t len = (uint32_t)strlen(value);
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atom, type, 8, len, value);
        xcb_flush(conn);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "set-window-empty") == 0) {
        if (argc != 5) usage();
        xcb_window_t win = (xcb_window_t)parse_u32(argv[2]);
        xcb_atom_t atom = get_atom(conn, argv[3]);
        xcb_atom_t type = get_atom(conn, argv[4]);
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atom, type, 32, 0, NULL);
        xcb_flush(conn);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "delete-window-prop") == 0) {
        if (argc != 4) usage();
        xcb_window_t win = (xcb_window_t)parse_u32(argv[2]);
        xcb_atom_t atom = get_atom(conn, argv[3]);
        xcb_delete_property(conn, win, atom);
        xcb_flush(conn);
        xcb_disconnect(conn);
        return 0;
    }

    if (strcmp(cmd, "send-client-message") == 0) {
        if (argc != 9) usage();
        xcb_window_t win = (xcb_window_t)parse_u32(argv[2]);
        xcb_atom_t atom = get_atom(conn, argv[3]);
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = win ? win : root;
        ev.type = atom;
        for (int i = 0; i < 5; i++) {
            ev.data.data32[i] = parse_u32(argv[4 + i]);
        }
        xcb_send_event(conn, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                       (const char*)&ev);
        xcb_flush(conn);
        xcb_disconnect(conn);
        return 0;
    }

    usage();
    return 1;
}
