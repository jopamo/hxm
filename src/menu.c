/* src/menu.c
 * Menu rendering and interaction
 *
 * The menu system runs as a modal overlay.
 * When `menu_show` is called:
 * 1. The menu window is mapped (OverrideRedirect).
 * 2. We grab the pointer and keyboard to steal all input.
 * 3. The main event loop delegates events to `menu_handle_*` functions.
 * 4. On dismissal (Escape, click outside, selection), grabs are released.
 */

#include "menu.h"

#include <X11/keysym.h>
#include <assert.h>
#include <errno.h>
#include <librsvg/rsvg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb_icccm.h>
#include <yaml.h>

#include "client.h"
#include "event.h"
#include "hxm.h"
#include "wm.h"
#include "xcb_utils.h"

#define MENU_PADDING 4
#define MENU_ITEM_HEIGHT 24
#define MENU_WIDTH 240

static void* xmalloc(size_t n) {
  void* p = malloc(n);
  if (!p) {
    LOG_ERROR("oom");
    abort();
  }
  return p;
}

static void* xcalloc(size_t n, size_t sz) {
  void* p = calloc(n, sz);
  if (!p) {
    LOG_ERROR("oom");
    abort();
  }
  return p;
}

static char* xstrdup(const char* s) {
  if (!s)
    return NULL;
  char* p = strdup(s);
  if (!p) {
    LOG_ERROR("oom");
    abort();
  }
  return p;
}

static int menu_find_next_selectable(server_t* s, int start, int dir);
static int menu_find_first_selectable(server_t* s);

static bool switcher_is_candidate(const server_t* s, const client_hot_t* hot) {
  if (!s || !hot)
    return false;
  if (s->showing_desktop && hot->show_desktop_hidden)
    return false;

  switch (hot->type) {
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

static void menu_format_client_label(const server_t* s, const client_hot_t* hot, const client_cold_t* cold, char* out, size_t out_len) {
  const char* title = (cold && cold->title) ? cold->title : "Unnamed";
  char tag[64];
  size_t pos = 0;

  tag[0] = '\0';

  if (hot && !hot->sticky && hot->desktop >= 0 && (uint32_t)hot->desktop != s->current_desktop) {
    pos += (size_t)snprintf(tag + pos, sizeof(tag) - pos, "ws %d", hot->desktop);
  }
  if (hot && hot->state == STATE_UNMAPPED) {
    if (pos > 0 && pos + 1 < sizeof(tag)) {
      tag[pos++] = ' ';
      tag[pos] = '\0';
    }
    snprintf(tag + pos, sizeof(tag) - pos, "min");
  }

  if (tag[0] != '\0') {
    snprintf(out, out_len, "[%s] %s", tag, title);
  }
  else {
    snprintf(out, out_len, "%s", title);
  }
}

static void menu_clear_items(server_t* s) {
  for (size_t i = 0; i < s->menu.items.length; i++) {
    menu_item_t* item = s->menu.items.items[i];
    if (item->label)
      free(item->label);
    if (item->cmd)
      free(item->cmd);
    if (item->icon_path)
      free(item->icon_path);
    if (item->icon_surface)
      cairo_surface_destroy(item->icon_surface);
    free(item);
  }
  small_vec_clear(&s->menu.items);
}

static void menu_clear_config(menu_t* m) {
  if (!m)
    return;
  for (size_t i = 0; i < m->config_items.length; i++) {
    menu_item_spec_t* spec = m->config_items.items[i];
    if (!spec)
      continue;
    free(spec->label);
    free(spec->cmd);
    free(spec->icon_path);
    free(spec);
  }
  small_vec_clear(&m->config_items);
}

static cairo_surface_t* menu_load_icon(const char* path) {
  if (!path || path[0] == '\0')
    return NULL;

  const char* dot = strrchr(path, '.');
  bool is_svg = (dot && strcasecmp(dot, ".svg") == 0);

  if (is_svg) {
    GError* error = NULL;
    RsvgHandle* handle = rsvg_handle_new_from_file(path, &error);
    if (!handle) {
      if (error) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
          LOG_WARN("Failed to load SVG icon %s: %s", path, error->message);
        }
        g_error_free(error);
      }
      return NULL;
    }

    gdouble width = 0, height = 0;
    if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &width, &height)) {
      width = 16;
      height = 16;
    }
    int w = (int)width;
    int h = (int)height;
    if (w <= 0)
      w = 16;
    if (h <= 0)
      h = 16;

    RsvgRectangle viewport = {0.0, 0.0, (double)w, (double)h};
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      g_object_unref(handle);
      cairo_surface_destroy(surface);
      return NULL;
    }

    cairo_t* cr = cairo_create(surface);
    gboolean ok = rsvg_handle_render_document(handle, cr, &viewport, &error);
    if (!ok || error) {
      LOG_WARN("Failed to render SVG icon %s: %s", path, error ? error->message : "unknown error");
      if (error)
        g_error_free(error);
    }
    cairo_destroy(cr);
    g_object_unref(handle);
    return surface;
  }
  else {
    cairo_surface_t* surface = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surface);
      return NULL;
    }
    return surface;
  }
}

static bool parse_bool_scalar(const char* val) {
  if (!val)
    return false;
  return strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0 || strcasecmp(val, "1") == 0 || strcasecmp(val, "on") == 0;
}

static menu_action_t parse_action_scalar(const char* val) {
  if (!val)
    return MENU_ACTION_NONE;
  if (strcasecmp(val, "exec") == 0)
    return MENU_ACTION_EXEC;
  if (strcasecmp(val, "restart") == 0)
    return MENU_ACTION_RESTART;
  if (strcasecmp(val, "exit") == 0)
    return MENU_ACTION_EXIT;
  if (strcasecmp(val, "reload") == 0)
    return MENU_ACTION_RELOAD;
  if (strcasecmp(val, "restore") == 0)
    return MENU_ACTION_RESTORE;
  if (strcasecmp(val, "separator") == 0)
    return MENU_ACTION_SEPARATOR;
  if (strcasecmp(val, "none") == 0)
    return MENU_ACTION_NONE;
  LOG_WARN("Unknown menu action: %s", val);
  return MENU_ACTION_NONE;
}

bool menu_load_config(server_t* s, const char* path) {
  if (!s || !path)
    return false;

  FILE* f = fopen(path, "r");
  if (!f)
    return false;

  yaml_parser_t parser;
  yaml_document_t doc;

  if (!yaml_parser_initialize(&parser)) {
    fclose(f);
    return false;
  }

  yaml_parser_set_input_file(&parser, f);

  if (!yaml_parser_load(&parser, &doc)) {
    LOG_WARN("Failed to parse YAML in %s", path);
    yaml_parser_delete(&parser);
    fclose(f);
    return false;
  }

  yaml_node_t* root = yaml_document_get_root_node(&doc);
  if (!root || root->type != YAML_SEQUENCE_NODE) {
    LOG_WARN("menu.conf must be a YAML sequence of menu items: %s", path);
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(f);
    return false;
  }

  menu_clear_config(&s->menu);

  size_t loaded = 0;
  for (yaml_node_item_t* item = root->data.sequence.items.start; item < root->data.sequence.items.top; item++) {
    yaml_node_t* node = yaml_document_get_node(&doc, *item);
    if (!node || node->type != YAML_MAPPING_NODE) {
      LOG_WARN("menu.conf item is not a mapping; skipping");
      continue;
    }

    menu_item_spec_t* spec = xcalloc(1, sizeof(menu_item_spec_t));
    bool separator_flag = false;

    for (yaml_node_pair_t* pair = node->data.mapping.pairs.start; pair < node->data.mapping.pairs.top; pair++) {
      yaml_node_t* key_node = yaml_document_get_node(&doc, pair->key);
      yaml_node_t* val_node = yaml_document_get_node(&doc, pair->value);
      if (!key_node || key_node->type != YAML_SCALAR_NODE)
        continue;

      const char* key = (const char*)key_node->data.scalar.value;
      if (!key)
        continue;

      if (strcasecmp(key, "separator") == 0) {
        if (val_node && val_node->type == YAML_SCALAR_NODE) {
          const char* val = (const char*)val_node->data.scalar.value;
          separator_flag = parse_bool_scalar(val);
        }
        continue;
      }

      if (!val_node || val_node->type != YAML_SCALAR_NODE)
        continue;
      const char* val = (const char*)val_node->data.scalar.value;
      if (!val)
        continue;

      if (strcasecmp(key, "label") == 0)
        spec->label = xstrdup(val);
      else if (strcasecmp(key, "action") == 0)
        spec->action = parse_action_scalar(val);
      else if (strcasecmp(key, "cmd") == 0)
        spec->cmd = xstrdup(val);
      else if (strcasecmp(key, "icon") == 0)
        spec->icon_path = xstrdup(val);
    }

    if (separator_flag) {
      spec->action = MENU_ACTION_SEPARATOR;
    }
    else if (spec->action == MENU_ACTION_NONE && spec->cmd) {
      spec->action = MENU_ACTION_EXEC;
    }

    if (spec->action != MENU_ACTION_SEPARATOR && !spec->label) {
      LOG_WARN("Menu item missing label; skipping");
      free(spec->label);
      free(spec->cmd);
      free(spec->icon_path);
      free(spec);
      continue;
    }

    small_vec_push(&s->menu.config_items, spec);
    loaded++;
  }

  yaml_document_delete(&doc);
  yaml_parser_delete(&parser);
  fclose(f);

  if (loaded == 0) {
    LOG_WARN("Loaded menu.conf from %s but found no items", path);
    return false;
  }

  LOG_INFO("Loaded menu config from %s (%zu items)", path, loaded);
  return true;
}

static void menu_add_item(server_t* s, const char* label, menu_action_t action, const char* cmd, handle_t client, const char* icon_path) {
  menu_item_t* item = xmalloc(sizeof(menu_item_t));
  item->label = label ? xstrdup(label) : NULL;
  item->action = action;
  item->cmd = cmd ? xstrdup(cmd) : NULL;
  item->client = client;
  item->icon_path = icon_path ? xstrdup(icon_path) : NULL;
  item->icon_surface = menu_load_icon(icon_path);
  small_vec_push(&s->menu.items, item);

  // Resize menu height
  s->menu.h = s->menu.items.length * MENU_ITEM_HEIGHT + 2 * MENU_PADDING;
}

void menu_init(server_t* s) {
  s->menu.visible = false;
  s->menu.is_client_list = false;
  s->menu.is_switcher = false;
  s->menu.selected_index = -1;
  s->menu.item_height = MENU_ITEM_HEIGHT;
  s->menu.w = MENU_WIDTH;
  s->menu.h = 2 * MENU_PADDING;
  small_vec_init(&s->menu.items);
  small_vec_init(&s->menu.config_items);
  render_init(&s->menu.render_ctx);

  // Create window (Override Redirect)
  s->menu.window = xcb_generate_id(s->conn);
  uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
  uint32_t values[] = {s->config.theme.menu_items.color, 1,
                       XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE};

  xcb_create_window(s->conn, XCB_COPY_FROM_PARENT, s->menu.window, s->root, 0, 0, s->menu.w, s->menu.h, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, mask, values);
}

void menu_destroy(server_t* s) {
  if (s->conn && s->menu.window != XCB_NONE) {
    xcb_destroy_window(s->conn, s->menu.window);
    s->menu.window = XCB_NONE;
  }
  render_free(&s->menu.render_ctx);

  menu_clear_items(s);
  menu_clear_config(&s->menu);
  if (s->menu.items.items)
    small_vec_destroy(&s->menu.items);
  if (s->menu.config_items.items)
    small_vec_destroy(&s->menu.config_items);
}

static void menu_populate_root(server_t* s) {
  menu_clear_items(s);

  if (s->menu.config_items.length == 0) {
    menu_add_item(s, "(Menu not configured)", MENU_ACTION_NONE, NULL, HANDLE_INVALID, NULL);
    return;
  }

  for (size_t i = 0; i < s->menu.config_items.length; i++) {
    menu_item_spec_t* spec = s->menu.config_items.items[i];
    if (!spec)
      continue;
    const char* label = (spec->action == MENU_ACTION_SEPARATOR) ? NULL : spec->label;
    menu_add_item(s, label, spec->action, spec->cmd, HANDLE_INVALID, spec->icon_path);
  }
}

/*
 * menu_show:
 * Display the root menu at the specified coordinates.
 *
 * This function transitions the WM into `INTERACTION_MENU` mode.
 * Crucially, it establishes an ASYNC grab on both Pointer and Keyboard.
 * This ensures that all subsequent events (clicks, keypresses) are delivered
 * to the WM for menu handling, effectively modalizing the UI.
 */
void menu_show(server_t* s, int16_t x, int16_t y) {
  if (s->menu.visible) {
    menu_hide(s);
    return;
  }

  assert(s->interaction_mode == INTERACTION_NONE);
  s->menu.is_client_list = false;
  s->menu.is_switcher = false;
  menu_populate_root(s);

  uint32_t values_h[] = {s->menu.h};
  xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_HEIGHT, values_h);

  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
  if (x + s->menu.w > screen->width_in_pixels)
    x = screen->width_in_pixels - s->menu.w;
  if (y + s->menu.h > screen->height_in_pixels)
    y = screen->height_in_pixels - s->menu.h;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;

  s->menu.x = x;
  s->menu.y = y;
  s->menu.visible = true;
  s->menu.selected_index = -1;
  s->interaction_mode = INTERACTION_MENU;

  uint32_t values[] = {(uint32_t)x, (uint32_t)y};
  xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);

  xcb_map_window(s->conn, s->menu.window);

  const uint32_t stack_values[] = {XCB_STACK_MODE_ABOVE};
  xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_STACK_MODE, stack_values);

  xcb_grab_pointer_cookie_t pc = xcb_grab_pointer(s->conn, 0, s->root, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION, XCB_GRAB_MODE_ASYNC,
                                                  XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
  xcb_grab_keyboard_cookie_t kc = xcb_grab_keyboard(s->conn, 0, s->root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

#if HXM_DIAG
  xcb_grab_pointer_reply_t* pr = xcb_grab_pointer_reply(s->conn, pc, NULL);
  if (pr) {
    if (pr->status != XCB_GRAB_STATUS_SUCCESS) {
      LOG_WARN("menu_show: pointer grab failed status=%d", pr->status);
    }
    free(pr);
  }
  xcb_grab_keyboard_reply_t* kr = xcb_grab_keyboard_reply(s->conn, kc, NULL);
  if (kr) {
    if (kr->status != XCB_GRAB_STATUS_SUCCESS) {
      LOG_WARN("menu_show: keyboard grab failed status=%d", kr->status);
    }
    free(kr);
  }
#else
  (void)pc;
  (void)kc;
#endif

  menu_handle_expose(s);
}

void menu_show_client_list(server_t* s, int16_t x, int16_t y) {
  if (s->menu.visible) {
    menu_hide(s);
    return;
  }

  assert(s->interaction_mode == INTERACTION_NONE);
  s->menu.is_client_list = true;
  s->menu.is_switcher = false;
  menu_clear_items(s);

  for (uint32_t i = 1; i < s->clients.cap; i++) {
    if (!s->clients.hdr[i].live)
      continue;
    handle_t h = handle_make(i, s->clients.hdr[i].gen);
    client_cold_t* cold = server_ccold(s, h);
    client_hot_t* hot = server_chot(s, h);
    if (!cold || !hot)
      continue;

    char label[256];
    if (hot->state == STATE_UNMAPPED) {
      snprintf(label, sizeof(label), "[%s]", cold->title ? cold->title : "Unnamed");
    }
    else {
      snprintf(label, sizeof(label), "%s", cold->title ? cold->title : "Unnamed");
    }
    menu_add_item(s, label, MENU_ACTION_RESTORE, NULL, h, NULL);
  }

  if (s->menu.items.length == 0) {
    menu_add_item(s, "(No windows)", MENU_ACTION_NONE, NULL, HANDLE_INVALID, NULL);
  }

  uint32_t values_h[] = {s->menu.h};
  xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_HEIGHT, values_h);

  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
  if (x + s->menu.w > screen->width_in_pixels)
    x = screen->width_in_pixels - s->menu.w;
  if (y + s->menu.h > screen->height_in_pixels)
    y = screen->height_in_pixels - s->menu.h;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;

  s->menu.x = x;
  s->menu.y = y;
  s->menu.visible = true;
  s->menu.selected_index = -1;
  s->interaction_mode = INTERACTION_MENU;

  uint32_t values[] = {(uint32_t)x, (uint32_t)y};
  xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);

  xcb_map_window(s->conn, s->menu.window);

  const uint32_t stack_values[] = {XCB_STACK_MODE_ABOVE};
  xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_STACK_MODE, stack_values);

  xcb_grab_pointer_cookie_t pc = xcb_grab_pointer(s->conn, 0, s->root, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION, XCB_GRAB_MODE_ASYNC,
                                                  XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
  xcb_grab_keyboard_cookie_t kc = xcb_grab_keyboard(s->conn, 0, s->root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

#if HXM_DIAG
  xcb_grab_pointer_reply_t* pr = xcb_grab_pointer_reply(s->conn, pc, NULL);
  if (pr) {
    if (pr->status != XCB_GRAB_STATUS_SUCCESS) {
      LOG_WARN("menu_show_client_list: pointer grab failed status=%d", pr->status);
    }
    free(pr);
  }
  xcb_grab_keyboard_reply_t* kr = xcb_grab_keyboard_reply(s->conn, kc, NULL);
  if (kr) {
    if (kr->status != XCB_GRAB_STATUS_SUCCESS) {
      LOG_WARN("menu_show_client_list: keyboard grab failed status=%d", kr->status);
    }
    free(kr);
  }
#else
  (void)pc;
  (void)kc;
#endif

  menu_handle_expose(s);
}

void menu_hide(server_t* s) {
  if (!s->menu.visible)
    return;
  s->menu.visible = false;
  s->menu.is_switcher = false;
  if (s->interaction_mode == INTERACTION_MENU) {
    s->interaction_mode = INTERACTION_NONE;
  }
  xcb_unmap_window(s->conn, s->menu.window);
  xcb_ungrab_pointer(s->conn, XCB_CURRENT_TIME);
  xcb_ungrab_keyboard(s->conn, XCB_CURRENT_TIME);
}

static rgba_t u32_to_rgba(uint32_t c) {
  return (rgba_t){((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, 1.0};
}

void menu_handle_expose(server_t* s) {
  menu_handle_expose_region(s, NULL);
}

void menu_handle_expose_region(server_t* s, const dirty_region_t* dirty) {
  if (s->menu.w == 0 || s->menu.h == 0)
    return;

  cairo_surface_t* target_surface = NULL;
  xcb_pixmap_t pixmap = XCB_NONE;

  if (s->is_test) {
    target_surface = cairo_image_surface_create((s->root_depth == 32) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, s->menu.w, s->menu.h);
  }
  else {
    pixmap = xcb_generate_id(s->conn);
    xcb_create_pixmap(s->conn, s->root_depth, pixmap, s->menu.window, s->menu.w, s->menu.h);
    target_surface = cairo_xcb_surface_create(s->conn, pixmap, s->root_visual_type, s->menu.w, s->menu.h);
  }

  cairo_t* cr = cairo_create(target_surface);
  if (dirty && dirty->valid) {
    dirty_region_t clip = *dirty;
    dirty_region_clamp(&clip, 0, 0, s->menu.w, s->menu.h);
    if (!clip.valid) {
      cairo_destroy(cr);
      if (s->is_test) {
        cairo_surface_destroy(target_surface);
      }
      else {
        cairo_surface_destroy(target_surface);
        xcb_free_pixmap(s->conn, pixmap);
      }
      return;
    }
    cairo_rectangle(cr, clip.x, clip.y, clip.w, clip.h);
    cairo_clip(cr);
  }

  rgba_t bg = u32_to_rgba(s->config.theme.menu_items.color);
  cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
  cairo_paint(cr);

  if (!s->menu.render_ctx.layout) {
    s->menu.render_ctx.layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(s->menu.render_ctx.layout, desc);
    pango_font_description_free(desc);
  }
  else {
    pango_cairo_update_layout(cr, s->menu.render_ctx.layout);
  }

  rgba_t fg = u32_to_rgba(s->config.theme.menu_items_text_color);
  rgba_t sel_bg = u32_to_rgba(s->config.theme.menu_items_active.color);
  rgba_t sel_fg = u32_to_rgba(s->config.theme.menu_items_active_text_color);

  for (size_t i = 0; i < s->menu.items.length; i++) {
    menu_item_t* item = s->menu.items.items[i];
    int16_t item_y = MENU_PADDING + i * MENU_ITEM_HEIGHT;

    if (item->action == MENU_ACTION_SEPARATOR) {
      cairo_set_source_rgba(cr, fg.r, fg.g, fg.b, 0.3);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, MENU_PADDING, item_y + MENU_ITEM_HEIGHT / 2.0);
      cairo_line_to(cr, s->menu.w - MENU_PADDING, item_y + MENU_ITEM_HEIGHT / 2.0);
      cairo_stroke(cr);
      continue;
    }

    bool selected = ((int)i == s->menu.selected_index);

    if (selected) {
      cairo_set_source_rgba(cr, sel_bg.r, sel_bg.g, sel_bg.b, sel_bg.a);
      cairo_rectangle(cr, 0, item_y, s->menu.w, MENU_ITEM_HEIGHT);
      cairo_fill(cr);
    }

    int text_x_offset = MENU_PADDING * 2;

    cairo_surface_t* icon = NULL;
    if (s->menu.is_client_list && item->client != HANDLE_INVALID) {
      client_hot_t* hot = server_chot(s, item->client);
      if (hot)
        icon = hot->icon_surface;
    }
    else {
      icon = item->icon_surface;
    }

    if (icon) {
      int icon_w = cairo_image_surface_get_width(icon);
      int icon_h = cairo_image_surface_get_height(icon);
      double target_size = MENU_ITEM_HEIGHT - 6;
      double scale = target_size / ((icon_w > icon_h) ? icon_w : icon_h);
      double draw_w = icon_w * scale;
      double draw_h = icon_h * scale;
      double icon_y = item_y + (MENU_ITEM_HEIGHT - draw_h) / 2.0;
      cairo_save(cr);
      cairo_translate(cr, text_x_offset, icon_y);
      cairo_scale(cr, scale, scale);
      cairo_set_source_surface(cr, icon, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
      text_x_offset += (int)draw_w + 6;
    }

    rgba_t text_color = selected ? sel_fg : fg;
    cairo_set_source_rgba(cr, text_color.r, text_color.g, text_color.b, text_color.a);
    pango_layout_set_text(s->menu.render_ctx.layout, item->label ? item->label : "", -1);
    pango_layout_set_width(s->menu.render_ctx.layout, (s->menu.w - text_x_offset - MENU_PADDING) * PANGO_SCALE);
    pango_layout_set_ellipsize(s->menu.render_ctx.layout, PANGO_ELLIPSIZE_END);
    int text_h;
    pango_layout_get_pixel_size(s->menu.render_ctx.layout, NULL, &text_h);
    double text_y = item_y + (MENU_ITEM_HEIGHT - text_h) / 2.0;
    cairo_move_to(cr, text_x_offset, text_y);
    pango_cairo_show_layout(cr, s->menu.render_ctx.layout);
  }

  cairo_destroy(cr);

  if (s->is_test) {
    cairo_surface_flush(target_surface);
    xcb_gcontext_t gc = xcb_generate_id(s->conn);
    uint32_t mask = XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values[] = {0};
    xcb_create_gc(s->conn, gc, s->menu.window, mask, values);
    xcb_put_image(s->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, s->menu.window, gc, (uint16_t)s->menu.w, (uint16_t)s->menu.h, 0, 0, 0, (uint8_t)s->root_depth,
                  (uint32_t)(cairo_image_surface_get_stride(target_surface) * s->menu.h), cairo_image_surface_get_data(target_surface));
    xcb_free_gc(s->conn, gc);
    cairo_surface_destroy(target_surface);
  }
  else {
    cairo_surface_destroy(target_surface);
    xcb_gcontext_t gc = xcb_generate_id(s->conn);
    uint32_t mask = XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t values[] = {0};
    xcb_create_gc(s->conn, gc, s->menu.window, mask, values);
    xcb_copy_area(s->conn, pixmap, s->menu.window, gc, 0, 0, 0, 0, s->menu.w, s->menu.h);
    xcb_free_gc(s->conn, gc);
    xcb_free_pixmap(s->conn, pixmap);
  }
}

void menu_show_switcher(server_t* s, handle_t origin) {
  if (s->menu.visible) {
    menu_hide(s);
  }

  assert(s->interaction_mode == INTERACTION_NONE);
  s->menu.is_client_list = true;
  s->menu.is_switcher = true;
  menu_clear_items(s);

  int origin_index = -1;
  int idx = 0;

  for (list_node_t* node = s->focus_history.next; node != &s->focus_history; node = node->next) {
    client_hot_t* hot = (client_hot_t*)((char*)node - offsetof(client_hot_t, focus_node));
    if (!switcher_is_candidate(s, hot))
      continue;

    client_cold_t* cold = server_ccold(s, hot->self);
    char label[256];
    menu_format_client_label(s, hot, cold, label, sizeof(label));
    menu_add_item(s, label, MENU_ACTION_RESTORE, NULL, hot->self, NULL);

    if (hot->self == origin)
      origin_index = idx;
    idx++;
  }

  if (s->menu.items.length == 0) {
    menu_add_item(s, "(No windows)", MENU_ACTION_NONE, NULL, HANDLE_INVALID, NULL);
  }

  uint32_t values_h[] = {s->menu.h};
  xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_HEIGHT, values_h);

  rect_t geom;
  client_hot_t* origin_hot = (origin != HANDLE_INVALID) ? server_chot(s, origin) : NULL;
  if (origin_hot) {
    wm_get_monitor_geometry(s, origin_hot, &geom);
  }
  else if (s->monitor_count > 0) {
    geom = s->monitors[0].geom;
  }
  else {
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(s->conn)).data;
    geom.x = 0;
    geom.y = 0;
    geom.w = (uint16_t)screen->width_in_pixels;
    geom.h = (uint16_t)screen->height_in_pixels;
  }

  int32_t x = geom.x + ((int32_t)geom.w - (int32_t)s->menu.w) / 2;
  int32_t y = geom.y + ((int32_t)geom.h - (int32_t)s->menu.h) / 2;

  if (x < geom.x)
    x = geom.x;
  if (y < geom.y)
    y = geom.y;
  if (x + s->menu.w > (int32_t)(geom.x + geom.w))
    x = (geom.x + (int32_t)geom.w) - (int32_t)s->menu.w;
  if (y + s->menu.h > (int32_t)(geom.y + geom.h))
    y = (geom.y + (int32_t)geom.h) - (int32_t)s->menu.h;

  s->menu.x = (int16_t)x;
  s->menu.y = (int16_t)y;
  s->menu.visible = true;
  s->menu.selected_index = origin_index;
  s->interaction_mode = INTERACTION_MENU;

  uint32_t values[] = {(uint32_t)s->menu.x, (uint32_t)s->menu.y};
  xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);

  xcb_map_window(s->conn, s->menu.window);

  const uint32_t stack_values[] = {XCB_STACK_MODE_ABOVE};
  xcb_configure_window(s->conn, s->menu.window, XCB_CONFIG_WINDOW_STACK_MODE, stack_values);

  xcb_grab_pointer_cookie_t pc = xcb_grab_pointer(s->conn, 0, s->root, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION, XCB_GRAB_MODE_ASYNC,
                                                  XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
  xcb_grab_keyboard_cookie_t kc = xcb_grab_keyboard(s->conn, 0, s->root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

#if HXM_DIAG
  xcb_grab_pointer_reply_t* pr = xcb_grab_pointer_reply(s->conn, pc, NULL);
  if (pr) {
    if (pr->status != XCB_GRAB_STATUS_SUCCESS) {
      LOG_WARN("menu_show_switcher: pointer grab failed status=%d", pr->status);
    }
    free(pr);
  }
  xcb_grab_keyboard_reply_t* kr = xcb_grab_keyboard_reply(s->conn, kc, NULL);
  if (kr) {
    if (kr->status != XCB_GRAB_STATUS_SUCCESS) {
      LOG_WARN("menu_show_switcher: keyboard grab failed status=%d", kr->status);
    }
    free(kr);
  }
#else
  (void)pc;
  (void)kc;
#endif

  menu_handle_expose(s);
}

bool menu_switcher_step(server_t* s, int dir) {
  if (!s->menu.visible || !s->menu.is_switcher)
    return false;

  int start = s->menu.selected_index;
  if (dir == 0) {
    int first = menu_find_first_selectable(s);
    if (first != s->menu.selected_index && first >= 0) {
      s->menu.selected_index = first;
      menu_handle_expose(s);
      return true;
    }
    return false;
  }

  if (start < 0)
    start = menu_find_first_selectable(s);
  int next = menu_find_next_selectable(s, start, dir);
  if (next != s->menu.selected_index && next >= 0) {
    s->menu.selected_index = next;
    menu_handle_expose(s);
    return true;
  }
  return false;
}

handle_t menu_switcher_selected_client(const server_t* s) {
  if (!s || !s->menu.visible || s->menu.selected_index < 0 || s->menu.selected_index >= (int32_t)s->menu.items.length)
    return HANDLE_INVALID;

  menu_item_t* item = s->menu.items.items[s->menu.selected_index];
  if (!item || item->action != MENU_ACTION_RESTORE)
    return HANDLE_INVALID;
  return item->client;
}

void menu_handle_pointer_motion(server_t* s, int16_t x, int16_t y) {
  int32_t local_x = (int32_t)x - (int32_t)s->menu.x;
  int32_t local_y = (int32_t)y - (int32_t)s->menu.y;

  if (local_x < 0 || local_x >= (int32_t)s->menu.w || local_y < 0 || local_y >= (int32_t)s->menu.h) {
    if (s->menu.selected_index != -1) {
      s->menu.selected_index = -1;
      menu_handle_expose(s);
    }
    return;
  }

  int32_t index = (local_y - MENU_PADDING) / MENU_ITEM_HEIGHT;
  if (index < 0 || index >= (int32_t)s->menu.items.length) {
    index = -1;
  }
  else {
    menu_item_t* item = s->menu.items.items[index];
    if (item->action == MENU_ACTION_SEPARATOR)
      index = -1;
  }

  if (index != s->menu.selected_index) {
    s->menu.selected_index = index;
    menu_handle_expose(s);
  }
}

extern volatile sig_atomic_t g_shutdown_pending;
extern volatile sig_atomic_t g_restart_pending;
extern volatile sig_atomic_t g_reload_pending;

static void spawn(const char* cmd) {
  pid_t p = fork();
  if (p < 0)
    return;

  if (p == 0) {
    pid_t p2 = fork();
    if (p2 < 0)
      _exit(1);
    if (p2 == 0) {
      setsid();
      execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
      _exit(1);
    }
    _exit(0);
  }

  int status = 0;
  (void)waitpid(p, &status, 0);
}

void menu_handle_button_press(server_t* s, xcb_button_press_event_t* ev) {
  int32_t local_x = (int32_t)ev->root_x - (int32_t)s->menu.x;
  int32_t local_y = (int32_t)ev->root_y - (int32_t)s->menu.y;

  if (local_x < 0 || local_x >= (int32_t)s->menu.w || local_y < 0 || local_y >= (int32_t)s->menu.h) {
    // Keep context menu visible on background right-clicks.
    if (ev->detail != 3) {
      menu_hide(s);
    }
  }
}

static void menu_activate_selected(server_t* s);

void menu_handle_button_release(server_t* s, xcb_button_release_event_t* ev) {
  int32_t local_x = (int32_t)ev->root_x - (int32_t)s->menu.x;
  int32_t local_y = (int32_t)ev->root_y - (int32_t)s->menu.y;

  if (local_x >= 0 && local_x < (int32_t)s->menu.w && local_y >= 0 && local_y < (int32_t)s->menu.h) {
    // Activate items with left-click only.
    if (ev->detail != 1) {
      return;
    }

    int32_t index = (local_y - MENU_PADDING) / MENU_ITEM_HEIGHT;
    if (index >= 0 && index < (int32_t)s->menu.items.length) {
      menu_item_t* item = s->menu.items.items[index];
      if (item->action != MENU_ACTION_SEPARATOR) {
        s->menu.selected_index = index;
        menu_activate_selected(s);
      }
    }
  }
  else {
    if (ev->detail != 3) {
      menu_hide(s);
    }
  }
}

static int menu_find_next_selectable(server_t* s, int start, int dir) {
  if (s->menu.items.length == 0)
    return -1;

  int i = start;
  for (size_t step = 0; step < s->menu.items.length; step++) {
    i += dir;
    if (i < 0)
      i = (int)s->menu.items.length - 1;
    if (i >= (int)s->menu.items.length)
      i = 0;

    menu_item_t* item = s->menu.items.items[i];
    if (!item)
      continue;
    if (item->action == MENU_ACTION_SEPARATOR)
      continue;
    if (item->action == MENU_ACTION_NONE)
      continue;
    return i;
  }
  return -1;
}

static int menu_find_first_selectable(server_t* s) {
  for (size_t i = 0; i < s->menu.items.length; i++) {
    menu_item_t* item = s->menu.items.items[i];
    if (!item)
      continue;
    if (item->action == MENU_ACTION_SEPARATOR)
      continue;
    if (item->action == MENU_ACTION_NONE)
      continue;
    return (int)i;
  }
  return -1;
}

static void menu_do_restore(server_t* s, handle_t client) {
  if (client == HANDLE_INVALID)
    return;

  client_hot_t* hot = server_chot(s, client);
  if (!hot)
    return;

  if (hot->state == STATE_UNMAPPED) {
    LOG_DEBUG("menu_do_restore: Restoring client %lx", client);
    wm_client_restore(s, client);
  }

  wm_set_focus(s, client);
  stack_raise(s, client);
}

static void menu_activate_selected(server_t* s) {
  if (!s->menu.visible)
    return;
  if (s->menu.selected_index < 0 || s->menu.selected_index >= (int)s->menu.items.length)
    return;

  menu_item_t* item = s->menu.items.items[s->menu.selected_index];
  if (!item)
    return;
  if (item->action == MENU_ACTION_SEPARATOR)
    return;

  LOG_INFO("Menu Action: %d", item->action);

  menu_hide(s);

  switch (item->action) {
    case MENU_ACTION_EXEC:
      if (item->cmd)
        spawn(item->cmd);
      break;
    case MENU_ACTION_EXIT:
      g_shutdown_pending = 1;
      server_schedule_timer(s, 1);
      break;
    case MENU_ACTION_RESTART:
      g_restart_pending = 1;
      server_schedule_timer(s, 1);
      break;
    case MENU_ACTION_RELOAD:
      g_reload_pending = 1;
      server_schedule_timer(s, 1);
      break;
    case MENU_ACTION_RESTORE:
      menu_do_restore(s, item->client);
      break;
    default:
      break;
  }
}

void menu_handle_key_press(server_t* s, xcb_key_press_event_t* ev) {
  if (!s->menu.visible)
    return;

  xcb_keysym_t sym = xcb_key_symbols_get_keysym(s->keysyms, ev->detail, 0);

  if (s->menu.is_switcher) {
    switch (sym) {
      case XK_Escape:
        wm_switcher_cancel(s);
        return;
      case XK_Tab: {
        int dir = (ev->state & XCB_MOD_MASK_SHIFT) ? -1 : 1;
        wm_switcher_step(s, dir);
        return;
      }
      case XK_ISO_Left_Tab:
        wm_switcher_step(s, -1);
        return;
      case XK_Return:
      case XK_KP_Enter:
      case XK_space:
        wm_switcher_commit(s);
        return;
      default:
        break;
    }
  }

  switch (sym) {
    case XK_Escape:
      menu_hide(s);
      return;

    case XK_Up: {
      int start = s->menu.selected_index;
      if (start < 0)
        start = menu_find_first_selectable(s);
      int next = menu_find_next_selectable(s, start, -1);
      if (next != s->menu.selected_index) {
        s->menu.selected_index = next;
        menu_handle_expose(s);
      }
      return;
    }

    case XK_Down: {
      int start = s->menu.selected_index;
      if (start < 0)
        start = menu_find_first_selectable(s);
      int next = menu_find_next_selectable(s, start, +1);
      if (next != s->menu.selected_index) {
        s->menu.selected_index = next;
        menu_handle_expose(s);
      }
      return;
    }

    case XK_Home: {
      int first = menu_find_first_selectable(s);
      if (first != s->menu.selected_index) {
        s->menu.selected_index = first;
        menu_handle_expose(s);
      }
      return;
    }

    case XK_Return:
    case XK_KP_Enter:
    case XK_space:
      if (s->menu.selected_index < 0) {
        s->menu.selected_index = menu_find_first_selectable(s);
      }
      menu_activate_selected(s);
      return;

    default:
      break;
  }
}
