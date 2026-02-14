/* src/main.c
 * Application entry point.
 *
 * Responsibilities:
 * - Parsing command-line arguments.
 * - Signal handling for IPC (restart/exit/reload commands to existing
 * instance).
 * - Initializing the server singleton and entering the run loop.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "event.h"
#include "hxm.h"
#include "xcb_utils.h"

static server_t server;

static void print_help(const char* prog) {
  printf("Usage: %s [options]\n", prog);
  printf("Options:\n");
  printf("  --exit          Exit the running hxm instance\n");
  printf("  --restart       Restart the running hxm instance\n");
  printf(
      "  --reconfigure   Reload the configuration of the running hxm "
      "instance\n");
#if HXM_DIAG
  printf("  --dump-stats    Print performance counters and exit\n");
#endif
  printf("  --help          Print this help and exit\n");
}

static bool get_cardinal32(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t prop, uint32_t* out) {
  xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, win, prop, XCB_ATOM_CARDINAL, 0, 1);
  xcb_get_property_reply_t* r = xcb_get_property_reply(conn, ck, NULL);
  if (!r)
    return false;
  bool ok = (r->type == XCB_ATOM_CARDINAL && r->format == 32 && xcb_get_property_value_length(r) >= 4);
  if (ok)
    *out = *(uint32_t*)xcb_get_property_value(r);
  free(r);
  return ok;
}

static bool get_window32(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t prop, xcb_window_t* out) {
  xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, win, prop, XCB_ATOM_WINDOW, 0, 1);
  xcb_get_property_reply_t* r = xcb_get_property_reply(conn, ck, NULL);
  if (!r)
    return false;
  bool ok = (r->type == XCB_ATOM_WINDOW && r->format == 32 && xcb_get_property_value_length(r) >= 4);
  if (ok)
    *out = *(xcb_window_t*)xcb_get_property_value(r);
  free(r);
  return ok;
}

static int send_signal_to_wm(int sig) {
  xcb_connection_t* conn = xcb_connect(NULL, NULL);
  if (!conn || xcb_connection_has_error(conn)) {
    fprintf(stderr, "Failed to connect to X server\n");
    if (conn)
      xcb_disconnect(conn);
    return -1;
  }

  atoms_init(conn);

  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
  xcb_window_t root = screen ? screen->root : XCB_NONE;

  // Prefer EWMH supporting WM check window published on root
  xcb_window_t wm_win = XCB_NONE;
  if (root != XCB_NONE) {
    (void)get_window32(conn, root, atoms._NET_SUPPORTING_WM_CHECK, &wm_win);
  }

  // Fallback to WM_S0 selection owner
  if (wm_win == XCB_NONE) {
    xcb_get_selection_owner_reply_t* owner = xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, atoms.WM_S0), NULL);
    if (owner) {
      wm_win = owner->owner;
      free(owner);
    }
  }

  if (wm_win == XCB_NONE) {
    fprintf(stderr, "No running hxm instance found\n");
    xcb_disconnect(conn);
    return -1;
  }

  uint32_t pid = 0;
  if (!get_cardinal32(conn, wm_win, atoms._NET_WM_PID, &pid) || pid == 0) {
    fprintf(stderr, "Could not find PID of running hxm instance\n");
    xcb_disconnect(conn);
    return -1;
  }

  xcb_disconnect(conn);

  if (kill((pid_t)pid, sig) == 0) {
    return 0;
  }
  else {
    perror("kill");
    return -1;
  }
}

int main(int argc, char** argv) {
#if HXM_DIAG
  counters_init();
#endif

  for (int i = 1; i < argc; i++) {
#if HXM_DIAG
    if (strcmp(argv[i], "--dump-stats") == 0) {
      // Ask the running instance to dump stats
      return send_signal_to_wm(SIGUSR1) == 0 ? 0 : 1;
    }
    else
#endif
        if (strcmp(argv[i], "--help") == 0) {
      print_help(argv[0]);
      return 0;
    }
    else if (strcmp(argv[i], "--exit") == 0) {
      return send_signal_to_wm(SIGTERM) == 0 ? 0 : 1;
    }
    else if (strcmp(argv[i], "--restart") == 0) {
      return send_signal_to_wm(SIGUSR2) == 0 ? 0 : 1;
    }
    else if (strcmp(argv[i], "--reconfigure") == 0) {
      return send_signal_to_wm(SIGHUP) == 0 ? 0 : 1;
    }
    else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      print_help(argv[0]);
      return 1;
    }
  }

  LOG_INFO("hxm starting");

  server_init(&server);
  server_run(&server);  // runs until shutdown
  server_cleanup(&server);

  return 0;
}
