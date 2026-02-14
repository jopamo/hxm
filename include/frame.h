/*
 * frame.h - Window frame rendering and hit-testing
 *
 * This interface owns:
 * - Per-server frame rendering resources (fonts, surfaces, caches)
 * - Redrawing frame decorations (border/title/buttons) for a client by handle
 * - Hit-testing frame buttons
 *
 * Design notes:
 * - All functions are expected to be called from the server's main thread
 * - handle_t refers to a managed client/window object owned by the server
 * - frame_redraw* mark or paint decorations based on current theme/state
 * - frame_flush pushes any pending drawing to the X server (or backing surface)
 *
 * Contract:
 * - frame_init_resources must be called once during server startup
 * - frame_cleanup_resources must be called once during server shutdown
 * - Passing HANDLE_INVALID is a no-op for functions that operate on a handle
 * - If the handle does not resolve to a live client, calls are no-ops and
 * return safe defaults
 */

#ifndef FRAME_H
#define FRAME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "handle.h"
#include "hxm.h"

typedef struct server server_t;

/* Redraw flags for the decoration subparts */
typedef enum frame_redraw_mask {
  FRAME_REDRAW_BORDER = 1u << 0,
  FRAME_REDRAW_TITLE = 1u << 1,
  FRAME_REDRAW_BUTTONS = 1u << 2,
  FRAME_REDRAW_ALL = (FRAME_REDRAW_BORDER | FRAME_REDRAW_TITLE | FRAME_REDRAW_BUTTONS)
} frame_redraw_mask_t;

/* Button identifiers returned by hit-testing */
typedef enum frame_button { FRAME_BUTTON_NONE = 0, FRAME_BUTTON_CLOSE, FRAME_BUTTON_MAXIMIZE, FRAME_BUTTON_MINIMIZE } frame_button_t;

/* Server-global resource lifecycle */
void frame_init_resources(server_t* s);
void frame_cleanup_resources(server_t* s);

/* Redraw requested subparts of a frame
 * what is a bitmask of frame_redraw_mask_t
 */
void frame_redraw(server_t* s, handle_t h, uint32_t what);

/* Redraw only the dirty region (damage)
 * dirty must point to a valid region description
 */
void frame_redraw_region(server_t* s, handle_t h, const dirty_region_t* dirty);

/* Flush any pending drawing operations for this frame */
void frame_flush(server_t* s, handle_t h);

/* Hit-test for frame buttons
 * x,y are relative to the frame (not root) coordinate space
 */
frame_button_t frame_get_button_at(server_t* s, handle_t h, int16_t x, int16_t y);

/* Helpers */
static inline uint32_t frame_redraw_mask(frame_redraw_mask_t m) {
  return (uint32_t)m;
}

static inline int frame_redraw_mask_valid(uint32_t what) {
  return (what & ~(uint32_t)FRAME_REDRAW_ALL) == 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* FRAME_H */
