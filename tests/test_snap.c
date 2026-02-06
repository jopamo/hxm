/*
 * Basic unit tests for snap_compute_candidate
 */

#include <assert.h>

#include "snap.h"

static void test_left_snap(void) {
  rect_t wa = {0, 0, 100, 80};
  snap_candidate_t c = snap_compute_candidate(4, 10, wa, 10);
  assert(c.active);
  assert(c.edge == SNAP_LEFT);
  assert(c.rect.x == 0 && c.rect.y == 0);
  assert(c.rect.w == 50 && c.rect.h == 80);
}

static void test_right_snap(void) {
  rect_t wa = {0, 0, 101, 80};
  snap_candidate_t c = snap_compute_candidate(100, 20, wa, 5);
  assert(c.active);
  assert(c.edge == SNAP_RIGHT);
  assert(c.rect.x == 50);
  assert(c.rect.w == 51);
}

static void test_none(void) {
  rect_t wa = {0, 0, 100, 80};
  snap_candidate_t c = snap_compute_candidate(40, 20, wa, 10);
  assert(!c.active);
  assert(c.edge == SNAP_NONE);
}

int main(void) {
  test_left_snap();
  test_right_snap();
  test_none();
  return 0;
}
