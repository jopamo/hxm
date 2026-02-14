/*
 * config.h - Configuration data structures
 *
 * Defines config_t: runtime settings loaded from hxm.conf and themerc
 *
 * Contains:
 * - theme + font
 * - desktops/workspaces
 * - key bindings
 * - application rules (match + actions)
 * - global policy flags
 *
 * Ownership:
 * - Strings in config_t (font_name, desktop_names, key_binding.exec_cmd, rule
 * match strings) are heap-owned by config_t and freed by config_destroy
 * - small_vec_t key_bindings holds key_binding_t* (or inline structs) depending
 * on ds.h
 * - small_vec_t rules holds app_rule_t* (or inline structs) depending on ds.h
 *
 * Threading:
 * - Not thread-safe
 * - Intended to be loaded/owned by the main server thread during startup/reload
 */

#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "ds.h"
#include "theme.h"

/* Action types referenced by key bindings and menu items */
typedef enum action_type {
  ACTION_NONE = 0,

  ACTION_CLOSE,
  ACTION_FOCUS_NEXT,
  ACTION_FOCUS_PREV,

  ACTION_EXEC,
  ACTION_RESTART,
  ACTION_EXIT,
  ACTION_TERMINAL,

  ACTION_WORKSPACE,
  ACTION_WORKSPACE_PREV,
  ACTION_WORKSPACE_NEXT,

  ACTION_MOVE_TO_WORKSPACE,
  ACTION_MOVE_TO_WORKSPACE_FOLLOW,

  ACTION_TOGGLE_STICKY,
  ACTION_MOVE,
  ACTION_RESIZE
} action_type_t;

typedef struct key_binding {
  uint32_t modifiers;
  xcb_keysym_t keysym;

  action_type_t action;

  /* Only used when action == ACTION_EXEC or ACTION_TERMINAL (if you model it
   * that way) */
  char* exec_cmd;
} key_binding_t;

/* Initial placement policy for newly-managed windows */
typedef enum placement_policy { PLACEMENT_DEFAULT = 0, PLACEMENT_CENTER, PLACEMENT_MOUSE } placement_policy_t;

/* Application rule:
 * Match fields:
 * - NULL means "do not match on this field"
 * - type_match: -1 any, else client_type_t value (defined elsewhere)
 * - transient_match: -1 any, 0 normal, 1 transient
 *
 * Apply fields:
 * - desktop: -2 don't change, -1 sticky, >=0 target desktop
 * - layer: -1 don't change, else stack_layer_t value (defined elsewhere)
 * - focus: -1 don't change, 0 no, 1 yes
 * - bypass_compositor: -1 don't change, 0 unset/allow, 1/2 set value for
 *   _NET_WM_BYPASS_COMPOSITOR
 */
typedef struct app_rule {
  char* class_match;
  char* instance_match;
  char* title_match;

  int32_t type_match;
  int8_t transient_match;

  int32_t desktop;
  int32_t layer;
  int8_t focus;
  int8_t bypass_compositor;

  placement_policy_t placement;
} app_rule_t;

/* Full runtime configuration */
typedef struct config {
  theme_t theme;

  char* font_name;

  uint32_t desktop_count;
  uint32_t desktop_names_count;
  char** desktop_names;

  /* Collection types are defined by ds.h
   * Recommended: small_vec_t of heap-allocated entries (key_binding_t*,
   * app_rule_t*)
   */
  small_vec_t key_bindings;
  small_vec_t rules;

  /* Policy flags */
  bool focus_raise;
  bool fullscreen_use_workarea;

  /* Snap-to-edge */
  bool snap_enable;
  uint32_t snap_threshold_px;
  uint32_t snap_preview_border_px;
  uint32_t snap_preview_color;
} config_t;

/* Initialize config to default values (does not load from disk) */
void config_init_defaults(config_t* config);

/* Load config from a file path
 * Returns true on success, false on parse/IO error
 * On failure, config should remain in a valid state (either defaults or
 * partially-loaded but destroyable)
 */
bool config_load(config_t* config, const char* path);

/* Load theme from themerc file into theme
 * Kept here for convenience since theme is config-owned
 */
bool theme_load(theme_t* theme, const char* path);

/* Free all heap-owned memory inside config */
void config_destroy(config_t* config);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
