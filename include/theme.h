#ifndef THEME_H
#define THEME_H

#include <stdint.h>

typedef enum {
    BG_SOLID = 0,
    BG_GRADIENT = (1 << 0),
    BG_VERTICAL = (1 << 1),
    BG_HORIZONTAL = (1 << 2),
    BG_DIAGONAL = (1 << 3),
    BG_CROSSDIAGONAL = (1 << 4),
    BG_RAISED = (1 << 5),
    BG_SUNKEN = (1 << 6),
    BG_FLAT = (1 << 7),
    BG_BEVEL1 = (1 << 8),
    BG_BEVEL2 = (1 << 9),
} background_style_t;

typedef struct {
    background_style_t flags;
    uint32_t color;
    uint32_t color_to;
} appearance_t;

typedef struct {
    uint32_t border_width;
    uint32_t padding_width;
    uint32_t title_height;
    uint32_t handle_height;
    uint32_t label_margin;

    appearance_t window_active_title;
    uint32_t window_active_label_text_color;
    uint32_t window_active_border_color;
    appearance_t window_active_handle;
    appearance_t window_active_grip;

    appearance_t window_inactive_title;
    uint32_t window_inactive_label_text_color;
    uint32_t window_inactive_border_color;
    appearance_t window_inactive_handle;
    appearance_t window_inactive_grip;

    appearance_t menu_items;
    uint32_t menu_items_text_color;
    appearance_t menu_items_active;
    uint32_t menu_items_active_text_color;
} theme_t;

#endif
