#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bbox.h"
#include "config.h"
#include "theme.h"

// Stub required globals

void test_theme_parser() {
    theme_t theme;
    memset(&theme, 0, sizeof(theme));

    // Create a temporary themerc file
    FILE* f = fopen("test_themerc", "w");
    fprintf(f, "border.width: 10\n");
    fprintf(f, "window.active.title.bg: raised gradient vertical\n");
    fprintf(f, "window.active.title.bg.color: #ff0000\n");
    fprintf(f, "window.active.title.bg.colorTo: #00ff00\n");
    fprintf(f, "window.active.label.text.color: #ffffff\n");
    fprintf(f, "window.title.height: 25\n");
    fprintf(f, "window.handle.height: 8\n");
    fclose(f);

    bool loaded = theme_load(&theme, "test_themerc");
    assert(loaded);
    assert(theme.border_width == 10);
    assert(theme.title_height == 25);
    assert(theme.handle_height == 8);
    assert(theme.window_active_title.flags == (BG_RAISED | BG_GRADIENT | BG_VERTICAL));
    assert(theme.window_active_title.color == 0xff0000);
    assert(theme.window_active_title.color_to == 0x00ff00);
    assert(theme.window_active_label_text_color == 0xffffff);

    remove("test_themerc");
    printf("test_theme_parser passed!\n");
}

int main() {
    test_theme_parser();
    return 0;
}
