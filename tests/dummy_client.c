#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xcb/xcb.h>

int main(void) {
  xcb_connection_t* c = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(c))
    return 1;

  xcb_window_t w = xcb_generate_id(c);
  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

  uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_EXPOSURE};

  xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 10, 10, 100, 100, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

  xcb_map_window(c, w);
  xcb_flush(c);

  while (1) {
    xcb_generic_event_t* e = xcb_wait_for_event(c);
    if (!e)
      break;
    free(e);
  }

  xcb_disconnect(c);
  return 0;
}
