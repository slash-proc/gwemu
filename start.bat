@echo off
rem Launch gwemu (patched Zelda + retro-go dual-boot) on Windows.
rem Run from the repo root; expects the cross-built binary + DLLs in
rem build-win\dist\ and firmware images in backup\qemu-images\.
rem Hold Game+Left during the first ~2 seconds of boot for retro-go.

build-win\dist\qemu-system-arm.exe -M gnw-h7b0 ^
  -global gnw-h7b0-soc.bank1-image=backup\qemu-images\zelda-bank1-patched.bin ^
  -global gnw-h7b0-soc.bank2-image=backup\qemu-images\retro-go-bank2.bin ^
  -global gnw-h7b0-soc.extflash-image=backup\qemu-images\zelda-extflash-patched-plus-retro-go.bin ^
  -drive if=sd,file=backup\qemu-images\sdcard-overlay.qcow2 ^
  -audiodev sdl3,id=snd0 -global gnw-h7b0-sai1.audiodev=snd0 ^
  -display gwemu
