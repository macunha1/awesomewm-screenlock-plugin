project(
  'awesomewm-screenlock-plugin',
  'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=3'],
)

xcb = dependency('xcb')
xcb_keysyms = dependency('xcb-keysyms')
x11 = dependency('x11')
pam = dependency('pam')
avcodec = dependency('libavcodec')
avdevice = dependency('libavdevice')
avfilter = dependency('libavfilter')
avformat = dependency('libavformat')
avutil = dependency('libavutil')
swscale = dependency('libswscale')

ffmpeg = [avcodec, avdevice, avfilter, avformat, avutil, swscale]

screenlock = library(
  'awesomewm_screenlock',
  'src/screenlock.c',
  'src/capture.c',
  include_directories: include_directories('include'),
  dependencies: [pam, x11, xcb, xcb_keysyms, ffmpeg],
  install: true,
)

helper = executable(
  'awesomewm-screenlock-helper',
  'src/helper.c',
  include_directories: include_directories('include'),
  dependencies: [pam, x11, xcb, xcb_keysyms, ffmpeg],
  link_with: screenlock,
  install_rpath: '$ORIGIN/../lib',
  install: true,
)

test_helper = find_program('tests/test-helper.sh')
test('helper usage', test_helper, args: [helper])

install_headers('include/awesomewm_screenlock.h')
foreach lua_version : ['5.1', '5.2']
  install_subdir(
    'lua/awesomewm_screenlock',
    install_dir: get_option('datadir') / 'lua' / lua_version,
  )
  install_subdir(
    'lua/plugins',
    install_dir: get_option('datadir') / 'lua' / lua_version,
  )
endforeach
