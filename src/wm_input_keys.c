/* src/wm_input_keys.c
 * Keyboard input handling and focus cycling.
 *
 * This module manages:
 * - Global key bindings (Alt-Tab, Workspace switching, etc.).
 * - Focus cycling logic (MRU traversal).
 * - Executing external commands (spawn).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TEST_WM_INPUT_KEYS
#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "client.h"
#include "config.h"
#include "event.h"
#include "hxm.h"
#include "menu.h"
#include "wm.h"
#include "wm_internal.h"
#endif

// Common modifiers that should be ignored when matching bindings
// (CapsLock, NumLock/Mod2, ScrollLock/Mod5)
#ifndef TEST_WM_INPUT_KEYS
static const uint16_t IGNORED_MODS[] = {0,
                                        XCB_MOD_MASK_LOCK,
                                        XCB_MOD_MASK_2,
                                        XCB_MOD_MASK_5,
                                        XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2,
                                        XCB_MOD_MASK_LOCK | XCB_MOD_MASK_5,
                                        XCB_MOD_MASK_2 | XCB_MOD_MASK_5,
                                        XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2 | XCB_MOD_MASK_5};
#endif

uint32_t wm_clean_mods(uint16_t state) {
    // Mask out NumLock, ScrollLock, and CapsLock for comparison
    return state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2 | XCB_MOD_MASK_5);
}

#ifndef TEST_WM_INPUT_KEYS
static void spawn(const char* cmd) {
    if (!cmd) return;

    // Double fork to avoid zombies if SIGCHLD isn't trapped globally
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            char* args[] = {"/bin/sh", "-c", (char*)cmd, NULL};
            execvp(args[0], args);
            // Use _exit to avoid flushing parent stdio buffers
            perror("spawn execvp failed");
            _exit(127);
        }
        _exit(0);
    }
    // Wait for the intermediate child
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}
#endif

// Helper predicate for focus cycling
static bool is_focusable(client_hot_t* c, server_t* s) {
    if (c->state != STATE_MAPPED) return false;

    // Respect "show desktop" temporary hides
    if (s->showing_desktop && c->show_desktop_hidden) return false;

    // Must be on current desktop or sticky
    if (c->desktop != (int32_t)s->current_desktop && !c->sticky) return false;

    // Reject un-focusable types
    switch (c->type) {
        case WINDOW_TYPE_DOCK:
        case WINDOW_TYPE_NOTIFICATION:
        case WINDOW_TYPE_DESKTOP:
        case WINDOW_TYPE_MENU:
        case WINDOW_TYPE_DROPDOWN_MENU:
        case WINDOW_TYPE_POPUP_MENU:
        case WINDOW_TYPE_TOOLTIP:
        case WINDOW_TYPE_COMBO:
        case WINDOW_TYPE_DND:
            return false;
        default:
            return true;
    }
}

void wm_cycle_focus(server_t* s, bool forward) {
    if (list_empty(&s->focus_history)) return;

    list_node_t* start_node = &s->focus_history;

    // If we have a focused client, start searching from there
    if (s->focused_client != HANDLE_INVALID) {
        client_hot_t* focused = server_chot(s, s->focused_client);
        if (focused) start_node = &focused->focus_node;
    }

    list_node_t* node = forward ? start_node->next : start_node->prev;

    // Guard against infinite loop if no windows are focusable
    int iterations = 0;
    int max_iterations = (int)s->active_clients.length + 4;  // Sanity limit

    while (node != start_node && iterations++ < max_iterations) {
        // Skip the list head (sentinel)
        if (node == &s->focus_history) {
            node = forward ? node->next : node->prev;
            if (node == start_node) break;
        }

        client_hot_t* c = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));

        if (is_focusable(c, s)) {
            wm_set_focus(s, c->self);
            stack_raise(s, c->self);
            return;
        }

        node = forward ? node->next : node->prev;
    }
}

#ifndef TEST_WM_INPUT_KEYS
/*
 * wm_setup_keys:
 * Grab all configured global keys on the root window.
 *
 * Robustness:
 * X11 grabs are exact. If NumLock or CapsLock is on, the modifier mask changes.
 * To ensure bindings work regardless of Lock state, we grab every binding with
 * all 8 combinations of (CapsLock | NumLock | ScrollLock).
 */
void wm_setup_keys(server_t* s) {
    if (s->keysyms) xcb_key_symbols_free(s->keysyms);
    s->keysyms = xcb_key_symbols_alloc(s->conn);
    if (!s->keysyms) return;

    xcb_ungrab_key(s->conn, XCB_GRAB_ANY, s->root, XCB_MOD_MASK_ANY);

    for (size_t i = 0; i < s->config.key_bindings.length; i++) {
        key_binding_t* b = s->config.key_bindings.items[i];
        if (!b) continue;

        xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(s->keysyms, b->keysym);
        if (!keycodes) continue;

        for (xcb_keycode_t* k = keycodes; *k; k++) {
            // Grab for all ignored modifier combinations
            // This ensures the bind works even if CapsLock or NumLock is on
            for (size_t m = 0; m < sizeof(IGNORED_MODS) / sizeof(IGNORED_MODS[0]); m++) {
                xcb_grab_key(s->conn, 1, s->root, b->modifiers | IGNORED_MODS[m], *k, XCB_GRAB_MODE_ASYNC,
                             XCB_GRAB_MODE_ASYNC);
            }
        }
        free(keycodes);
    }

    // xcb_flush(s->conn);
}
#endif

// Safer string-to-integer helper
static int safe_atoi(const char* str) {
    if (!str) return 0;
    char* end;
    long val = strtol(str, &end, 10);
    if (end == str) return 0;
    if (val < 0) return 0;
    return (int)val;
}

void wm_handle_key_press(server_t* s, xcb_key_press_event_t* ev) {
    if (!s->keysyms) return;

    xcb_keysym_t sym = xcb_key_symbols_get_keysym(s->keysyms, ev->detail, 0);

    // Menu logic takes precedence
    if (s->menu.visible) {
        menu_handle_key_press(s, ev);
        return;
    }

    uint32_t clean_state = wm_clean_mods(ev->state);

    LOG_DEBUG("Key press: detail=%u state=%u clean=%u sym=%x", ev->detail, ev->state, clean_state, sym);

    // Linear scan of bindings
    // Note: For large configs, a hash map lookup would be O(1)
    for (size_t i = 0; i < s->config.key_bindings.length; i++) {
        key_binding_t* b = s->config.key_bindings.items[i];
        if (!b) continue;

        if (b->keysym != sym || (uint32_t)b->modifiers != clean_state) continue;

        LOG_INFO("Matched key binding action %d", b->action);

        switch (b->action) {
            case ACTION_CLOSE:
                if (s->focused_client != HANDLE_INVALID) client_close(s, s->focused_client);
                break;

            case ACTION_FOCUS_NEXT:
                wm_cycle_focus(s, true);
                break;

            case ACTION_FOCUS_PREV:
                wm_cycle_focus(s, false);
                break;

            case ACTION_TERMINAL:
                spawn("st || xterm || x-terminal-emulator");
                break;

            case ACTION_EXEC:
                spawn(b->exec_cmd);
                break;

            case ACTION_RESTART:
                LOG_INFO("Triggering restart...");
                g_restart_pending = 1;
                break;

            case ACTION_EXIT:
                exit(0);
                break;

            case ACTION_WORKSPACE:
                if (b->exec_cmd) wm_switch_workspace(s, (uint32_t)safe_atoi(b->exec_cmd));
                break;

            case ACTION_WORKSPACE_PREV:
                wm_switch_workspace_relative(s, -1);
                break;

            case ACTION_WORKSPACE_NEXT:
                wm_switch_workspace_relative(s, 1);
                break;

            case ACTION_MOVE_TO_WORKSPACE:
                if (b->exec_cmd && s->focused_client != HANDLE_INVALID) {
                    wm_client_move_to_workspace(s, s->focused_client, (uint32_t)safe_atoi(b->exec_cmd), false);
                }
                break;

            case ACTION_MOVE_TO_WORKSPACE_FOLLOW:
                if (b->exec_cmd && s->focused_client != HANDLE_INVALID) {
                    wm_client_move_to_workspace(s, s->focused_client, (uint32_t)safe_atoi(b->exec_cmd), true);
                }
                break;

            case ACTION_TOGGLE_STICKY:
                if (s->focused_client != HANDLE_INVALID) wm_client_toggle_sticky(s, s->focused_client);
                break;

            case ACTION_MOVE:
            case ACTION_RESIZE:
                if (s->focused_client != HANDLE_INVALID) {
                    client_hot_t* hot = server_chot(s, s->focused_client);
                    if (!hot) break;

                    if (b->action == ACTION_MOVE && !client_can_move(hot)) break;
                    if (b->action == ACTION_RESIZE && !client_can_resize(hot)) break;

                    int16_t root_x, root_y;

                    if (b->action == ACTION_MOVE) {
                        xcb_query_pointer_cookie_t ck = xcb_query_pointer(s->conn, s->root);
                        cookie_jar_push(&s->cookie_jar, ck.sequence, COOKIE_QUERY_POINTER, s->focused_client, 0x100,
                                        s->txn_id, wm_handle_reply);
                    } else {
                        // RESIZE: Warp to bottom right
                        root_x = hot->server.x + hot->server.w;
                        root_y = hot->server.y + hot->server.h;

                        xcb_warp_pointer(s->conn, XCB_NONE, s->root, 0, 0, 0, 0, root_x, root_y);

                        wm_start_interaction(s, s->focused_client, hot, false, RESIZE_BOTTOM | RESIZE_RIGHT, root_x,
                                             root_y, XCB_CURRENT_TIME, true);
                    }
                }
                break;

            default:
                break;
        }
        // Break after finding the first matching binding (prevent duplicate triggers)
        return;
    }
}
