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
