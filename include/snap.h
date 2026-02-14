/*
 * snap.h - Snap-to-edge helpers
 */

#ifndef SNAP_H
#define SNAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "client.h"

typedef struct snap_candidate {
  bool active;
  snap_edge_t edge;
  rect_t rect;
} snap_candidate_t;

snap_candidate_t snap_compute_candidate(int px, int py, rect_t wa, int threshold_px);

#ifdef __cplusplus
}
#endif

#endif /* SNAP_H */
