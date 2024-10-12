#pragma once

#include "wm.h"

// Internal helpers used across wm_*.c
uint32_t wm_clean_mods(uint16_t state);

// wm_dirty.c
void wm_publish_workarea(server_t* s, const rect_t* wa);

// wm.c (to be moved to wm_state.c later)
void wm_client_set_maximize(server_t* s, client_hot_t* hot, bool max_horz, bool max_vert);

// wm_desktop.c
void wm_set_showing_desktop(server_t* s, bool show);
