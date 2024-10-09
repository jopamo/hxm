/* src/main.c
 * Main entry point and initialization
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bbox.h"
#include "event.h"
#include "xcb_utils.h"

static server_t server;

static void handle_signal(int sig) {
    switch (sig) {
        case SIGHUP:
            LOG_INFO("Reload requested");
            g_reload_pending = 1;
            break;
        case SIGINT:
        case SIGTERM:
            LOG_INFO("Shutting down");
            g_shutdown_pending = 1;
            break;
        case SIGUSR1:
            counters_dump();
            break;
        case SIGUSR2:
            LOG_INFO("Restart requested");
            g_restart_pending = 1;
            break;
    }
}

static void print_help(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --exit          Exit the running bbox instance\n");
    printf("  --restart       Restart the running bbox instance\n");
    printf("  --reconfigure   Reload the configuration of the running bbox instance\n");
    printf("  --dump-stats    Print performance counters and exit\n");
    printf("  --help          Print this help and exit\n");
}

static int send_signal_to_wm(int sig) {
    xcb_connection_t* conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "Failed to connect to X server\n");
        return -1;
    }

    atoms_init(conn);

    xcb_get_selection_owner_reply_t* owner =
        xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, atoms.WM_S0), NULL);
    if (!owner || owner->owner == XCB_NONE) {
        fprintf(stderr, "No running bbox instance found (WM_S0 selection not owned)\n");
        free(owner);
        xcb_disconnect(conn);
        return -1;
    }

    xcb_window_t wm_win = owner->owner;
    free(owner);

    xcb_get_property_reply_t* prop = xcb_get_property_reply(
        conn, xcb_get_property(conn, 0, wm_win, atoms._NET_WM_PID, XCB_ATOM_CARDINAL, 0, 1), NULL);
    if (!prop || prop->type != XCB_ATOM_CARDINAL || xcb_get_property_value_length(prop) == 0) {
        fprintf(stderr, "Could not find PID of running bbox instance (_NET_WM_PID missing on supporting window)\n");
        free(prop);
        xcb_disconnect(conn);
        return -1;
    }

    uint32_t pid = *(uint32_t*)xcb_get_property_value(prop);
    free(prop);
    xcb_disconnect(conn);

    if (kill((pid_t)pid, sig) == 0) {
        return 0;
    } else {
        perror("kill");
        return -1;
    }
}

int main(int argc, char** argv) {
    counters_init();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-stats") == 0) {
            counters_dump();
            return 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--exit") == 0) {
            return send_signal_to_wm(SIGTERM) == 0 ? 0 : 1;
        } else if (strcmp(argv[i], "--restart") == 0) {
            return send_signal_to_wm(SIGUSR2) == 0 ? 0 : 1;
        } else if (strcmp(argv[i], "--reconfigure") == 0) {
            return send_signal_to_wm(SIGHUP) == 0 ? 0 : 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGUSR1, handle_signal);
    signal(SIGUSR2, handle_signal);

    LOG_INFO("bbox starting");

    server_init(&server);
    server_run(&server);  // never returns
    server_cleanup(&server);

    return 0;
}