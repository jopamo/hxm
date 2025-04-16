#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <X11/keysym.h>

#include "client.h"
#include "config.h"
#include "hxm.h"

// Helper to write content to a temp file
static char* write_temp_file(const char* content) {
    char* path = strdup("/tmp/hxm_test_config_XXXXXX");
    int fd = mkstemp(path);
    assert(fd != -1);
    ssize_t len = strlen(content);
    ssize_t written = write(fd, content, len);
    assert(written == len);
    close(fd);
    return path;
}

static void test_defaults(void) {
    config_t c;
    config_init_defaults(&c);

    assert(c.desktop_count == 4);
    assert(c.theme.border_width == 2);
    assert(strcmp(c.font_name, "fixed") == 0);
    assert(c.focus_raise == true);
    assert(c.fullscreen_use_workarea == false);
    assert(c.key_bindings.length > 0);
    
    // Verify specific default keybinds
    bool found_term = false;
    for (size_t i = 0; i < c.key_bindings.length; i++) {
        key_binding_t* b = c.key_bindings.items[i];
        if (b->keysym == XK_Return && b->modifiers == XCB_MOD_MASK_4 && b->action == ACTION_TERMINAL) {
            found_term = true;
        }
    }
    assert(found_term);

    config_destroy(&c);
    printf("test_defaults passed\n");
}

static void test_load_simple(void) {
    const char* content = 
        "desktop_count=6\n"
        "border_width=5\n"
        "font_name=Monospace 12\n"
        "focus_raise=false\n"
        "active_bg=#FF0000\n"
        "desktop_names=Web,Code,Music\n";

    char* path = write_temp_file(content);
    
    config_t c;
    config_init_defaults(&c);
    bool res = config_load(&c, path);
    assert(res);

    assert(c.desktop_count == 6);
    assert(c.theme.border_width == 5);
    assert(strcmp(c.font_name, "Monospace 12") == 0);
    assert(!c.focus_raise);
    assert(c.theme.window_active_title.color == 0xFF0000);
    
    assert(c.desktop_names_count == 3);
    assert(strcmp(c.desktop_names[0], "Web") == 0);
    assert(strcmp(c.desktop_names[1], "Code") == 0);
    assert(strcmp(c.desktop_names[2], "Music") == 0);

    config_destroy(&c);
    unlink(path);
    free(path);
    printf("test_load_simple passed\n");
}

static void test_keybinds(void) {
    const char* content = 
        "clear_keybinds=\n" // Should clear defaults
        "keybind=Mod4+Shift+q : close\n"
        "keybind=Control+Alt+t : exec terminal\n"
        "keybind=Mod1+Tab:focus_next\n"; // Minimal spaces

    char* path = write_temp_file(content);
    
    config_t c;
    config_init_defaults(&c);
    bool res = config_load(&c, path);
    assert(res);

    assert(c.key_bindings.length == 3);

    // Check 1: Mod4+Shift+q -> close
    key_binding_t* b1 = c.key_bindings.items[0];
    assert((b1->modifiers & XCB_MOD_MASK_4));
    assert((b1->modifiers & XCB_MOD_MASK_SHIFT));
    assert(b1->keysym == XK_q);
    assert(b1->action == ACTION_CLOSE);

    // Check 2: Control+Alt+t -> exec terminal
    key_binding_t* b2 = c.key_bindings.items[1];
    assert((b2->modifiers & XCB_MOD_MASK_CONTROL));
    assert((b2->modifiers & XCB_MOD_MASK_1)); // Alt is usually Mod1
    assert(b2->keysym == XK_t);
    assert(b2->action == ACTION_EXEC);
    assert(strcmp(b2->exec_cmd, "terminal") == 0);

    config_destroy(&c);
    unlink(path);
    free(path);
    printf("test_keybinds passed\n");
}

static void test_rules(void) {
    const char* content = 
        "rule=class:Firefox -> desktop:1\n"
        "rule=title:Error, type:dialog -> layer:above, focus:yes\n"
        "rule=instance:term -> placement:center\n";

    char* path = write_temp_file(content);
    
    config_t c;
    config_init_defaults(&c);
    bool res = config_load(&c, path);
    assert(res);

    assert(c.rules.length == 3);

    // Rule 1
    app_rule_t* r1 = c.rules.items[0];
    assert(strcmp(r1->class_match, "Firefox") == 0);
    assert(r1->desktop == 1);

    // Rule 2
    app_rule_t* r2 = c.rules.items[1];
    assert(strcmp(r2->title_match, "Error") == 0);
    assert(r2->type_match == WINDOW_TYPE_DIALOG);
    assert(r2->layer == LAYER_ABOVE);
    assert(r2->focus == 1);

    // Rule 3
    app_rule_t* r3 = c.rules.items[2];
    assert(strcmp(r3->instance_match, "term") == 0);
    assert(r3->placement == PLACEMENT_CENTER);

    config_destroy(&c);
    unlink(path);
    free(path);
    printf("test_rules passed\n");
}

static void test_theme(void) {
    const char* content = 
        "window.active.title.bg: gradient vertical\n"
        "window.active.title.bg.color: #00FF00\n"
        "window.active.title.bg.colorTo: #0000FF\n"
        "border.width: 10\n";

    char* path = write_temp_file(content);
    
    theme_t t;
    memset(&t, 0, sizeof(t));
    bool res = theme_load(&t, path);
    assert(res);

    assert(t.border_width == 10);
    assert(t.window_active_title.color == 0x00FF00);
    assert(t.window_active_title.color_to == 0x0000FF);
    assert(t.window_active_title.flags & BG_GRADIENT);
    assert(t.window_active_title.flags & BG_VERTICAL);

    unlink(path);
    free(path);
    printf("test_theme passed\n");
}

static void test_invalid(void) {
    const char* content = 
        "invalid_key=123\n"
        "broken_line_no_eq\n"
        "keybind=BadMod+z : close\n" // BadMod should be ignored? Or logged?
        "keybind=Mod4+BadKey : close\n" // BadKey -> NoSymbol
        "rule=class:Foo -> bad_action\n"; // Bad action part

    char* path = write_temp_file(content);
    
    config_t c;
    config_init_defaults(&c);
    // Should not crash, just warn
    bool res = config_load(&c, path);
    assert(res); // Still returns true if file exists

    config_destroy(&c);
    unlink(path);
    free(path);
    printf("test_invalid passed\n");
}

static void test_missing_file(void) {
    config_t c;
    config_init_defaults(&c);
    bool res = config_load(&c, "/non/existent/path/config");
    assert(!res);
    config_destroy(&c);
    printf("test_missing_file passed\n");
}

int main(void) {
    test_defaults();
    test_load_simple();
    test_keybinds();
    test_rules();
    test_theme();
    test_invalid();
    test_missing_file();
    return 0;
}
