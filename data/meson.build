# gnw-h7b0 GUI (Phase 1, ported from xemu) binary asset embedding.
# NOTE: these are xemu's own bundled assets as-is (including xemu's own
# logo/icon and Xbox-controller-mask images) -- cosmetic branding cleanup
# (swap for gnw-h7b0-appropriate assets) is real follow-up work, not done
# here; this phase is about proving the build/render pipeline, not polish.
pfiles = [
  'controller_mask.png',
  'controller_mask_s.png',
  'xmu_mask.png',
  'logo_sdf.png',
  'xemu_64x64.png',
  'abxy.ttf',
  'Roboto-Medium.ttf',
  'RobotoCondensed-Regular.ttf',
  'font_awesome_6_1_1_solid.min.otf',
]

gnw_gui_pfile_targets = []

foreach e : pfiles
  t = custom_target('@0@.[ch]'.format(e),
                    output: ['@0@.c'.format(e), '@0@.h'.format(e)],
                    input: files('@0@'.format(e)),
                    command: [packfile, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'])
  gnw_gui_pfile_targets += t[0]
  genh += t[1]
endforeach

libgnw_gui_pfile = static_library('gnw_gui_pdata', gnw_gui_pfile_targets,
                          name_suffix: 'fa')
gnw_gui_ss.add(declare_dependency(link_whole: libgnw_gui_pfile))
