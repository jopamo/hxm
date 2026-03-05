#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"

typedef enum scenario_kind {
  SCENARIO_ALL = 0,
  SCENARIO_FOCUS_CYCLE,
  SCENARIO_STACKING_OPS,
  SCENARIO_MOVE_RESIZE,
  SCENARIO_FLUSH_LOOPS,
} scenario_kind_t;

static uint64_t parse_u64(const char* s, const char* name) {
  char* end = NULL;
  unsigned long long v = strtoull(s, &end, 10);
  if (!s || *s == '\0' || !end || *end != '\0') {
    fprintf(stderr, "invalid %s: %s\n", name, s ? s : "(null)");
    exit(2);
  }
  return (uint64_t)v;
}

static scenario_kind_t parse_scenario(const char* s) {
  if (strcmp(s, "all") == 0)
    return SCENARIO_ALL;
  if (strcmp(s, "focus_cycle") == 0)
    return SCENARIO_FOCUS_CYCLE;
  if (strcmp(s, "stacking_ops") == 0)
    return SCENARIO_STACKING_OPS;
  if (strcmp(s, "move_resize") == 0)
    return SCENARIO_MOVE_RESIZE;
  if (strcmp(s, "flush_loops") == 0)
    return SCENARIO_FLUSH_LOOPS;

  fprintf(stderr, "unknown scenario: %s\n", s);
  exit(2);
}

static void print_usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s [--scenario all|focus_cycle|stacking_ops|move_resize|flush_loops] "
          "[--iters N] [--clients N]\n",
          argv0);
}

static void init_clients(client_hot_t* clients, size_t n) {
  if (!clients)
    return;

  for (size_t i = 0; i < n; ++i) {
    client_hot_t* c = &clients[i];
    memset(c, 0, sizeof(*c));

    c->self = handle_make((uint32_t)i + 1u, 1u);
    c->xid = (xcb_window_t)(1000u + (uint32_t)i);
    c->frame = (xcb_window_t)(2000u + (uint32_t)i);

    c->server = (rect_t){.x = (int16_t)(i % 32), .y = (int16_t)(i % 24), .w = 800, .h = 600};
    c->desired = c->server;
    c->pending = c->server;

    c->hints.min_w = 64;
    c->hints.min_h = 64;
    c->hints.max_w = 4096;
    c->hints.max_h = 4096;
    c->hints_flags = 0;

    c->stacking_index = (int32_t)i;
    c->stacking_layer = (int8_t)LAYER_NORMAL;
    c->layer = (uint8_t)LAYER_NORMAL;
    c->base_layer = (uint8_t)LAYER_NORMAL;

    list_init(&c->focus_node);
    list_init(&c->transient_sibling);
    list_init(&c->transients_head);

    dirty_region_reset(&c->damage_region);
    dirty_region_reset(&c->frame_damage);
  }
}

static uint64_t run_focus_cycle(client_hot_t* clients, size_t n, uint64_t iters) {
  LIST_HEAD(focus_head);

  for (size_t i = 0; i < n; ++i)
    list_push_back(&focus_head, &clients[i].focus_node);

  uint64_t ops = 0;
  list_node_t* node = focus_head.next;

  for (uint64_t i = 0; i < iters; ++i) {
    if (node == &focus_head)
      node = node->next;

    client_hot_t* c = list_entry(node, client_hot_t, focus_node);
    c->dirty ^= DIRTY_FOCUS;
    ops++;

    node = node->next;
  }

  return ops;
}

static uint64_t run_stacking_ops(client_hot_t* clients, size_t n, uint64_t iters) {
  uint32_t* order = calloc(n, sizeof(*order));
  if (!order) {
    fprintf(stderr, "failed to allocate order array\n");
    exit(1);
  }

  for (size_t i = 0; i < n; ++i)
    order[i] = (uint32_t)i;

  uint64_t ops = 0;

  for (uint64_t i = 0; i < iters; ++i) {
    size_t pos = (size_t)(i % (uint64_t)n);
    uint32_t moved = order[pos];

    if ((i & 1u) == 0u) {
      if (pos + 1 < n)
        memmove(&order[pos], &order[pos + 1], (n - pos - 1) * sizeof(*order));
      order[n - 1] = moved;

      for (size_t j = pos; j < n; ++j) {
        clients[order[j]].stacking_index = (int32_t)j;
        ops++;
      }
    } else {
      if (pos > 0)
        memmove(&order[1], &order[0], pos * sizeof(*order));
      order[0] = moved;

      for (size_t j = 0; j <= pos; ++j) {
        clients[order[j]].stacking_index = (int32_t)j;
        ops++;
      }
    }
  }

  free(order);
  return ops;
}

static uint64_t run_move_resize(client_hot_t* clients, size_t n, uint64_t iters) {
  uint64_t ops = 0;

  for (uint64_t i = 0; i < iters; ++i) {
    size_t idx = (size_t)(i % (uint64_t)n);
    client_hot_t* c = &clients[idx];

    int16_t dx = (int16_t)((i & 7u) - 3);
    int16_t dy = (int16_t)(((i >> 3u) & 7u) - 3);
    int32_t nw = (int32_t)c->desired.w + (int32_t)((i & 3u) - 1);
    int32_t nh = (int32_t)c->desired.h + (int32_t)(((i >> 2u) & 3u) - 1);

    if (nw < 64)
      nw = 64;
    if (nh < 64)
      nh = 64;
    if (nw > 4096)
      nw = 4096;
    if (nh > 4096)
      nh = 4096;

    c->desired.x = (int16_t)(c->desired.x + dx);
    c->desired.y = (int16_t)(c->desired.y + dy);
    c->desired.w = (uint16_t)nw;
    c->desired.h = (uint16_t)nh;

    c->pending = c->desired;
    c->server = c->pending;
    c->dirty |= DIRTY_GEOM;

    ops += 3;
  }

  return ops;
}

static uint64_t run_flush_loops(client_hot_t* clients, size_t n, uint64_t iters) {
  uint64_t ops = 0;

  for (uint64_t i = 0; i < iters; ++i) {
    size_t idx = (size_t)(i % (uint64_t)n);
    client_hot_t* c = &clients[idx];

    int16_t x = (int16_t)((i * 13u) % 1920u);
    int16_t y = (int16_t)((i * 7u) % 1080u);
    uint16_t w = (uint16_t)(16u + (i % 64u));
    uint16_t h = (uint16_t)(16u + ((i >> 1u) % 64u));

    dirty_region_union_rect(&c->damage_region, x, y, w, h);
    dirty_region_union(&c->frame_damage, &c->damage_region);
    dirty_region_clamp(&c->frame_damage, 0, 0, 1920, 1080);

    if (c->frame_damage.valid)
      c->dirty |= DIRTY_FRAME_ALL;

    ops += 4;
  }

  return ops;
}

static void run_one_scenario(const char* name, scenario_kind_t kind, client_hot_t* clients, size_t n, uint64_t iters) {
  init_clients(clients, n);

  uint64_t ops = 0;
  switch (kind) {
    case SCENARIO_FOCUS_CYCLE:
      ops = run_focus_cycle(clients, n, iters);
      break;
    case SCENARIO_STACKING_OPS:
      ops = run_stacking_ops(clients, n, iters);
      break;
    case SCENARIO_MOVE_RESIZE:
      ops = run_move_resize(clients, n, iters);
      break;
    case SCENARIO_FLUSH_LOOPS:
      ops = run_flush_loops(clients, n, iters);
      break;
    case SCENARIO_ALL:
    default:
      fprintf(stderr, "invalid non-concrete scenario kind\n");
      exit(2);
  }

  printf("SCENARIO %s OPS %llu\n", name, (unsigned long long)ops);
}

int main(int argc, char** argv) {
  scenario_kind_t scenario = SCENARIO_ALL;
  uint64_t iters = 100000;
  size_t clients_n = 1024;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--scenario") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      scenario = parse_scenario(argv[++i]);
    } else if (strcmp(argv[i], "--iters") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      iters = parse_u64(argv[++i], "iters");
    } else if (strcmp(argv[i], "--clients") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      uint64_t parsed = parse_u64(argv[++i], "clients");
      if (parsed == 0) {
        fprintf(stderr, "clients must be > 0\n");
        return 2;
      }
      clients_n = (size_t)parsed;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      print_usage(argv[0]);
      return 2;
    }
  }

  client_hot_t* clients = calloc(clients_n, sizeof(*clients));
  if (!clients) {
    fprintf(stderr, "failed to allocate %zu clients\n", clients_n);
    return 1;
  }

  if (scenario == SCENARIO_ALL) {
    run_one_scenario("focus_cycle", SCENARIO_FOCUS_CYCLE, clients, clients_n, iters);
    run_one_scenario("stacking_ops", SCENARIO_STACKING_OPS, clients, clients_n, iters);
    run_one_scenario("move_resize", SCENARIO_MOVE_RESIZE, clients, clients_n, iters);
    run_one_scenario("flush_loops", SCENARIO_FLUSH_LOOPS, clients, clients_n, iters);
  } else if (scenario == SCENARIO_FOCUS_CYCLE) {
    run_one_scenario("focus_cycle", scenario, clients, clients_n, iters);
  } else if (scenario == SCENARIO_STACKING_OPS) {
    run_one_scenario("stacking_ops", scenario, clients, clients_n, iters);
  } else if (scenario == SCENARIO_MOVE_RESIZE) {
    run_one_scenario("move_resize", scenario, clients, clients_n, iters);
  } else {
    run_one_scenario("flush_loops", scenario, clients, clients_n, iters);
  }

  free(clients);
  return 0;
}
