/* src/wm_input_keys.c
 * Keyboard input handling and focus cycling
 */

#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "bbox.h"
#include "client.h"
#include "config.h"
#include "event.h"
#include "menu.h"
#include "wm.h"
#include "wm_internal.h"

uint32_t wm_clean_mods(uint16_t state) {
    // Mask out NumLock/ScrollLock if they interfere (common in WMs)
    return state & ~(XCB_MOD_MASK_2 | XCB_MOD_MASK_5 | XCB_MOD_MASK_LOCK);
}

static void spawn(const char* cmd) {
    if (fork() == 0) {
        if (fork() == 0) {
            setsid();
            char* args[] = {"/bin/sh", "-c", (char*)cmd, NULL};
            execvp(args[0], args);
            exit(1);
        }
        exit(0);
    }
    wait(NULL);
}

void wm_cycle_focus(server_t* s, bool forward) {
    if (list_empty(&s->focus_history)) return;

    list_node_t* start_node = &s->focus_history;
    if (s->focused_client != HANDLE_INVALID) {
        client_hot_t* focused = server_chot(s, s->focused_client);
        if (focused) start_node = &focused->focus_node;
    }

    list_node_t* node = forward ? start_node->next : start_node->prev;
    while (node != start_node) {
        if (node == &s->focus_history) {
            node = forward ? node->next : node->prev;
            if (node == start_node) break;
        }

        client_hot_t* c = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));

        if (c->state == STATE_MAPPED && (c->desktop == (int32_t)s->current_desktop || c->sticky) &&
            c->type != WINDOW_TYPE_DOCK && c->type != WINDOW_TYPE_NOTIFICATION && c->type != WINDOW_TYPE_DESKTOP &&
            c->type != WINDOW_TYPE_MENU && c->type != WINDOW_TYPE_DROPDOWN_MENU && c->type != WINDOW_TYPE_POPUP_MENU &&
            c->type != WINDOW_TYPE_TOOLTIP && c->type != WINDOW_TYPE_COMBO && c->type != WINDOW_TYPE_DND) {
            wm_set_focus(s, c->self);
            stack_raise(s, c->self);
            return;
        }

        node = forward ? node->next : node->prev;
    }
}

void wm_setup_keys(server_t* s) {
    if (s->keysyms) xcb_key_symbols_free(s->keysyms);
    s->keysyms = xcb_key_symbols_alloc(s->conn);

    xcb_ungrab_key(s->conn, XCB_GRAB_ANY, s->root, XCB_MOD_MASK_ANY);

    // Grab keys from config
    for (size_t i = 0; i < s->config.key_bindings.length; i++) {
        key_binding_t* b = s->config.key_bindings.items[i];
        xcb_keycode_t* keycodes = xcb_key_symbols_get_keycode(s->keysyms, b->keysym);
        if (!keycodes) continue;

        for (xcb_keycode_t* k = keycodes; *k; k++) {
            xcb_grab_key(s->conn, 1, s->root, b->modifiers, *k, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        }

        free(keycodes);
    }

    xcb_flush(s->conn);
}

void wm_handle_key_press(server_t* s, xcb_key_press_event_t* ev) {
    if (!s->keysyms) return;

    xcb_keysym_t sym = xcb_key_symbols_get_keysym(s->keysyms, ev->detail, 0);

    if (s->menu.visible && sym == XK_Escape) {
        menu_hide(s);
        return;
    }

    uint32_t mods = wm_clean_mods(ev->state);

    LOG_DEBUG("Key press: detail=%u state=%u clean=%u sym=%x", ev->detail, ev->state, mods, sym);

    for (size_t i = 0; i < s->config.key_bindings.length; i++) {
        key_binding_t* b = s->config.key_bindings.items[i];
        if (!b) continue;

        if (b->keysym != sym || b->modifiers != mods) continue;

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
                if (b->exec_cmd) spawn(b->exec_cmd);
                break;
            case ACTION_RESTART:
                LOG_INFO("Triggering restart...");
                g_restart_pending = 1;
                break;
            case ACTION_EXIT:
                exit(0);
                break;
            case ACTION_WORKSPACE:
                if (b->exec_cmd) wm_switch_workspace(s, (uint32_t)atoi(b->exec_cmd));
                break;
            case ACTION_WORKSPACE_PREV:
                wm_switch_workspace_relative(s, -1);
                break;
            case ACTION_WORKSPACE_NEXT:
                wm_switch_workspace_relative(s, 1);
                break;
            case ACTION_MOVE_TO_WORKSPACE:
                if (b->exec_cmd && s->focused_client != HANDLE_INVALID) {
                    wm_client_move_to_workspace(s, s->focused_client, (uint32_t)atoi(b->exec_cmd), false);
                }
                break;
            case ACTION_MOVE_TO_WORKSPACE_FOLLOW:
                if (b->exec_cmd && s->focused_client != HANDLE_INVALID) {
                    wm_client_move_to_workspace(s, s->focused_client, (uint32_t)atoi(b->exec_cmd), true);
                }
                break;
            case ACTION_TOGGLE_STICKY:
                if (s->focused_client != HANDLE_INVALID) wm_client_toggle_sticky(s, s->focused_client);
                break;
            default:
                break;
        }
        return;
    }
}
