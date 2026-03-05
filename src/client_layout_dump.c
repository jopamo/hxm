#include <stddef.h>
#include <stdio.h>

#include "client.h"

#define PRINT_TYPE(type_name) printf("TYPE %s SIZE %zu\n", #type_name, sizeof(type_name))
#define PRINT_OFFSET(type_name, member_name) printf("OFFSET %s.%s %zu\n", #type_name, #member_name, offsetof(type_name, member_name))

int main(void) {
  PRINT_TYPE(client_hot_t);
  PRINT_TYPE(client_cold_t);

  PRINT_TYPE(rect_t);
  PRINT_TYPE(size_hints_t);
  PRINT_TYPE(dirty_region_t);
  PRINT_TYPE(pending_state_msg_t);
  PRINT_TYPE(strut_t);

  PRINT_OFFSET(client_hot_t, xid);
  PRINT_OFFSET(client_hot_t, server);
  PRINT_OFFSET(client_hot_t, desired);
  PRINT_OFFSET(client_hot_t, pending);
  PRINT_OFFSET(client_hot_t, hints);
  PRINT_OFFSET(client_hot_t, stacking_index);
  PRINT_OFFSET(client_hot_t, dirty);
  PRINT_OFFSET(client_hot_t, state);
  PRINT_OFFSET(client_hot_t, flags);
  PRINT_OFFSET(client_hot_t, render_ctx);
  PRINT_OFFSET(client_hot_t, icon_surface);
  PRINT_OFFSET(client_hot_t, visual_id);
  PRINT_OFFSET(client_hot_t, damage_region);
  PRINT_OFFSET(client_hot_t, sync_value);
  PRINT_OFFSET(client_hot_t, fullscreen_monitors);

  PRINT_OFFSET(client_cold_t, title);
  PRINT_OFFSET(client_cold_t, protocols);
  PRINT_OFFSET(client_cold_t, strut);
  PRINT_OFFSET(client_cold_t, pid);

  return 0;
}
