gnw_gui_ss.add(files(
  'actions.cc',
  'animation.cc',
  'audio-view.cc',
  'display-view.cc',
  'flash-storage-view.cc',
  'font-manager.cc',
  'gl-helpers.cc',
  'input-manager.cc',
  'main-menu.cc',
  'main.cc',
  'misc.cc',
  'menubar.cc',
  'monitor.cc',
  'notifications.cc',
  'popup-menu.cc',
  'scene-components.cc',
  'scene-manager.cc',
  'scene.cc',
  'sdcard-view.cc',
  'snapshot-manager.cc',
  'titlebar.cc',
  'viewport-manager.cc',
  'welcome.cc',
  'widgets.cc',
))

# Flash/Storage tab links gnw_make_boot_images() directly (see
# flash-storage-view.cc) -- pull in its one source file rather than
# restructure contrib/gnw-tools/meson.build's existing executable-only
# target during a concurrent pass touching that directory.
gnw_gui_ss.add(files('../../contrib/gnw-tools/gnw_boot_images.c'))
