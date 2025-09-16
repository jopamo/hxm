#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>

#define WM_STATE_NORMAL 1

typedef struct {
    xcb_atom_t net_supporting_wm_check;
    xcb_atom_t net_wm_window_type;
    xcb_atom_t net_wm_window_type_dock;
    xcb_atom_t net_wm_window_type_desktop;
    xcb_atom_t net_wm_state;
    xcb_atom_t net_wm_state_below;
    xcb_atom_t net_wm_state_sticky;
    xcb_atom_t net_wm_state_skip_taskbar;
    xcb_atom_t net_wm_state_skip_pager;
    xcb_atom_t net_client_list_stacking;
    xcb_atom_t wm_state;
    xcb_atom_t wm_hints;
    xcb_atom_t wm_normal_hints;
    xcb_atom_t wm_class;
    xcb_atom_t net_wm_name;
    xcb_atom_t wm_name;
    xcb_atom_t utf8_string;
} atom_set_t;

static void die(const char* msg) {
    fprintf(stderr, "conky_probe: %s\n", msg);
    exit(1);
}

static void dief(const char* msg) {
    fprintf(stderr, "conky_probe: %s (%s)\n", msg, strerror(errno));
    exit(1);
}

static xcb_atom_t get_atom(xcb_connection_t* conn, const char* name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, NULL);
    if (!reply) return XCB_ATOM_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

static void sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static bool contains_case_insensitive(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nlen = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i]) {
            char hc = (char)tolower((unsigned char)p[i]);
            char nc = (char)tolower((unsigned char)needle[i]);
            if (hc != nc) break;
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static xcb_connection_t* connect_with_retry(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (waited <= timeout_ms) {
        xcb_connection_t* conn = xcb_connect(NULL, NULL);
        if (!xcb_connection_has_error(conn)) return conn;
        xcb_disconnect(conn);
        sleep_ms(100);
        waited += 100;
    }
    return NULL;
}

static bool property_exists(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t prop) {
    xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, win, prop, XCB_ATOM_ANY, 0, 128);
    xcb_get_property_reply_t* rep = xcb_get_property_reply(conn, ck, NULL);
    if (!rep) return false;
    bool ok = (rep->type != XCB_ATOM_NONE && rep->value_len > 0);
    free(rep);
    return ok;
}

static bool fetch_atom_list(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t prop, xcb_atom_t** atoms_out,
                            uint32_t* len_out) {
    xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, win, prop, XCB_ATOM_ATOM, 0, 32);
    xcb_get_property_reply_t* rep = xcb_get_property_reply(conn, ck, NULL);
    if (!rep) return false;
    if (rep->type == XCB_ATOM_NONE || rep->format != 32) {
        free(rep);
        return false;
    }
    uint32_t len = (uint32_t)(xcb_get_property_value_length(rep) / sizeof(xcb_atom_t));
    xcb_atom_t* atoms = NULL;
    if (len > 0) {
        atoms = malloc(len * sizeof(xcb_atom_t));
        if (!atoms) {
            free(rep);
            die("alloc failed");
        }
        memcpy(atoms, xcb_get_property_value(rep), len * sizeof(xcb_atom_t));
    }
    free(rep);
    *atoms_out = atoms;
    *len_out = len;
    return true;
}

static bool atom_list_contains(const xcb_atom_t* atoms, uint32_t len, xcb_atom_t needle) {
    for (uint32_t i = 0; i < len; i++) {
        if (atoms[i] == needle) return true;
    }
    return false;
}

static char* get_text_property(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t prop) {
    xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, win, prop, XCB_ATOM_ANY, 0, 1024);
    xcb_get_property_reply_t* rep = xcb_get_property_reply(conn, ck, NULL);
    if (!rep) return NULL;
    if (rep->type == XCB_ATOM_NONE || rep->value_len == 0) {
        free(rep);
        return NULL;
    }
    int len = xcb_get_property_value_length(rep);
    const char* src = (const char*)xcb_get_property_value(rep);
    char* out = calloc((size_t)len + 1, 1);
    if (!out) {
        free(rep);
        die("alloc failed");
    }
    memcpy(out, src, (size_t)len);
    out[len] = '\0';
    free(rep);
    return out;
}

static bool window_matches_class(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t wm_class, const char* match) {
    if (!match || !*match || wm_class == XCB_ATOM_NONE) return false;
    char* data = get_text_property(conn, win, wm_class);
    if (!data) return false;
    bool ok = false;
    if (contains_case_insensitive(data, match)) ok = true;
    free(data);
    return ok;
}

static bool window_matches_name(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t net_wm_name, xcb_atom_t wm_name,
                                const char* match) {
    if (!match || !*match) return false;
    char* name = NULL;
    if (net_wm_name != XCB_ATOM_NONE) name = get_text_property(conn, win, net_wm_name);
    if (!name && wm_name != XCB_ATOM_NONE) name = get_text_property(conn, win, wm_name);
    if (!name) return false;
    bool ok = contains_case_insensitive(name, match);
    free(name);
    return ok;
}

static bool wm_state_is_normal(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t wm_state) {
    xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, win, wm_state, wm_state, 0, 2);
    xcb_get_property_reply_t* rep = xcb_get_property_reply(conn, ck, NULL);
    if (!rep) return false;
    bool ok = false;
    if (rep->type == wm_state && rep->format == 32 && rep->value_len >= 1) {
        uint32_t* vals = (uint32_t*)xcb_get_property_value(rep);
        ok = (vals[0] == WM_STATE_NORMAL);
    }
    free(rep);
    return ok;
}

static bool get_window_geometry_on_root(xcb_connection_t* conn, xcb_window_t win, xcb_window_t root, int16_t* rx,
                                        int16_t* ry, uint16_t* w, uint16_t* h) {
    xcb_get_geometry_cookie_t gck = xcb_get_geometry(conn, win);
    xcb_get_geometry_reply_t* greply = xcb_get_geometry_reply(conn, gck, NULL);
    if (!greply) return false;
    *w = greply->width;
    *h = greply->height;

    xcb_translate_coordinates_cookie_t tck = xcb_translate_coordinates(conn, win, root, 0, 0);
    xcb_translate_coordinates_reply_t* treply = xcb_translate_coordinates_reply(conn, tck, NULL);
    if (!treply) {
        free(greply);
        return false;
    }
    *rx = treply->dst_x;
    *ry = treply->dst_y;
    free(greply);
    free(treply);
    return true;
}

static bool get_root_stacking_list(xcb_connection_t* conn, xcb_window_t root, xcb_atom_t prop, xcb_window_t** out,
                                   uint32_t* len_out) {
    xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, root, prop, XCB_ATOM_WINDOW, 0, 1024);
    xcb_get_property_reply_t* rep = xcb_get_property_reply(conn, ck, NULL);
    if (!rep) return false;
    if (rep->type == XCB_ATOM_NONE || rep->format != 32) {
        free(rep);
        return false;
    }
    uint32_t len = (uint32_t)(xcb_get_property_value_length(rep) / sizeof(xcb_window_t));
    if (len == 0) {
        free(rep);
        return false;
    }
    xcb_window_t* list = malloc(len * sizeof(xcb_window_t));
    if (!list) {
        free(rep);
        die("alloc failed");
    }
    memcpy(list, xcb_get_property_value(rep), len * sizeof(xcb_window_t));
    free(rep);
    *out = list;
    *len_out = len;
    return true;
}

static size_t append_unique(xcb_window_t* out, size_t count, size_t cap, xcb_window_t win) {
    for (size_t i = 0; i < count; i++) {
        if (out[i] == win) return count;
    }
    if (count < cap) out[count++] = win;
    return count;
}

static size_t collect_candidate_windows(xcb_connection_t* conn, xcb_window_t root, xcb_atom_t stacking_prop,
                                        xcb_window_t* out, size_t cap, bool* used_stacking) {
    *used_stacking = false;
    xcb_window_t* list = NULL;
    uint32_t len = 0;
    if (stacking_prop != XCB_ATOM_NONE && get_root_stacking_list(conn, root, stacking_prop, &list, &len)) {
        size_t count = 0;
        for (uint32_t i = 0; i < len && count < cap; i++) {
            count = append_unique(out, count, cap, list[i]);
        }
        free(list);
        *used_stacking = true;
        return count;
    }

    xcb_query_tree_cookie_t ck = xcb_query_tree(conn, root);
    xcb_query_tree_reply_t* rep = xcb_query_tree_reply(conn, ck, NULL);
    if (!rep) return 0;
    int child_len = xcb_query_tree_children_length(rep);
    xcb_window_t* children = xcb_query_tree_children(rep);

    size_t count = 0;
    for (int i = 0; i < child_len && count < cap; i++) {
        xcb_window_t child = children[i];
        xcb_query_tree_cookie_t ck_child = xcb_query_tree(conn, child);
        xcb_query_tree_reply_t* rep_child = xcb_query_tree_reply(conn, ck_child, NULL);
        if (rep_child) {
            int gc_len = xcb_query_tree_children_length(rep_child);
            xcb_window_t* gchildren = xcb_query_tree_children(rep_child);
            if (gc_len > 0) {
                for (int j = 0; j < gc_len && count < cap; j++) {
                    count = append_unique(out, count, cap, gchildren[j]);
                }
            } else {
                count = append_unique(out, count, cap, child);
            }
            free(rep_child);
        } else {
            count = append_unique(out, count, cap, child);
        }
    }

    free(rep);
    return count;
}

static bool wait_for_wm_ready(xcb_connection_t* conn, xcb_window_t root, xcb_atom_t prop, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (waited <= timeout_ms) {
        xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, root, prop, XCB_ATOM_WINDOW, 0, 1);
        xcb_get_property_reply_t* rep = xcb_get_property_reply(conn, ck, NULL);
        if (rep && rep->type == XCB_ATOM_WINDOW && rep->format == 32 && rep->value_len >= 1) {
            xcb_window_t* win = (xcb_window_t*)xcb_get_property_value(rep);
            if (win && win[0] != XCB_NONE) {
                free(rep);
                return true;
            }
        }
        if (rep) free(rep);
        sleep_ms(50);
        waited += 50;
    }
    return false;
}

static const char* getenv_default(const char* key, const char* def) {
    const char* val = getenv(key);
    return val && *val ? val : def;
}

static int getenv_int(const char* key, int def) {
    const char* val = getenv(key);
    if (!val || !*val) return def;
    char* end = NULL;
    long parsed = strtol(val, &end, 10);
    if (!end || *end != '\0') return def;
    return (int)parsed;
}

static bool getenv_bool(const char* key, bool def) {
    const char* val = getenv(key);
    if (!val || !*val) return def;
    return strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0;
}

int main(void) {
    int connect_timeout = getenv_int("CONNECT_TIMEOUT_MS", 3000);
    int wm_timeout = getenv_int("WM_TIMEOUT_MS", 5000);
    int window_timeout = getenv_int("WINDOW_TIMEOUT_MS", 5000);

    xcb_connection_t* conn = connect_with_retry((uint32_t)connect_timeout);
    if (!conn) die("unable to connect to X server");

    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    if (!screen) die("no screen");

    atom_set_t atoms = {0};
    atoms.net_supporting_wm_check = get_atom(conn, "_NET_SUPPORTING_WM_CHECK");
    atoms.net_wm_window_type = get_atom(conn, "_NET_WM_WINDOW_TYPE");
    atoms.net_wm_window_type_dock = get_atom(conn, "_NET_WM_WINDOW_TYPE_DOCK");
    atoms.net_wm_window_type_desktop = get_atom(conn, "_NET_WM_WINDOW_TYPE_DESKTOP");
    atoms.net_wm_state = get_atom(conn, "_NET_WM_STATE");
    atoms.net_wm_state_below = get_atom(conn, "_NET_WM_STATE_BELOW");
    atoms.net_wm_state_sticky = get_atom(conn, "_NET_WM_STATE_STICKY");
    atoms.net_wm_state_skip_taskbar = get_atom(conn, "_NET_WM_STATE_SKIP_TASKBAR");
    atoms.net_wm_state_skip_pager = get_atom(conn, "_NET_WM_STATE_SKIP_PAGER");
    atoms.net_client_list_stacking = get_atom(conn, "_NET_CLIENT_LIST_STACKING");
    atoms.wm_state = get_atom(conn, "WM_STATE");
    atoms.wm_hints = get_atom(conn, "WM_HINTS");
    atoms.wm_normal_hints = get_atom(conn, "WM_NORMAL_HINTS");
    atoms.wm_class = get_atom(conn, "WM_CLASS");
    atoms.net_wm_name = get_atom(conn, "_NET_WM_NAME");
    atoms.wm_name = get_atom(conn, "WM_NAME");
    atoms.utf8_string = get_atom(conn, "UTF8_STRING");

    if (!wait_for_wm_ready(conn, screen->root, atoms.net_supporting_wm_check, (uint32_t)wm_timeout)) {
        die("WM did not become ready in time");
    }

    const char* conky_class_match = getenv_default("CONKY_CLASS_MATCH", "Conky");
    const char* conky_name_match = getenv_default("CONKY_NAME_MATCH", "hxm-conky-test");
    const char* normal_class_match = getenv_default("NORMAL_CLASS_MATCH", "HxmNormal");
    const char* normal_name_match = getenv_default("NORMAL_NAME_MATCH", "hxm-normal");
    const char* expect_type = getenv_default("EXPECT_TYPE", "DOCK");

    bool expect_below = getenv_bool("EXPECT_BELOW", true);
    bool expect_sticky = getenv_bool("EXPECT_STICKY", true);
    bool expect_skip_taskbar = getenv_bool("EXPECT_SKIP_TASKBAR", true);
    bool expect_skip_pager = getenv_bool("EXPECT_SKIP_PAGER", true);
    bool check_stacking = getenv_bool("CHECK_STACKING", true);

    int min_w = getenv_int("MIN_W", 50);
    int min_h = getenv_int("MIN_H", 20);

    xcb_window_t conky_win = XCB_NONE;
    xcb_window_t normal_win = XCB_NONE;
    uint32_t waited = 0;

    while (waited <= (uint32_t)window_timeout) {
        xcb_window_t candidates[1024];
        bool used_stacking = false;
        size_t count = collect_candidate_windows(conn, screen->root, atoms.net_client_list_stacking, candidates, 1024,
                                                 &used_stacking);
        for (size_t i = 0; i < count; i++) {
            xcb_window_t win = candidates[i];
            if (conky_win == XCB_NONE) {
                if (window_matches_class(conn, win, atoms.wm_class, conky_class_match) ||
                    window_matches_name(conn, win, atoms.net_wm_name, atoms.wm_name, conky_name_match)) {
                    conky_win = win;
                }
            }
            if (normal_win == XCB_NONE) {
                if (window_matches_class(conn, win, atoms.wm_class, normal_class_match) ||
                    window_matches_name(conn, win, atoms.net_wm_name, atoms.wm_name, normal_name_match)) {
                    normal_win = win;
                }
            }
        }

        if (conky_win != XCB_NONE && (!check_stacking || normal_win != XCB_NONE)) break;
        sleep_ms(50);
        waited += 50;
    }

    if (conky_win == XCB_NONE) die("Conky window not found");
    if (check_stacking && normal_win == XCB_NONE) die("Normal test window not found for stacking check");

    xcb_window_t* type_atoms = NULL;
    uint32_t type_len = 0;
    if (!fetch_atom_list(conn, conky_win, atoms.net_wm_window_type, &type_atoms, &type_len)) {
        die("_NET_WM_WINDOW_TYPE not readable");
    }

    bool type_ok = false;
    if (strcasecmp(expect_type, "ANY") == 0) {
        type_ok = true;
    } else if (strcasecmp(expect_type, "DESKTOP") == 0) {
        type_ok = atom_list_contains(type_atoms, type_len, atoms.net_wm_window_type_desktop);
    } else {
        type_ok = atom_list_contains(type_atoms, type_len, atoms.net_wm_window_type_dock);
    }
    free(type_atoms);
    if (!type_ok) die("_NET_WM_WINDOW_TYPE mismatch");

    xcb_window_t* state_atoms = NULL;
    uint32_t state_len = 0;
    if (!fetch_atom_list(conn, conky_win, atoms.net_wm_state, &state_atoms, &state_len)) {
        die("_NET_WM_STATE not readable");
    }

    if (expect_below && !atom_list_contains(state_atoms, state_len, atoms.net_wm_state_below)) {
        free(state_atoms);
        die("_NET_WM_STATE missing BELOW");
    }
    if (expect_sticky && !atom_list_contains(state_atoms, state_len, atoms.net_wm_state_sticky)) {
        free(state_atoms);
        die("_NET_WM_STATE missing STICKY");
    }
    if (expect_skip_taskbar && !atom_list_contains(state_atoms, state_len, atoms.net_wm_state_skip_taskbar)) {
        free(state_atoms);
        die("_NET_WM_STATE missing SKIP_TASKBAR");
    }
    if (expect_skip_pager && !atom_list_contains(state_atoms, state_len, atoms.net_wm_state_skip_pager)) {
        free(state_atoms);
        die("_NET_WM_STATE missing SKIP_PAGER");
    }
    free(state_atoms);

    if (!wm_state_is_normal(conn, conky_win, atoms.wm_state)) {
        die("WM_STATE missing or not normal");
    }
    if (!property_exists(conn, conky_win, atoms.wm_hints)) {
        die("WM_HINTS missing");
    }
    if (!property_exists(conn, conky_win, atoms.wm_normal_hints)) {
        die("WM_NORMAL_HINTS missing");
    }

    xcb_get_window_attributes_cookie_t ack = xcb_get_window_attributes(conn, conky_win);
    xcb_get_window_attributes_reply_t* arep = xcb_get_window_attributes_reply(conn, ack, NULL);
    if (!arep) die("window attributes failed");
    if (arep->map_state != XCB_MAP_STATE_VIEWABLE) {
        free(arep);
        die("Conky not viewable");
    }
    free(arep);

    int16_t rx = 0, ry = 0;
    uint16_t w = 0, h = 0;
    if (!get_window_geometry_on_root(conn, conky_win, screen->root, &rx, &ry, &w, &h)) {
        die("failed to get Conky geometry");
    }

    if ((int)w < min_w || (int)h < min_h) {
        die("Conky window too small");
    }

    int32_t sx = rx;
    int32_t sy = ry;
    int32_t sw = (int32_t)w;
    int32_t sh = (int32_t)h;
    int32_t screen_w = (int32_t)screen->width_in_pixels;
    int32_t screen_h = (int32_t)screen->height_in_pixels;

    bool intersects = !(sx + sw <= 0 || sy + sh <= 0 || sx >= screen_w || sy >= screen_h);
    if (!intersects) {
        die("Conky window off-screen");
    }

    if (check_stacking) {
        xcb_window_t* list = NULL;
        uint32_t len = 0;
        bool got_list = get_root_stacking_list(conn, screen->root, atoms.net_client_list_stacking, &list, &len);
        if (!got_list) {
            die("_NET_CLIENT_LIST_STACKING missing for stacking check");
        }
        int conky_idx = -1;
        int normal_idx = -1;
        for (uint32_t i = 0; i < len; i++) {
            if (list[i] == conky_win) conky_idx = (int)i;
            if (list[i] == normal_win) normal_idx = (int)i;
        }
        free(list);
        if (conky_idx < 0 || normal_idx < 0) {
            die("stacking list missing windows");
        }
        if (conky_idx >= normal_idx) {
            die("Conky not below normal window in stacking order");
        }
    }

    xcb_disconnect(conn);
    printf("conky_probe: ok\n");
    return 0;
}
