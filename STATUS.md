# Status

Last updated: 2026-07-10

## Where things stand

Project just started. Repo is a fork of upstream QEMU (`qemu/qemu`), pinned
to tag `v9.2.4`, pushed to `origin` (`slash-proc/gwemu`). No game-and-watch
code exists yet — this is pre-Phase-0.

## Current phase

**Phase 0 — Fork setup** (see `docs/roadmap.md`). Not started:
- [ ] Check out a working branch based on `v9.2.4`.
- [ ] Confirm baseline `arm-softmmu` build works unmodified.
- [ ] Add `hw/arm/gnw-h7b0.c` skeleton: bare Cortex-M7 + RAM only, no
      peripherals, registered in Kconfig/meson.build.
- [ ] Boot a trivial spin-loop ELF on it to prove the pipeline works.

## Next step

Create the working branch and get a baseline build running.

## Known constraints

- No STM32H7B0 machine exists upstream; everything here is new.
- Keep `../minicraft-gnw`'s MPS2 fault-trap QEMU harness as the working
  regression baseline until this fork's model is proven equivalent.
