# Vendored third-party code, warnings-as-errors relaxed for these specific
# compilation units only (not our code to fix) -- newer GCC than xemu's own
# CI catches a couple of extra warning classes in fpng/stb_image as shipped.
libstb_image_ui = static_library('stb_image_ui',
                              sources: 'stb_image/stb_image_impl.c',
                              c_args: ['-Wno-error=missing-prototypes'])
stb_image_ui = declare_dependency(include_directories: 'stb_image',
                               link_with: libstb_image_ui)

fa = declare_dependency(include_directories: 'fa')

if cpu == 'x86_64'
  libfpng_cpp_args = ['-DFPNG_NO_SSE=0', '-msse4.1', '-mpclmul']
else
  libfpng_cpp_args = ['-DFPNG_NO_SSE=1']
endif
libfpng_cpp_args += ['-Wno-error=type-limits']

libfpng = static_library('fpng', sources: 'fpng/fpng.cpp', cpp_args: libfpng_cpp_args)
fpng = declare_dependency(include_directories: 'fpng', link_with: libfpng)
