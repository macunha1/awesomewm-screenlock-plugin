project(
  'awesomewm-screenlock-plugin',
  'c',
  version: '0.1.0',
  meson_version: '>=1.6.0',
  default_options: ['c_std=c11', 'warning_level=3'],
)

lua_version = get_option('lua-version')
if lua_version not in ['5.1', '5.2']
  error('lua-version must be either 5.1 or 5.2')
endif

xcb = dependency('xcb')
xcb_keysyms = dependency('xcb-keysyms')
xcb_xinerama = dependency('xcb-xinerama')
x11 = dependency('x11')
xft = dependency('xft')
pam = dependency('pam')
avcodec = dependency('libavcodec')
avdevice = dependency('libavdevice')
avfilter = dependency('libavfilter')
avformat = dependency('libavformat')
avutil = dependency('libavutil')
swscale = dependency('libswscale')
threads = dependency('threads')

ffmpeg = [avcodec, avdevice, avfilter, avformat, avutil, swscale]

screenlock = library(
  'awesomewm_screenlock',
  'src/screenlock.c',
  'src/capture.c',
  'src/control.c',
  include_directories: include_directories('include'),
  dependencies: [pam, x11, xft, xcb, xcb_keysyms, xcb_xinerama, threads, ffmpeg],
  install: true,
)

helper = executable(
  'awesomewm-screenlock-helper',
  'src/helper.c',
  include_directories: include_directories('include'),
  dependencies: [pam, x11, xft, xcb, xcb_keysyms, xcb_xinerama, threads, ffmpeg],
  link_with: screenlock,
  install_rpath: '$ORIGIN/../lib',
  install: true,
)

test_helper = find_program('tests/test-helper.sh')
test('helper usage', test_helper, args: [helper])

lua = find_program('luajit', 'lua', required: true)
test_lua = find_program('tests/test-lua.sh')
test('Lua module', test_lua, args: [lua, meson.project_source_root()])

install_headers('include/awesomewm_screenlock.h')
install_subdir(
  'lua/awesomewm_screenlock',
  install_dir: get_option('datadir') / 'lua' / lua_version,
)
install_subdir(
  'lua/plugins',
  install_dir: get_option('datadir') / 'lua' / lua_version,
)
