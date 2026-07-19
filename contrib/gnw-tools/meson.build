executable('gnw-make-boot-images',
           files('make_boot_images.c', 'gnw_boot_images.c'),
           install: false)

# Thumb-2 assembler for the (in-progress) C port of the gnwmanager CFW patch
# pipeline -- see gnw_thumb_asm.h. Built as its own static lib so it can be
# linked into both a future gnw-tools CLI target and, eventually, the GUI
# (ui/xui/) without duplicating compilation.
gnw_thumb_asm_lib = static_library('gnw_thumb_asm',
                                    files('gnw_thumb_asm.c'),
                                    install: false)

# lz77 decompressor (rwdata-init tables) and LZMA1 compressor (links system
# liblzma -- see gnw_lzma.c's own comment on that choice) for the same
# in-progress CFW patch pipeline port.
gnw_lz77_lib = static_library('gnw_lz77',
                               files('gnw_lz77.c'),
                               install: false)

gnw_lzma_dep = dependency('liblzma', required: true)
gnw_lzma_lib = static_library('gnw_lzma',
                               files('gnw_lzma.c'),
                               dependencies: [gnw_lzma_dep],
                               install: false)
