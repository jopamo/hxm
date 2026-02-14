#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "slotmap.h"
#include "wm.h"

// These should be real helpers in wm.c or wm_props.c that contain pure logic
size_t wm_build_net_wm_state_atoms(const server_t* s, const client_hot_t* hot, xcb_atom_t* out_atoms, size_t cap);

static server_t test_server_minimal(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;

  // No XCB connection needed for pure logic tests
  // Do not call xcb_get_visualtype here
  s.root_depth = 24;
  s.root_visual_type = NULL;

  bool ok = slotmap_init(&s.clients, 16, sizeof(client_hot_t), sizeof(client_cold_t));
  assert(ok);

  return s;
}

static void test_root_dirty_flags(void) {
  server_t s = test_server_minimal();

  // 1. Mark client list dirty on manage
  s.root_dirty = 0;
  s.root_dirty |= ROOT_DIRTY_CLIENT_LIST;
  assert((s.root_dirty & ROOT_DIRTY_CLIENT_LIST) != 0);

  // 2. Mark client list dirty on unmanage
  s.root_dirty = 0;
  handle_t h = slotmap_alloc(&s.clients, NULL, NULL);
  assert(h != HANDLE_INVALID);

  slotmap_free(&s.clients, h);
  s.root_dirty |= ROOT_DIRTY_CLIENT_LIST;
  assert((s.root_dirty & ROOT_DIRTY_CLIENT_LIST) != 0);

  // 3. Mark active window dirty on focus change
  s.root_dirty = 0;
  s.root_dirty |= ROOT_DIRTY_ACTIVE_WINDOW;
  assert((s.root_dirty & ROOT_DIRTY_ACTIVE_WINDOW) != 0);

  slotmap_destroy(&s.clients);
  printf("test_root_dirty_flags passed\n");
}

static void test_net_wm_state_atoms_fullscreen_and_urgent(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;

  // Mock the atoms your helper will reference through server_t
  s.atoms.net_wm_state_fullscreen = 100;
  s.atoms.net_wm_state_above = 101;
  s.atoms.net_wm_state_below = 102;
  s.atoms.net_wm_state_demands_attention = 103;

  client_hot_t hot;
  memset(&hot, 0, sizeof(hot));
  hot.layer = LAYER_FULLSCREEN;
  hot.base_layer = LAYER_NORMAL;

  // Whatever internal flag you use for urgency/attention
  hot.flags = CLIENT_FLAG_URGENT;

  xcb_atom_t atoms[8];
  size_t n = wm_build_net_wm_state_atoms(&s, &hot, atoms, 8);

  assert(n == 2);
  assert(atoms[0] == s.atoms.net_wm_state_fullscreen);
  assert(atoms[1] == s.atoms.net_wm_state_demands_attention);

  printf("test_net_wm_state_atoms_fullscreen_and_urgent passed\n");
}

static void test_net_wm_state_atoms_above_only(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;

  s.atoms.net_wm_state_fullscreen = 100;
  s.atoms.net_wm_state_above = 101;
  s.atoms.net_wm_state_below = 102;
  s.atoms.net_wm_state_demands_attention = 103;

  client_hot_t hot;
  memset(&hot, 0, sizeof(hot));
  hot.layer = LAYER_ABOVE;
  hot.base_layer = LAYER_NORMAL;
  hot.state_above = true;

  xcb_atom_t atoms[8];
  size_t n = wm_build_net_wm_state_atoms(&s, &hot, atoms, 8);

  assert(n == 1);
  assert(atoms[0] == s.atoms.net_wm_state_above);

  printf("test_net_wm_state_atoms_above_only passed\n");
}

static void test_net_wm_state_atoms_below_only(void) {
  server_t s;
  memset(&s, 0, sizeof(s));
  s.is_test = true;

  s.atoms.net_wm_state_fullscreen = 100;
  s.atoms.net_wm_state_above = 101;
  s.atoms.net_wm_state_below = 102;
  s.atoms.net_wm_state_demands_attention = 103;

  client_hot_t hot;
  memset(&hot, 0, sizeof(hot));
  hot.layer = LAYER_BELOW;
  hot.base_layer = LAYER_NORMAL;
  hot.state_below = true;

  xcb_atom_t atoms[8];
  size_t n = wm_build_net_wm_state_atoms(&s, &hot, atoms, 8);

  assert(n == 1);
  assert(atoms[0] == s.atoms.net_wm_state_below);

  printf("test_net_wm_state_atoms_below_only passed\n");
}

int main(void) {
  test_root_dirty_flags();

  // These become real tests once your logic is in a pure helper
  test_net_wm_state_atoms_fullscreen_and_urgent();
  test_net_wm_state_atoms_above_only();
  test_net_wm_state_atoms_below_only();

  return 0;
}
