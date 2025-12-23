#ifndef WM_INTERNAL_H
#define WM_INTERNAL_H

#include "event.h"
#include "hxm.h"

uint32_t wm_clean_mods(uint16_t state);

// Exposed interaction logic
void wm_start_interaction(server_t* s, handle_t h, client_hot_t* hot, bool start_move, int resize_dir, int16_t root_x,
                          int16_t root_y);

void wm_client_set_maximize(server_t* s, client_hot_t* hot, bool max_horz, bool max_vert);

void wm_publish_workarea(server_t* s, const rect_t* wa);
void wm_set_showing_desktop(server_t* s, bool show);
void wm_install_client_colormap(server_t* s, client_hot_t* hot);
void wm_get_monitor_geometry(server_t* s, client_hot_t* hot, rect_t* out_geom);

#endif
