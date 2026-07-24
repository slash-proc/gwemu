executable('gnw-make-boot-images',
           files('make_boot_images.c', 'gnw_boot_images.c'),
           dependencies: [glib],
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

# AES-128 single-block encrypt (ExtFirmware.crypt()'s OTFDEC keystream) --
# see gnw_aes128.h. gnw_symbols_{mario,zelda}.h (pre-extracted ELF symbol
# tables, header-only, no .c to build) complete the set of building blocks.
gnw_aes128_lib = static_library('gnw_aes128',
                                 files('gnw_aes128.c'),
                                 install: false)

# Firmware/Device relocation engine (gnw_cfw_engine.h) -- structural port of
# gnw-web-builder's firmware.ts/device.ts, built on the four libs above.
# Verified so far: the bit-manipulation primitives (b/bl/relative) against
# live Python on synthetic offsets, byte-exact. NOT yet verified: a full
# mario/zelda patch run end-to-end (the actual per-game patch() sequences
# from mario.py/zelda.py -- ~650 lines of magic-hex-offset call chains --
# haven't been transcribed/generated yet; see gnw_cfw_engine.h's own
# module comment and CHANGELOG for the current state of this in-progress
# port before assuming it's usable).
gnw_cfw_engine_lib = static_library('gnw_cfw_engine',
                                     files('gnw_cfw_engine.c'),
                                     link_with: [gnw_thumb_asm_lib, gnw_lz77_lib, gnw_lzma_lib, gnw_aes128_lib],
                                     dependencies: [gnw_lzma_dep],
                                     install: false)

# CFW patch build driver as a LIBRARY (gnw_cfw_build.h) -- the actual
# patch pipeline (verified byte-exact against scripts/make_cfw_images.py
# for both games, see CHANGELOG), linkable by the GUI so profile creation
# patches in-process instead of popen()ing the CLI.
gnw_cfw_build_lib = static_library('gnw_cfw_build',
                                    files('gnw_cfw_build.c', 'gnw_mario_patch.c', 'gnw_zelda_patch.c'),
                                    link_with: [gnw_cfw_engine_lib],
                                    dependencies: [gnw_lzma_dep],
                                    install: false)

# Thin CLI wrapper over gnw_cfw_build_lib, same convention as
# gnw-make-boot-images above.
executable('gnw-make-cfw-images',
           files('gnw_make_cfw_images.c'),
           link_with: [gnw_cfw_build_lib],
           dependencies: [gnw_lzma_dep],
           install: false)

# FAT32 mkfs + MBR/SD-image builder (gnw_fat32.h/gnw_sdimg.h) -- C port of
# scripts/make_sdcard_image.py's raw-image half. All disk I/O goes through
# a caller-supplied sector-write callback so the GUI can later drive it
# through QEMU's own block layer (qcow2) instead of a raw file.
gnw_sdimg_lib = static_library('gnw_sdimg',
                                files('gnw_fat32.c', 'gnw_sdimg.c'),
                                c_args: ['-D_GNU_SOURCE'],
                                install: false)

executable('gnw-make-sd-image',
           files('make_sd_image.c'),
           link_with: [gnw_sdimg_lib],
           install: false)
