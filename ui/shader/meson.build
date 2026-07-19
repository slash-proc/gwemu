shaders = [
  ['texture-blit', 'frag'],
  ['texture-blit', 'vert'],
  ['texture-blit-flip', 'vert'],
]

if sdl3.found()
  # gnw-h7b0 GUI (Phase 1, ported from xemu).
  shaders += [['xemu-logo', 'frag']]
endif

foreach e : shaders
  output = '@0@-@1@.h'.format(e[0], e[1])
  genh += custom_target(output,
                output: output,
                capture: true,
                input: files('@0@.@1@'.format(e[0], e[1])),
                build_by_default: false,
                command: [shaderinclude, '@INPUT0@'])
endforeach
