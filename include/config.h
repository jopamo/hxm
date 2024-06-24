#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "containers.h"
#include "theme.h"

typedef enum {
    ACTION_NONE,
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
    ACTION_TOGGLE_STICKY
} action_type_t;

typedef struct {
    uint32_t modifiers;
    xcb_keysym_t keysym;
    action_type_t action;
    char* exec_cmd;
} key_binding_t;

typedef enum {
    PLACEMENT_DEFAULT = 0,
    PLACEMENT_CENTER,
    PLACEMENT_MOUSE,
} placement_policy_t;

typedef struct {
    char* class_match;
    char* instance_match;
    char* title_match;
    int32_t type_match;      // -1 for any, or one of client_type_t
    int8_t transient_match;  // -1 for any, 0 for normal, 1 for transient

    int32_t desktop;  // -2 for don't change, -1 for sticky, >=0 for desktop
    int32_t layer;    // -1 for don't change, or one of stack_layer_t
    int8_t focus;     // -1 for don't change, 0 for no, 1 for yes
    placement_policy_t placement;
} app_rule_t;

typedef struct {
    theme_t theme;

    char* font_name;

    uint32_t desktop_count;
    uint32_t desktop_names_count;
    char** desktop_names;

    small_vec_t key_bindings;
    small_vec_t rules;

    // Policy
    bool focus_raise;
    bool fullscreen_use_workarea;
} config_t;

void config_init_defaults(config_t* config);
bool config_load(config_t* config, const char* path);
bool theme_load(theme_t* theme, const char* path);
void config_destroy(config_t* config);

#endif
