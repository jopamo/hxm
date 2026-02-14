#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>

typedef struct {
  xcb_window_t* items;
  size_t len;
  size_t cap;
} window_set_t;

typedef struct {
  xcb_connection_t* conn;
  xcb_window_t root;
  xcb_atom_t atom_client_list;
  xcb_atom_t atom_client_list_stacking;
  xcb_atom_t atom_wm_state;
  xcb_atom_t atom_wm_window_type;
  xcb_atom_t atom_wm_desktop;
  window_set_t tracked;
} panel_ctx_t;

static void die(const char* msg) {
  fprintf(stderr, "panel_simulator: %s\n", msg);
  exit(1);
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

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000ull);
}

static void sleep_ms(uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
  nanosleep(&ts, NULL);
}

static xcb_atom_t get_atom(xcb_connection_t* conn, const char* name) {
  xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
  xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, NULL);
  if (!reply)
    die("failed to intern atom");
  xcb_atom_t atom = reply->atom;
  free(reply);
  return atom;
}

static char* get_atom_name(xcb_connection_t* conn, xcb_atom_t atom) {
  xcb_get_atom_name_cookie_t cookie = xcb_get_atom_name(conn, atom);
  xcb_get_atom_name_reply_t* reply = xcb_get_atom_name_reply(conn, cookie, NULL);
  if (!reply)
    return NULL;
  int len = xcb_get_atom_name_name_length(reply);
  char* name = malloc((size_t)len + 1);
  if (!name)
    die("alloc failed");
  memcpy(name, xcb_get_atom_name_name(reply), (size_t)len);
  name[len] = '\0';
  free(reply);
  return name;
}

static uint32_t* get_u32_property_list(xcb_connection_t* conn, xcb_window_t win, xcb_atom_t prop, xcb_atom_t type, size_t* out_len) {
  xcb_get_property_cookie_t ck = xcb_get_property(conn, 0, win, prop, type, 0, UINT32_MAX);
  xcb_get_property_reply_t* rep = xcb_get_property_reply(conn, ck, NULL);
  if (!rep) {
    if (out_len)
      *out_len = 0;
    return NULL;
  }
  int len_bytes = xcb_get_property_value_length(rep);
  if (len_bytes <= 0) {
    free(rep);
    if (out_len)
      *out_len = 0;
    return NULL;
  }
  size_t len = (size_t)len_bytes / 4;
  uint32_t* vals = malloc(len * sizeof(uint32_t));
  if (!vals)
    die("alloc failed");
  memcpy(vals, xcb_get_property_value(rep), len * sizeof(uint32_t));
  free(rep);
  if (out_len)
    *out_len = len;
  return vals;
}

static bool window_set_contains(const window_set_t* set, xcb_window_t win) {
  for (size_t i = 0; i < set->len; i++) {
    if (set->items[i] == win)
      return true;
  }
  return false;
}

static void window_set_add(window_set_t* set, xcb_window_t win) {
  if (window_set_contains(set, win))
    return;
  if (set->len == set->cap) {
    size_t next = set->cap ? set->cap * 2 : 32;
    xcb_window_t* items = realloc(set->items, next * sizeof(xcb_window_t));
    if (!items)
      die("alloc failed");
    set->items = items;
    set->cap = next;
  }
  set->items[set->len++] = win;
}

static void print_u32_list(const uint32_t* vals, size_t len) {
  printf("[");
  for (size_t i = 0; i < len; i++) {
    if (i > 0)
      printf(" ");
    printf("%" PRIu32, vals[i]);
  }
  printf("]");
}

static void print_atom_list(xcb_connection_t* conn, const uint32_t* vals, size_t len) {
  printf("[");
  for (size_t i = 0; i < len; i++) {
    if (i > 0)
      printf(" ");
    char* name = get_atom_name(conn, (xcb_atom_t)vals[i]);
    if (name) {
      printf("%s", name);
      free(name);
    }
    else {
      printf("%" PRIu32, vals[i]);
    }
  }
  printf("]");
}

static void watch_window(panel_ctx_t* ctx, xcb_window_t win) {
  if (window_set_contains(&ctx->tracked, win))
    return;
  uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_void_cookie_t ck = xcb_change_window_attributes_checked(ctx->conn, win, XCB_CW_EVENT_MASK, &mask);
  xcb_generic_error_t* err = xcb_request_check(ctx->conn, ck);
  if (err) {
    free(err);
    return;
  }
  window_set_add(&ctx->tracked, win);
}

static void update_root_lists(panel_ctx_t* ctx) {
  size_t len = 0;
  uint32_t* list = get_u32_property_list(ctx->conn, ctx->root, ctx->atom_client_list, XCB_ATOM_WINDOW, &len);
  printf("root _NET_CLIENT_LIST ");
  print_u32_list(list, len);
  printf("\n");
  for (size_t i = 0; i < len; i++) {
    watch_window(ctx, (xcb_window_t)list[i]);
  }
  free(list);

  len = 0;
  list = get_u32_property_list(ctx->conn, ctx->root, ctx->atom_client_list_stacking, XCB_ATOM_WINDOW, &len);
  printf("root _NET_CLIENT_LIST_STACKING ");
  print_u32_list(list, len);
  printf("\n");
  for (size_t i = 0; i < len; i++) {
    watch_window(ctx, (xcb_window_t)list[i]);
  }
  free(list);

  fflush(stdout);
}

static void handle_window_prop(panel_ctx_t* ctx, xcb_window_t win, xcb_atom_t atom, uint8_t state) {
  if (state == XCB_PROPERTY_DELETE) {
    const char* name = "unknown";
    if (atom == ctx->atom_wm_state)
      name = "_NET_WM_STATE";
    else if (atom == ctx->atom_wm_window_type)
      name = "_NET_WM_WINDOW_TYPE";
    else if (atom == ctx->atom_wm_desktop)
      name = "_NET_WM_DESKTOP";
    printf("win %" PRIu32 " %s <deleted>\n", win, name);
    fflush(stdout);
    return;
  }

  size_t len = 0;
  if (atom == ctx->atom_wm_state) {
    uint32_t* vals = get_u32_property_list(ctx->conn, win, atom, XCB_ATOM_ATOM, &len);
    printf("win %" PRIu32 " _NET_WM_STATE ", win);
    print_atom_list(ctx->conn, vals, len);
    printf("\n");
    free(vals);
  }
  else if (atom == ctx->atom_wm_window_type) {
    uint32_t* vals = get_u32_property_list(ctx->conn, win, atom, XCB_ATOM_ATOM, &len);
    printf("win %" PRIu32 " _NET_WM_WINDOW_TYPE ", win);
    print_atom_list(ctx->conn, vals, len);
    printf("\n");
    free(vals);
  }
  else if (atom == ctx->atom_wm_desktop) {
    uint32_t* vals = get_u32_property_list(ctx->conn, win, atom, XCB_ATOM_CARDINAL, &len);
    printf("win %" PRIu32 " _NET_WM_DESKTOP ", win);
    print_u32_list(vals, len);
    printf("\n");
    free(vals);
  }
  fflush(stdout);
}

static void usage(void) {
  fprintf(stderr,
          "Usage:\n"
          "  panel_simulator [--duration <seconds>]\n");
  exit(2);
}

int main(int argc, char** argv) {
  uint32_t duration_sec = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--duration") == 0) {
      if (i + 1 >= argc)
        usage();
      duration_sec = parse_u32(argv[++i]);
    }
    else {
      usage();
    }
  }

  panel_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  ctx.conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(ctx.conn))
    die("cannot connect to X server");

  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(ctx.conn)).data;
  if (!screen)
    die("no screen");
  ctx.root = screen->root;

  ctx.atom_client_list = get_atom(ctx.conn, "_NET_CLIENT_LIST");
  ctx.atom_client_list_stacking = get_atom(ctx.conn, "_NET_CLIENT_LIST_STACKING");
  ctx.atom_wm_state = get_atom(ctx.conn, "_NET_WM_STATE");
  ctx.atom_wm_window_type = get_atom(ctx.conn, "_NET_WM_WINDOW_TYPE");
  ctx.atom_wm_desktop = get_atom(ctx.conn, "_NET_WM_DESKTOP");

  uint32_t root_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_change_window_attributes(ctx.conn, ctx.root, XCB_CW_EVENT_MASK, &root_mask);
  xcb_flush(ctx.conn);

  update_root_lists(&ctx);

  uint64_t end_us = 0;
  if (duration_sec > 0)
    end_us = now_us() + (uint64_t)duration_sec * 1000000ull;

  while (duration_sec == 0 || now_us() < end_us) {
    xcb_generic_event_t* ev = xcb_poll_for_event(ctx.conn);
    if (!ev) {
      sleep_ms(5);
      continue;
    }

    uint8_t type = ev->response_type & ~0x80;
    if (type == XCB_PROPERTY_NOTIFY) {
      xcb_property_notify_event_t* p = (xcb_property_notify_event_t*)ev;
      if (p->window == ctx.root && (p->atom == ctx.atom_client_list || p->atom == ctx.atom_client_list_stacking)) {
        update_root_lists(&ctx);
      }
      else if (window_set_contains(&ctx.tracked, p->window) && (p->atom == ctx.atom_wm_state || p->atom == ctx.atom_wm_window_type || p->atom == ctx.atom_wm_desktop)) {
        handle_window_prop(&ctx, p->window, p->atom, p->state);
      }
    }
    free(ev);
  }

  free(ctx.tracked.items);
  xcb_disconnect(ctx.conn);
  return 0;
}
