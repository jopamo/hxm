/*
 * snap_preview.h - Preview window management for snap-to-edge
 */

#ifndef SNAP_PREVIEW_H
#define SNAP_PREVIEW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <xcb/xcb.h>

#include "client.h"
#include "event.h"

void snap_preview_init(server_t* s);
void snap_preview_destroy(server_t* s);
void snap_preview_apply(server_t* s, const rect_t* rect, bool show);

#ifdef __cplusplus
}
#endif

#endif /* SNAP_PREVIEW_H */
