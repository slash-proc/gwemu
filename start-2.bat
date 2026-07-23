build-win\dist\qemu-system-arm.exe -M gnw-h7b0 ^
    -device loader,file=backup\qemu-images\zelda-bank1-patched.bin,addr=0x08000000,force-raw=on ^
    -device loader,file=backup\qemu-images\retro-go-bank2.bin,addr=0x08100000,force-raw=on ^
    -device loader,file=backup\qemu-images\zelda-extflash-patched-plus-retro-go.bin,addr=0x90000000,force-raw=on ^
    -drive if=sd,file=backup\qemu-images\sdcard-overlay.qcow2 ^
    -audiodev sdl3,id=snd0 -global gnw-h7b0-sai1.audiodev=snd0 ^
    -display gwemu
