#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

#include "handle.h"

typedef struct server server_t;

typedef enum frame_redraw_mask {
    FRAME_REDRAW_BORDER = 1 << 0,
    FRAME_REDRAW_TITLE = 1 << 1,
    FRAME_REDRAW_ALL = FRAME_REDRAW_BORDER | FRAME_REDRAW_TITLE
} frame_redraw_mask_t;

typedef enum frame_button {
    FRAME_BUTTON_NONE = 0,
    FRAME_BUTTON_CLOSE,
    FRAME_BUTTON_MAXIMIZE,
    FRAME_BUTTON_MINIMIZE
} frame_button_t;

void frame_init_resources(server_t* s);
void frame_cleanup_resources(server_t* s);

void frame_redraw(server_t* s, handle_t h, uint32_t what);
frame_button_t frame_get_button_at(server_t* s, handle_t h, int16_t x, int16_t y);

#endif  // FRAME_H
