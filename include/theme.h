/*
 * theme.h - Visual styling definitions
 *
 * Defines theme_t as loaded from themerc
 * Provides appearance/background style flags and color values used by
 * render/frame/menu code
 *
 * Conventions:
 * - Colors are 0xAARRGGBB in host endian as an integer value
 *   Use helpers below to extract channels safely
 * - background_style_t is a bitmask, BG_SOLID is 0 (no flags)
 *
 * Notes:
 * - This header is pure data + helpers
 * - Parsing/loading is expected to live elsewhere (theme_load.c, etc)
 */

#ifndef THEME_H
#define THEME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* Background/appearance style flags */
typedef enum background_style {
  BG_SOLID = 0,

  BG_GRADIENT = 1u << 0,
  BG_VERTICAL = 1u << 1,
  BG_HORIZONTAL = 1u << 2,
  BG_DIAGONAL = 1u << 3,
  BG_CROSSDIAGONAL = 1u << 4,

  BG_RAISED = 1u << 5,
  BG_SUNKEN = 1u << 6,
  BG_FLAT = 1u << 7,
  BG_BEVEL1 = 1u << 8,
  BG_BEVEL2 = 1u << 9
} background_style_t;

/* Appearance for a themed element */
typedef struct appearance {
  background_style_t flags;

  /* Primary and secondary colors
   * If BG_GRADIENT is unset, color_to may be ignored
   * Encoding: 0xAARRGGBB
   */
  uint32_t color;
  uint32_t color_to;
} appearance_t;

/* Theme data as loaded from themerc */
typedef struct theme {
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

/* ---------- Helpers ---------- */

static inline bool theme_style_has(background_style_t flags, background_style_t bit) {
  return ((uint32_t)flags & (uint32_t)bit) != 0u;
}

static inline bool theme_style_is_gradient(background_style_t flags) {
  return theme_style_has(flags, BG_GRADIENT);
}

/* Color channel extraction for 0xAARRGGBB */
static inline uint8_t theme_color_a(uint32_t argb) {
  return (uint8_t)((argb >> 24) & 0xFFu);
}
static inline uint8_t theme_color_r(uint32_t argb) {
  return (uint8_t)((argb >> 16) & 0xFFu);
}
static inline uint8_t theme_color_g(uint32_t argb) {
  return (uint8_t)((argb >> 8) & 0xFFu);
}
static inline uint8_t theme_color_b(uint32_t argb) {
  return (uint8_t)((argb >> 0) & 0xFFu);
}

/* Convert 0xAARRGGBB to premultiplied or straight doubles (0..1)
 * These are straight (non-premultiplied) components, suitable for
 * cairo_set_source_rgba
 */
static inline double theme_color_a_f(uint32_t argb) {
  return (double)theme_color_a(argb) / 255.0;
}
static inline double theme_color_r_f(uint32_t argb) {
  return (double)theme_color_r(argb) / 255.0;
}
static inline double theme_color_g_f(uint32_t argb) {
  return (double)theme_color_g(argb) / 255.0;
}
static inline double theme_color_b_f(uint32_t argb) {
  return (double)theme_color_b(argb) / 255.0;
}

/* Basic flag validation (keeps call sites honest)
 * Returns true if orientation bits are either 0 or exactly one of them
 */
static inline bool theme_style_orientation_valid(background_style_t flags) {
  uint32_t o = (uint32_t)flags & ((uint32_t)BG_VERTICAL | (uint32_t)BG_HORIZONTAL | (uint32_t)BG_DIAGONAL | (uint32_t)BG_CROSSDIAGONAL);
  return (o == 0u) || (o == (uint32_t)BG_VERTICAL) || (o == (uint32_t)BG_HORIZONTAL) || (o == (uint32_t)BG_DIAGONAL) || (o == (uint32_t)BG_CROSSDIAGONAL);
}

#ifdef __cplusplus
}
#endif

#endif /* THEME_H */
