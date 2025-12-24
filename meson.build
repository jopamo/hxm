project('hxm', 'c',
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

if get_option('debug')
  add_project_arguments('-DHXM_ENABLE_DEBUG_LOGGING', language: 'c')
endif

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
  'src/ds.c',
  'src/wm.c',
  'src/wm_dirty.c',
  'src/wm_desktop.c',
  'src/wm_input_keys.c',
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
hxm = executable('hxm', src,
  include_directories: incdir,
  dependencies: deps,
  install: true,
)

if get_option('debug')
# Tests
test_src = [
  'src/core.c',
  'src/xcb_utils.c',
  'src/ds.c',
  'src/wm.c',
  'src/wm_dirty.c',
  'src/wm_desktop.c',
  'src/wm_input_keys.c',
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

test_ds = executable('test_ds',
  ['tests/test_ds.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('ds', test_ds)

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

test_icccm_props_extra = executable('test_icccm_props_extra',
  ['tests/test_icccm_props_extra.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('icccm_props_extra', test_icccm_props_extra)

test_icccm_fuzz = executable('test_icccm_fuzz',
  ['tests/test_icccm_fuzz.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('icccm_fuzz', test_icccm_fuzz)

test_wm_icon_invalid = executable('test_wm_icon_invalid',
  ['tests/test_wm_icon_invalid.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('wm_icon_invalid', test_wm_icon_invalid)

test_colormap = executable('test_colormap',
  ['tests/test_colormap.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('colormap', test_colormap)

test_save_set = executable('test_save_set',
  ['tests/test_save_set.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('save_set', test_save_set)

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

test_position_hints = executable('test_position_hints',
  ['tests/test_position_hints.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('position_hints', test_position_hints)

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

test_frame_destroy = executable('test_frame_destroy',
  ['tests/test_frame_destroy.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('frame_destroy', test_frame_destroy)

test_wm_config_theme = executable('test_wm_config_theme',
  ['tests/test_wm_config_theme.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('wm_config_theme', test_wm_config_theme)

test_config_parsing = executable('test_config_parsing',
  ['tests/test_config_parsing.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('config_parsing', test_config_parsing)

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

integration_client = executable('integration_client',
  'tests/integration_client.c',
  include_directories: incdir,
  dependencies: [xcb_dep, xcb_icccm_dep],
)

dummy_client = executable('dummy_client',
  'tests/dummy_client.c',
  include_directories: incdir,
  dependencies: [xcb_dep],
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

test_core = executable('test_core',
  ['tests/test_core.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('core', test_core)

test_slotmap_fail = executable('test_slotmap_fail',
  ['tests/test_slotmap_fail.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('slotmap_fail', test_slotmap_fail)

test_log_mono = executable('test_log_mono',
  ['tests/test_log_mono.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('log_mono', test_log_mono)

test_log_utc = executable('test_log_utc',
  ['tests/test_log_utc.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('log_utc', test_log_utc)

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

test_event_ingest = executable('test_event_ingest',
  ['tests/test_event_ingest.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('event_ingest', test_event_ingest)

test_manage_unmanage = executable('test_manage_unmanage',
  ['tests/test_manage_unmanage.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('manage_unmanage', test_manage_unmanage)

test_input_interaction = executable('test_input_interaction',
  ['tests/test_input_interaction.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('input_interaction', test_input_interaction)

test_ewmh_props = executable('test_ewmh_props',
  ['tests/test_ewmh_props.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('ewmh_props', test_ewmh_props)

test_ewmh_negative = executable('test_ewmh_negative',
  ['tests/test_ewmh_negative.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('ewmh_negative', test_ewmh_negative)

test_event_process = executable('test_event_process',
  ['tests/test_event_process.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
  link_args: [
    '-Wl,--wrap=wm_handle_key_press',
    '-Wl,--wrap=wm_handle_button_press',
    '-Wl,--wrap=wm_handle_button_release',
    '-Wl,--wrap=menu_handle_expose_region',
    '-Wl,--wrap=frame_redraw_region',
    '-Wl,--wrap=wm_handle_motion_notify',
    '-Wl,--wrap=wm_update_monitors',
    '-Wl,--wrap=wm_compute_workarea',
    '-Wl,--wrap=wm_publish_workarea',
  ]
)
test('event_process', test_event_process)

test_configure_geometry = executable('test_configure_geometry',
  ['tests/test_configure_geometry.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('configure_geometry', test_configure_geometry)

test_expose_damage = executable('test_expose_damage',
  ['tests/test_expose_damage.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('expose_damage', test_expose_damage)

test_race_conditions = executable('test_race_conditions',
  ['tests/test_race_conditions.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('race_conditions', test_race_conditions)

test_txn_serialization = executable('test_txn_serialization',
  ['tests/test_txn_serialization.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('txn_serialization', test_txn_serialization)

test_stress_lifecycle = executable('test_stress_lifecycle',
  ['tests/stress_lifecycle.c', 'tests/xcb_stubs.c'] + test_src,
  include_directories: incdir,
  dependencies: deps,
)
test('stress_lifecycle', test_stress_lifecycle)

bash = find_program('bash')
source_root = meson.project_source_root()
integration_script = join_paths(source_root, 'scripts', 'test-integration.sh')
headless_script = join_paths(source_root, 'scripts', 'test-headless.sh')
ewmh_script = join_paths(source_root, 'tests', 'ewmh', 'run_in_xvfb.sh')

script_env = environment()
script_env.set('HXM_BIN', hxm.full_path())
script_env.set('INTEGRATION_CLIENT', integration_client.full_path())
script_env.set('DUMMY_CLIENT', dummy_client.full_path())

test('integration_script', bash,
  args: [integration_script],
  env: script_env,
  workdir: source_root,
  is_parallel: false,
  timeout: 120,
  depends: [hxm, integration_client],
)

test('headless_script', bash,
  args: [headless_script],
  env: script_env,
  workdir: source_root,
  is_parallel: false,
  timeout: 120,
  depends: [hxm, dummy_client],
)

test('ewmh_xvfb', bash,
  args: [ewmh_script],
  env: script_env,
  workdir: source_root,
  is_parallel: false,
  timeout: 120,
  depends: [hxm],
)
endif

# Install configuration files
install_data('data/hxm.desktop', install_dir: get_option('datadir') / 'xsessions')
install_data('data/hxm.conf', install_dir: get_option('sysconfdir') / 'hxm')

# Summary
summary('Build type', get_option('buildtype'))
summary('Install prefix', get_option('prefix'))
summary('Compiler', cc.get_id())
