/* snap.c - Snap-to-edge helpers */

#include "snap.h"

#include <stdint.h>

snap_candidate_t snap_compute_candidate(int px, int py, rect_t wa, int threshold_px) {
    (void)py;  // Currently unused

    snap_candidate_t out = {0};
    out.active = false;
    out.edge = SNAP_NONE;
    out.rect = wa;

    if (wa.w < 2 || threshold_px <= 0) return out;

    int thr = threshold_px;
    if (thr < 0) thr = 0;

    int half = (int)(wa.w / 2);
    if (half < 1) return out;

    // clamp threshold so left/right zones can't overlap
    if (thr > half) thr = half;

    int left_edge = (int)wa.x;

    // compute right edge safely in 64-bit then clamp back
    int64_t right64 = (int64_t)wa.x + (int64_t)wa.w;
    int right_edge = (right64 > INT32_MAX) ? INT32_MAX : (right64 < INT32_MIN ? INT32_MIN : (int)right64);

    if (px <= left_edge + thr) {
        out.active = true;
        out.edge = SNAP_LEFT;
        out.rect.x = wa.x;
        out.rect.y = wa.y;
        out.rect.w = (uint16_t)half;
        out.rect.h = wa.h;
        return out;
    }

    if (px >= right_edge - thr) {
        out.active = true;
        out.edge = SNAP_RIGHT;
        out.rect.x = (int16_t)(wa.x + half);
        out.rect.y = wa.y;
        out.rect.w = (uint16_t)((int)wa.w - half);  // right gets the remainder when odd
        out.rect.h = wa.h;
        return out;
    }

    return out;
}
