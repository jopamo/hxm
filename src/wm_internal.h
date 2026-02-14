/*
 * wm_internal.h - Private shared definitions for the Window Manager module.
 *
 * This header exposes internal functions that are shared between `wm.c`
 * and its helper modules (wm_dirty.c, wm_input.c, etc.) but should not
 * be exposed to the rest of the application (main, event loop).
 */

#ifndef WM_INTERNAL_H
#define WM_INTERNAL_H

#include "event.h"
#include "hxm.h"

#define MIN_FRAME_SIZE 32
#define MAX_FRAME_SIZE 65535

uint32_t wm_clean_mods(uint16_t state);

// Exposed interaction logic
void wm_start_interaction(server_t* s, handle_t h, client_hot_t* hot, bool start_move, int resize_dir, int16_t root_x, int16_t root_y, uint32_t time, bool is_keyboard);

void wm_client_set_maximize(server_t* s, client_hot_t* hot, bool max_horz, bool max_vert);

void wm_publish_workarea(server_t* s, const rect_t* wa);
void wm_send_sync_request(server_t* s, const client_hot_t* hot, uint64_t value, uint32_t time);

bool wm_flush_dirty(server_t* s, uint64_t now);
void wm_set_showing_desktop(server_t* s, bool show);
void wm_install_client_colormap(server_t* s, client_hot_t* hot);
void wm_update_monitors(server_t* s);
void wm_get_monitor_geometry(server_t* s, client_hot_t* hot, rect_t* out_geom);
int wm_monitor_at_point(const server_t* s, int root_x, int root_y);
void wm_set_frame_extents_for_window(server_t* s, xcb_window_t win, bool undecorated);

#endif
