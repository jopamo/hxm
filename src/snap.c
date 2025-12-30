/* snap.c - Snap-to-edge helpers */

#include "snap.h"

snap_candidate_t snap_compute_candidate(int px, int py, rect_t wa, int threshold_px) {
    (void)py; /* Currently unused */
    snap_candidate_t out;
    out.active = false;
    out.edge = SNAP_NONE;
    out.rect = wa;

    int left_edge = wa.x;
    int right_edge = wa.x + (int)wa.w;

    if (px <= left_edge + threshold_px) {
        out.active = true;
        out.edge = SNAP_LEFT;
        out.rect.x = wa.x;
        out.rect.y = wa.y;
        out.rect.w = (uint16_t)(wa.w / 2);
        out.rect.h = wa.h;
        return out;
    }

    if (px >= right_edge - threshold_px) {
        out.active = true;
        out.edge = SNAP_RIGHT;
        out.rect.x = (int16_t)(wa.x + (int)(wa.w / 2));
        out.rect.y = wa.y;
        out.rect.w = (uint16_t)(wa.w - (wa.w / 2));
        out.rect.h = wa.h;
        return out;
    }

    return out;
}
