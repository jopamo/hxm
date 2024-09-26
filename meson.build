project('bbox', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
    'optimization=2',
    'debug=true',
    'b_ndebug=if-release',
  ]
)

# Dependencies
cc = meson.get_compiler('c')
math = cc.find_library('m', required: false)
rt = cc.find_library('rt', required: false)

xcb_dep = dependency('xcb')
xcb_icccm_dep = dependency('xcb-icccm')
xcb_xkb_dep = dependency('xcb-xkb')
xcb_randr_dep = dependency('xcb-randr')
xcb_xinerama_dep = dependency('xcb-xinerama', required: false)
xcb_keysyms_dep = dependency('xcb-keysyms', required: false)
xcb_damage_dep = dependency('xcb-damage')
xcb_sync_dep = dependency('xcb-sync')
cairo_dep = dependency('cairo')
pango_dep = dependency('pango')
pangocairo_dep = dependency('pangocairo')
fontconfig_dep = dependency('fontconfig')
rsvg_dep = dependency('librsvg-2.0')

# Optional dependencies
xkbcommon_dep = dependency('xkbcommon', required: false)

# Feature detection
have_epoll = cc.has_function('epoll_create1', prefix: '#include <sys/epoll.h>')
if not have_epoll
  error('epoll support required')
endif

# Compiler flags
add_project_arguments('-D_GNU_SOURCE', language: 'c')
add_project_arguments('-Wpedantic', language: 'c')

deps = [
  math,
  rt,
  xcb_dep,
  xcb_icccm_dep,
  xcb_xkb_dep,
  xcb_randr_dep,
  xcb_xinerama_dep,
  xcb_keysyms_dep,
  xcb_damage_dep,
  xcb_sync_dep,
  xkbcommon_dep,
  cairo_dep,
  pango_dep,
  pangocairo_dep,
  fontconfig_dep,
  rsvg_dep,
]

# Include directories
incdir = include_directories('include')

# Source files
src = files(
  'src/main.c',
  'src/core.c',
  'src/xcb_utils.c',
  'src/containers.c',
  'src/wm.c',
  'src/event.c',
  'src/log.c',
  'src/cookie_jar.c',
  'src/client.c',
  'src/wm_reply.c',
  'src/stack.c',
  'src/focus.c',
  'src/frame.c',
  'src/menu.c',
  'src/render.c',
  'src/config.c',
)

# Create executable
bbox = executable('bbox', src,
  include_directories: incdir,
  dependencies: deps,
  install: true,
)

# Tests
test_src = [
  'src/core.c',
  'src/xcb_utils.c',
  'src/containers.c',
  'src/wm.c',
  'src/event.c',
  'src/log.c',
  'src/cookie_jar.c',
  'src/client.c',
  'src/wm_reply.c',
  'src/stack.c',
  'src/focus.c',
  'src/frame.c',
  'src/menu.c',
  'src/render.c',
  'src/config.c',
]

test_workspaces = executable('test_workspaces',
  ['tests/test_workspaces.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('workspaces', test_workspaces)

test_containers = executable('test_containers',
  ['tests/test_containers.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('containers', test_containers)

test_icccm = executable('test_icccm',
  ['tests/test_icccm.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('icccm', test_icccm)

test_net_close = executable('test_net_close',
  ['tests/test_net_close.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('net_close', test_net_close)

test_ewmh_ext = executable('test_ewmh_ext',
  ['tests/test_ewmh_ext.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('ewmh_ext', test_ewmh_ext)

test_wm_class = executable('test_wm_class',
  ['tests/test_wm_class.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('wm_class', test_wm_class)

test_wm_name_fallback = executable('test_wm_name_fallback',
  ['tests/test_wm_name_fallback.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('wm_name_fallback', test_wm_name_fallback)

test_wm_icon_invalid = executable('test_wm_icon_invalid',
  ['tests/test_wm_icon_invalid.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('wm_icon_invalid', test_wm_icon_invalid)

test_workarea_compute = executable('test_workarea_compute',
  ['tests/test_workarea_compute.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('workarea_compute', test_workarea_compute)

test_unmanage_race = executable('test_unmanage_race',
  ['tests/test_unmanage_race.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('unmanage_race', test_unmanage_race)

test_transients = executable('test_transients',
  ['tests/test_transients.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('transients', test_transients)

test_size_hints = executable('test_size_hints',
  ['tests/test_size_hints.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('size_hints', test_size_hints)

test_focus = executable('test_focus',
  ['tests/test_focus.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('focus', test_focus)

test_menu = executable('test_menu',
  ['tests/test_menu.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('menu', test_menu)

test_wm_focus_policy = executable('test_wm_focus_policy',
  ['tests/test_wm_focus_policy.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('wm_focus_policy', test_wm_focus_policy)

test_rules = executable('test_rules',
  ['tests/test_rules.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('rules', test_rules)

test_resize = executable('test_resize',
  ['tests/test_resize.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('resize', test_resize)

test_wm_config_theme = executable('test_wm_config_theme',
  ['tests/test_wm_config_theme.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('wm_config_theme', test_wm_config_theme)

test_wm_icon = executable('test_wm_icon',
  ['tests/test_wm_icon.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('wm_icon', test_wm_icon)

test_fullscreen = executable('test_fullscreen',
  ['tests/test_fullscreen.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('fullscreen', test_fullscreen)

parity_client = executable('parity_client',
  'tests/parity_client.c',
  include_directories: incdir,
  dependencies: deps,
)

test_transient_cycle = executable('test_transient_cycle',
  ['tests/test_transient_cycle.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('transient_cycle', test_transient_cycle)

test_gtk_extents = executable('test_gtk_extents',
  ['tests/test_gtk_extents.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('gtk_extents', test_gtk_extents)

test_gtk_extents_update = executable('test_gtk_extents_update',
  ['tests/test_gtk_extents_update.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('gtk_extents_update', test_gtk_extents_update)

test_gtk_decorations = executable('test_gtk_decorations',
  ['tests/test_gtk_decorations.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('gtk_decorations', test_gtk_decorations)

test_stacking = executable('test_stacking',
  ['tests/test_stacking.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('stacking', test_stacking)

test_dirty_region = executable('test_dirty_region',
  ['tests/test_dirty_region.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('dirty_region', test_dirty_region)

test_cookie_jar = executable('test_cookie_jar',
  ['tests/test_cookie_jar.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('cookie_jar', test_cookie_jar)

test_ewmh_check = executable('test_ewmh_check',
  ['tests/test_ewmh_check.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('ewmh_check', test_ewmh_check)

# Install configuration files
install_data('data/bbox.desktop', install_dir: get_option('datadir') / 'xsessions')
install_data('data/bbox.conf', install_dir: get_option('sysconfdir') / 'bbox')

# Summary
summary('Build type', get_option('buildtype'))
summary('Install prefix', get_option('prefix'))
summary('Compiler', cc.get_id())
