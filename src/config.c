/* src/config.c
 * Configuration file parsing and handling
 *
 * This module parses the `hxm.conf` and `themerc` files.
 * Format is line-based key-value pairs: `key = value`.
 *
 * It supports:
 * - Basic types: integers, booleans, strings.
 * - Colors (hex).
 * - Keybindings (modifiers + keysym + action).
 * - Window Rules (regex matching -> placement/layer/desktop).
 */

#include "config.h"

#include <X11/keysym.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "client.h"
#include "hxm.h"

#define DEFAULT_ACTIVE_BG 0x4c597d
#define DEFAULT_ACTIVE_FG 0xffffff
#define DEFAULT_ACTIVE_BORDER 0x7a8aa2
#define DEFAULT_INACTIVE_BG 0x333333
#define DEFAULT_INACTIVE_FG 0x888888
#define DEFAULT_INACTIVE_BORDER 0x444444
#define DEFAULT_MENU_BG 0x333333
#define DEFAULT_MENU_FG 0xcccccc
#define DEFAULT_MENU_SEL_BG 0x4c597d
#define DEFAULT_MENU_SEL_FG 0xffffff
#define DEFAULT_BORDER_WIDTH 2
#define DEFAULT_TITLE_HEIGHT 20
#define DEFAULT_DESKTOP_COUNT 4
#define DEFAULT_FONT "fixed"

static void add_keybind(config_t* config, uint32_t mods, xcb_keysym_t sym, action_type_t action, const char* cmd) {
    key_binding_t* b = calloc(1, sizeof(*b));
    b->modifiers = mods;
    b->keysym = sym;
    b->action = action;
    if (cmd) b->exec_cmd = strdup(cmd);
    small_vec_push(&config->key_bindings, b);
}

void config_init_defaults(config_t* config) {
    memset(config, 0, sizeof(*config));

    config->theme.window_active_title.color = DEFAULT_ACTIVE_BG;
    config->theme.window_active_title.flags = BG_SOLID | BG_FLAT;
    config->theme.window_active_label_text_color = DEFAULT_ACTIVE_FG;
    config->theme.window_active_border_color = DEFAULT_ACTIVE_BORDER;

    config->theme.window_inactive_title.color = DEFAULT_INACTIVE_BG;
    config->theme.window_inactive_title.flags = BG_SOLID | BG_FLAT;
    config->theme.window_inactive_label_text_color = DEFAULT_INACTIVE_FG;
    config->theme.window_inactive_border_color = DEFAULT_INACTIVE_BORDER;

    config->theme.menu_items.color = DEFAULT_MENU_BG;
    config->theme.menu_items.flags = BG_SOLID | BG_FLAT;
    config->theme.menu_items_text_color = DEFAULT_MENU_FG;
    config->theme.menu_items_active.color = DEFAULT_MENU_SEL_BG;
    config->theme.menu_items_active.flags = BG_SOLID | BG_FLAT;
    config->theme.menu_items_active_text_color = DEFAULT_MENU_SEL_FG;

    config->theme.border_width = DEFAULT_BORDER_WIDTH;
    config->theme.title_height = DEFAULT_TITLE_HEIGHT;
    config->theme.handle_height = 6;
    config->theme.label_margin = 2;

    config->desktop_count = DEFAULT_DESKTOP_COUNT;
    config->desktop_names_count = 0;
    config->font_name = strdup(DEFAULT_FONT);
    config->desktop_names = NULL;

    config->focus_raise = true;
    config->fullscreen_use_workarea = false;

    small_vec_init(&config->key_bindings);
    small_vec_init(&config->rules);

    // Default bindings
    add_keybind(config, XCB_MOD_MASK_1, XK_F4, ACTION_CLOSE, NULL);
    add_keybind(config, XCB_MOD_MASK_1, XK_Tab, ACTION_FOCUS_NEXT, NULL);
    add_keybind(config, XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT, XK_Tab, ACTION_FOCUS_PREV, NULL);
    add_keybind(config, XCB_MOD_MASK_4, XK_Return, ACTION_TERMINAL, NULL);
    add_keybind(config, XCB_MOD_MASK_4, XK_d, ACTION_EXEC, "dmenu_run");

    for (int i = 0; i < 9; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", i);
        add_keybind(config, XCB_MOD_MASK_4, XK_1 + i, ACTION_WORKSPACE, buf);
        add_keybind(config, XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT, XK_1 + i, ACTION_MOVE_TO_WORKSPACE, buf);
    }

    add_keybind(config, XCB_MOD_MASK_4, XK_s, ACTION_TOGGLE_STICKY, NULL);
    add_keybind(config, XCB_MOD_MASK_4, XK_Left, ACTION_WORKSPACE_PREV, NULL);
    add_keybind(config, XCB_MOD_MASK_4, XK_Right, ACTION_WORKSPACE_NEXT, NULL);
    add_keybind(config, XCB_MOD_MASK_1, XK_F7, ACTION_MOVE, NULL);
    add_keybind(config, XCB_MOD_MASK_1, XK_F8, ACTION_RESIZE, NULL);
}

void config_destroy(config_t* config) {
    if (config->font_name) {
        free(config->font_name);
        config->font_name = NULL;
    }
    if (config->desktop_names) {
        for (uint32_t i = 0; i < config->desktop_names_count; i++) {
            if (config->desktop_names[i]) free(config->desktop_names[i]);
        }
        free(config->desktop_names);
        config->desktop_names = NULL;
        config->desktop_names_count = 0;
    }
    for (size_t i = 0; i < config->key_bindings.length; i++) {
        key_binding_t* b = config->key_bindings.items[i];
        if (b->exec_cmd) free(b->exec_cmd);
        free(b);
    }
    small_vec_destroy(&config->key_bindings);

    for (size_t i = 0; i < config->rules.length; i++) {
        app_rule_t* r = config->rules.items[i];
        if (r->class_match) free(r->class_match);
        if (r->instance_match) free(r->instance_match);
        if (r->title_match) free(r->title_match);
        free(r);
    }
    small_vec_destroy(&config->rules);
}

static char* trim_whitespace(char* str) {
    char* end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static uint32_t parse_color(const char* val) {
    if (val[0] == '#') val++;
    if (val[0] == '0' && val[1] == 'x') val += 2;
    return (uint32_t)strtoul(val, NULL, 16);
}

static background_style_t parse_appearance_flags(const char* val) {
    background_style_t flags = 0;
    char* copy = strdup(val);
    char* token = strtok(copy, " \t");
    while (token) {
        if (strcasecmp(token, "solid") == 0)
            flags |= BG_SOLID;
        else if (strcasecmp(token, "gradient") == 0)
            flags |= BG_GRADIENT;
        else if (strcasecmp(token, "vertical") == 0)
            flags |= BG_VERTICAL;
        else if (strcasecmp(token, "horizontal") == 0)
            flags |= BG_HORIZONTAL;
        else if (strcasecmp(token, "diagonal") == 0)
            flags |= BG_DIAGONAL;
        else if (strcasecmp(token, "crossdiagonal") == 0)
            flags |= BG_CROSSDIAGONAL;
        else if (strcasecmp(token, "raised") == 0)
            flags |= BG_RAISED;
        else if (strcasecmp(token, "sunken") == 0)
            flags |= BG_SUNKEN;
        else if (strcasecmp(token, "flat") == 0)
            flags |= BG_FLAT;
        else if (strcasecmp(token, "bevel1") == 0)
            flags |= BG_BEVEL1;
        else if (strcasecmp(token, "bevel2") == 0)
            flags |= BG_BEVEL2;
        token = strtok(NULL, " \t");
    }
    free(copy);
    return flags;
}

static void parse_keybind(config_t* config, const char* val) {
    char* copy = strdup(val);
    char* colon = strchr(copy, ':');
    if (!colon) {
        LOG_WARN("Invalid keybind format: %s", val);
        free(copy);
        return;
    }

    *colon = '\0';
    char* keys = trim_whitespace(copy);
    char* action_str = trim_whitespace(colon + 1);

    uint32_t mods = 0;
    xcb_keysym_t sym = XKB_KEY_NoSymbol;

    char* p = keys;
    char* plus;
    while ((plus = strchr(p, '+')) != NULL) {
        *plus = '\0';
        char* mod = trim_whitespace(p);
        if (strcasecmp(mod, "Mod1") == 0 || strcasecmp(mod, "Alt") == 0)
            mods |= XCB_MOD_MASK_1;
        else if (strcasecmp(mod, "Mod2") == 0)
            mods |= XCB_MOD_MASK_2;
        else if (strcasecmp(mod, "Mod3") == 0)
            mods |= XCB_MOD_MASK_3;
        else if (strcasecmp(mod, "Mod4") == 0 || strcasecmp(mod, "Super") == 0)
            mods |= XCB_MOD_MASK_4;
        else if (strcasecmp(mod, "Mod5") == 0)
            mods |= XCB_MOD_MASK_5;
        else if (strcasecmp(mod, "Control") == 0 || strcasecmp(mod, "Ctrl") == 0)
            mods |= XCB_MOD_MASK_CONTROL;
        else if (strcasecmp(mod, "Shift") == 0)
            mods |= XCB_MOD_MASK_SHIFT;
        else if (strcasecmp(mod, "Lock") == 0)
            mods |= XCB_MOD_MASK_LOCK;
        p = plus + 1;
    }

    char* keyname = trim_whitespace(p);
    sym = xkb_keysym_from_name(keyname, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol) {
        LOG_WARN("Invalid keysym: %s", keyname);
        free(copy);
        return;
    }

    action_type_t action = ACTION_NONE;
    char* cmd = NULL;

    if (strcasecmp(action_str, "close") == 0)
        action = ACTION_CLOSE;
    else if (strcasecmp(action_str, "focus_next") == 0)
        action = ACTION_FOCUS_NEXT;
    else if (strcasecmp(action_str, "focus_prev") == 0)
        action = ACTION_FOCUS_PREV;
    else if (strcasecmp(action_str, "terminal") == 0)
        action = ACTION_TERMINAL;
    else if (strncasecmp(action_str, "exec ", 5) == 0) {
        action = ACTION_EXEC;
        cmd = trim_whitespace(action_str + 5);
    } else if (strncasecmp(action_str, "workspace ", 10) == 0) {
        action = ACTION_WORKSPACE;
        cmd = trim_whitespace(action_str + 10);
    } else if (strcasecmp(action_str, "workspace_prev") == 0)
        action = ACTION_WORKSPACE_PREV;
    else if (strcasecmp(action_str, "workspace_next") == 0)
        action = ACTION_WORKSPACE_NEXT;
    else if (strncasecmp(action_str, "move_to_workspace ", 18) == 0) {
        action = ACTION_MOVE_TO_WORKSPACE;
        cmd = trim_whitespace(action_str + 18);
    } else if (strcasecmp(action_str, "toggle_sticky") == 0)
        action = ACTION_TOGGLE_STICKY;
    else if (strcasecmp(action_str, "move") == 0)
        action = ACTION_MOVE;
    else if (strcasecmp(action_str, "resize") == 0)
        action = ACTION_RESIZE;
    else if (strcasecmp(action_str, "restart") == 0)
        action = ACTION_RESTART;
    else if (strcasecmp(action_str, "exit") == 0)
        action = ACTION_EXIT;

    if (action != ACTION_NONE) {
        add_keybind(config, mods, sym, action, cmd);
    } else {
        LOG_WARN("Unknown action: %s", action_str);
    }

    free(copy);
}

static void parse_rule(config_t* config, const char* val) {
    char* copy = strdup(val);
    char* arrow = strstr(copy, "->");
    if (!arrow) {
        LOG_WARN("Invalid rule format (missing '->'): %s", val);
        free(copy);
        return;
    }

    *arrow = '\0';
    char* match_part = trim_whitespace(copy);
    char* action_part = trim_whitespace(arrow + 2);

    app_rule_t* r = calloc(1, sizeof(*r));
    r->type_match = -1;
    r->transient_match = -1;
    r->desktop = -2;
    r->layer = -1;
    r->focus = -1;

    char* p = match_part;
    while (p && *p) {
        char* comma = strchr(p, ',');
        if (comma) *comma = '\0';

        char* kv = trim_whitespace(p);
        char* sep = strchr(kv, ':');
        if (sep) {
            *sep = '\0';
            char* k = trim_whitespace(kv);
            char* v = trim_whitespace(sep + 1);
            if (strcasecmp(k, "class") == 0)
                r->class_match = strdup(v);
            else if (strcasecmp(k, "instance") == 0)
                r->instance_match = strdup(v);
            else if (strcasecmp(k, "title") == 0)
                r->title_match = strdup(v);
            else if (strcasecmp(k, "type") == 0) {
                if (strcasecmp(v, "normal") == 0)
                    r->type_match = WINDOW_TYPE_NORMAL;
                else if (strcasecmp(v, "dialog") == 0)
                    r->type_match = WINDOW_TYPE_DIALOG;
                else if (strcasecmp(v, "dock") == 0)
                    r->type_match = WINDOW_TYPE_DOCK;
                else if (strcasecmp(v, "notification") == 0)
                    r->type_match = WINDOW_TYPE_NOTIFICATION;
                else if (strcasecmp(v, "desktop") == 0)
                    r->type_match = WINDOW_TYPE_DESKTOP;
                else if (strcasecmp(v, "splash") == 0)
                    r->type_match = WINDOW_TYPE_SPLASH;
                else if (strcasecmp(v, "toolbar") == 0)
                    r->type_match = WINDOW_TYPE_TOOLBAR;
                else if (strcasecmp(v, "utility") == 0)
                    r->type_match = WINDOW_TYPE_UTILITY;
            } else if (strcasecmp(k, "transient") == 0) {
                r->transient_match = (strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
            }
        }
        p = comma ? comma + 1 : NULL;
    }

    p = action_part;
    while (p && *p) {
        char* comma = strchr(p, ',');
        if (comma) *comma = '\0';

        char* kv = trim_whitespace(p);
        char* sep = strchr(kv, ':');
        if (sep) {
            *sep = '\0';
            char* k = trim_whitespace(kv);
            char* v = trim_whitespace(sep + 1);
            if (strcasecmp(k, "desktop") == 0) {
                if (strcasecmp(v, "sticky") == 0)
                    r->desktop = -1;
                else
                    r->desktop = atoi(v);
            } else if (strcasecmp(k, "layer") == 0) {
                if (strcasecmp(v, "desktop") == 0)
                    r->layer = LAYER_DESKTOP;
                else if (strcasecmp(v, "below") == 0)
                    r->layer = LAYER_BELOW;
                else if (strcasecmp(v, "normal") == 0)
                    r->layer = LAYER_NORMAL;
                else if (strcasecmp(v, "above") == 0)
                    r->layer = LAYER_ABOVE;
                else if (strcasecmp(v, "dock") == 0)
                    r->layer = LAYER_DOCK;
                else if (strcasecmp(v, "fullscreen") == 0)
                    r->layer = LAYER_FULLSCREEN;
                else if (strcasecmp(v, "overlay") == 0)
                    r->layer = LAYER_OVERLAY;
            } else if (strcasecmp(k, "focus") == 0) {
                r->focus = (strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
            } else if (strcasecmp(k, "placement") == 0) {
                if (strcasecmp(v, "center") == 0)
                    r->placement = PLACEMENT_CENTER;
                else if (strcasecmp(v, "mouse") == 0)
                    r->placement = PLACEMENT_MOUSE;
            }
        }
        p = comma ? comma + 1 : NULL;
    }

    small_vec_push(&config->rules, r);
    free(copy);
}

bool config_load(config_t* config, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        LOG_INFO("Config file not found: %s", path);
        return false;
    }

    char* line = NULL;
    size_t len = 0;
    ssize_t read;
#ifdef HXM_ENABLE_DEBUG_LOGGING
    int line_num = 0;
#endif

    while ((read = getline(&line, &len, f)) != -1) {
#ifdef HXM_ENABLE_DEBUG_LOGGING
        line_num++;
#endif
        char* trimmed = trim_whitespace(line);
        if (trimmed[0] == '#' || trimmed[0] == '\0') continue;

        char* eq = strchr(trimmed, '=');
        if (!eq) {
            LOG_WARN("%s:%d: Missing '=' in config line", path, line_num);
            continue;
        }

        *eq = '\0';
        char* key = trim_whitespace(trimmed);
        char* val = trim_whitespace(eq + 1);

        if (strcmp(key, "active_bg") == 0) {
            config->theme.window_active_title.color = parse_color(val);
        } else if (strcmp(key, "active_fg") == 0) {
            config->theme.window_active_label_text_color = parse_color(val);
        } else if (strcmp(key, "active_border") == 0) {
            config->theme.window_active_border_color = parse_color(val);
        } else if (strcmp(key, "inactive_bg") == 0) {
            config->theme.window_inactive_title.color = parse_color(val);
        } else if (strcmp(key, "inactive_fg") == 0) {
            config->theme.window_inactive_label_text_color = parse_color(val);
        } else if (strcmp(key, "inactive_border") == 0) {
            config->theme.window_inactive_border_color = parse_color(val);
        } else if (strcmp(key, "menu_bg") == 0) {
            config->theme.menu_items.color = parse_color(val);
        } else if (strcmp(key, "menu_fg") == 0) {
            config->theme.menu_items_text_color = parse_color(val);
        } else if (strcmp(key, "menu_sel_bg") == 0) {
            config->theme.menu_items_active.color = parse_color(val);
        } else if (strcmp(key, "menu_sel_fg") == 0) {
            config->theme.menu_items_active_text_color = parse_color(val);
        } else if (strcmp(key, "border_width") == 0) {
            config->theme.border_width = (uint32_t)atoi(val);
        } else if (strcmp(key, "title_height") == 0) {
            config->theme.title_height = (uint32_t)atoi(val);
        } else if (strcmp(key, "desktop_count") == 0) {
            config->desktop_count = (uint32_t)atoi(val);
        } else if (strcmp(key, "desktop_names") == 0) {
            // Free old if any
            if (config->desktop_names) {
                for (uint32_t j = 0; j < config->desktop_names_count; j++) {
                    if (config->desktop_names[j]) free(config->desktop_names[j]);
                }
                free(config->desktop_names);
            }

            // Split by comma
            char* val_copy = strdup(val);
            char* p = val_copy;
            uint32_t count = 0;
            while (*p) {
                if (*p == ',') count++;
                p++;
            }
            count++;  // One more than commas

            // Allocate names array
            config->desktop_names = calloc(count, sizeof(char*));
            config->desktop_names_count = count;
            char* token = strtok(val_copy, ",");
            uint32_t i = 0;
            while (token && i < count) {
                config->desktop_names[i++] = strdup(trim_whitespace(token));
                token = strtok(NULL, ",");
            }
            free(val_copy);
        } else if (strcmp(key, "font_name") == 0) {
            free(config->font_name);
            config->font_name = strdup(val);
        } else if (strcmp(key, "focus_raise") == 0) {
            config->focus_raise = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "fullscreen_use_workarea") == 0) {
            config->fullscreen_use_workarea = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "keybind") == 0) {
            parse_keybind(config, val);
        } else if (strcmp(key, "rule") == 0) {
            parse_rule(config, val);
        } else if (strcmp(key, "clear_keybinds") == 0) {
            for (size_t i = 0; i < config->key_bindings.length; i++) {
                key_binding_t* b = config->key_bindings.items[i];
                if (b->exec_cmd) free(b->exec_cmd);
                free(b);
            }
            small_vec_clear(&config->key_bindings);
        } else {
            LOG_WARN("%s:%d: Unknown config key: %s", path, line_num, key);
        }
    }

    free(line);
    fclose(f);
    LOG_INFO("Loaded config from %s", path);
    return true;
}

bool theme_load(theme_t* theme, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        LOG_INFO("Theme file not found: %s", path);
        return false;
    }

    char* line = NULL;
    size_t len = 0;
    ssize_t read;
#ifdef HXM_ENABLE_DEBUG_LOGGING
    int line_num = 0;
#endif

    while ((read = getline(&line, &len, f)) != -1) {
#ifdef HXM_ENABLE_DEBUG_LOGGING
        line_num++;
#endif
        char* trimmed = trim_whitespace(line);
        if (trimmed[0] == '#' || trimmed[0] == '!' || trimmed[0] == '\0') continue;

        char* colon = strchr(trimmed, ':');
        if (!colon) {
            colon = strchr(trimmed, '=');
        }

        if (!colon) {
            LOG_WARN("%s:%d: Missing ':' or '=' in theme line", path, line_num);
            continue;
        }

        *colon = '\0';
        char* key = trim_whitespace(trimmed);
        char* val = trim_whitespace(colon + 1);

        if (strcmp(key, "border.width") == 0)
            theme->border_width = atoi(val);
        else if (strcmp(key, "padding.width") == 0)
            theme->padding_width = atoi(val);
        else if (strcmp(key, "window.title.height") == 0)
            theme->title_height = atoi(val);
        else if (strcmp(key, "window.handle.height") == 0)
            theme->handle_height = atoi(val);
        else if (strcmp(key, "window.label.margin") == 0)
            theme->label_margin = atoi(val);

        else if (strcmp(key, "window.active.title.bg") == 0)
            theme->window_active_title.flags = parse_appearance_flags(val);
        else if (strcmp(key, "window.active.title.bg.color") == 0)
            theme->window_active_title.color = parse_color(val);
        else if (strcmp(key, "window.active.title.bg.colorTo") == 0)
            theme->window_active_title.color_to = parse_color(val);
        else if (strcmp(key, "window.active.label.text.color") == 0)
            theme->window_active_label_text_color = parse_color(val);
        else if (strcmp(key, "window.active.border.color") == 0)
            theme->window_active_border_color = parse_color(val);
        else if (strcmp(key, "window.active.handle.bg") == 0)
            theme->window_active_handle.flags = parse_appearance_flags(val);
        else if (strcmp(key, "window.active.handle.bg.color") == 0)
            theme->window_active_handle.color = parse_color(val);
        else if (strcmp(key, "window.active.grip.bg") == 0)
            theme->window_active_grip.flags = parse_appearance_flags(val);
        else if (strcmp(key, "window.active.grip.bg.color") == 0)
            theme->window_active_grip.color = parse_color(val);

        else if (strcmp(key, "window.inactive.title.bg") == 0)
            theme->window_inactive_title.flags = parse_appearance_flags(val);
        else if (strcmp(key, "window.inactive.title.bg.color") == 0)
            theme->window_inactive_title.color = parse_color(val);
        else if (strcmp(key, "window.inactive.title.bg.colorTo") == 0)
            theme->window_inactive_title.color_to = parse_color(val);
        else if (strcmp(key, "window.inactive.label.text.color") == 0)
            theme->window_inactive_label_text_color = parse_color(val);
        else if (strcmp(key, "window.inactive.border.color") == 0)
            theme->window_inactive_border_color = parse_color(val);
        else if (strcmp(key, "window.inactive.handle.bg") == 0)
            theme->window_inactive_handle.flags = parse_appearance_flags(val);
        else if (strcmp(key, "window.inactive.handle.bg.color") == 0)
            theme->window_inactive_handle.color = parse_color(val);
        else if (strcmp(key, "window.inactive.grip.bg") == 0)
            theme->window_inactive_grip.flags = parse_appearance_flags(val);
        else if (strcmp(key, "window.inactive.grip.bg.color") == 0)
            theme->window_inactive_grip.color = parse_color(val);

        else if (strcmp(key, "menu.items.bg") == 0)
            theme->menu_items.flags = parse_appearance_flags(val);
        else if (strcmp(key, "menu.items.bg.color") == 0)
            theme->menu_items.color = parse_color(val);
        else if (strcmp(key, "menu.items.text.color") == 0)
            theme->menu_items_text_color = parse_color(val);
        else if (strcmp(key, "menu.items.active.bg") == 0)
            theme->menu_items_active.flags = parse_appearance_flags(val);
        else if (strcmp(key, "menu.items.active.bg.color") == 0)
            theme->menu_items_active.color = parse_color(val);
        else if (strcmp(key, "menu.items.active.text.color") == 0)
            theme->menu_items_active_text_color = parse_color(val);
        else {
            LOG_DEBUG("%s:%d: Unknown theme key: %s", path, line_num, key);
        }
    }

    free(line);
    fclose(f);
    LOG_INFO("Loaded theme from %s (border_width=%u)", path, theme->border_width);
    return true;
}
