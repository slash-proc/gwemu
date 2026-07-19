# 2026-07-13 — gnw-web-builder integration debugging session

Started as "why can't the web builder talk to the RAM util stub" and grew
into a full pass over every layer between a browser tab and QEMU: the
Docker bridge, the GDB Remote Serial Protocol itself, three separate
real device-model bugs, and a persistence model change. Kept here as one
narrative since each fix only made sense once the previous one was ruled
out.

## Tooling built this session: `scripts/gdb_tap.py`

A transparent TCP proxy that sits between whatever's driving QEMU over
GDB RSP (browser bridge, `gnwmanager --qemu`, ad-hoc scripts) and QEMU's
real `-gdb tcp::<port>`, logging every byte both directions. Became the
single most useful tool this session — nearly every bug below was found
by reading its log, not by guessing.

```
./build/qemu-system-arm ... -gdb tcp::12340   # QEMU on an internal port
python3 scripts/gdb_tap.py --listen-host 0.0.0.0 --listen-port 1234 \
    --target-port 12340                        # tap on the port clients expect
```

Notable things it does, and why:
- **`0.0.0.0` bind, not `127.0.0.1`**: the backend runs inside a Docker
  container; `host.docker.internal` resolves to the container's
  `172.17.0.1` bridge gateway, not the host's loopback. A tap bound only
  to loopback is unreachable from the container, and separately,
  host-native tools (`gnwmanager --qemu`, which resolves `localhost`) need
  loopback too. `0.0.0.0` covers both without running two tap processes.
- **`TCP_NODELAY` + re-armed `TCP_QUICKACK`** on both legs: see the Nagle
  section below — this is the same fix as `backend/src/server.ts`'s, just
  needed here too since `gnwmanager`'s Python client goes through this tap
  directly, not through the Node backend.
- **Connection preemption**: QEMU's gdbstub only serves one attached
  client at a time. A long-lived browser tab's background liveness poll
  would otherwise permanently occupy that slot and starve/hang any
  one-shot tool (`gnwmanager --qemu info`) that connects later. The tap
  now closes whichever session is currently active the moment a new one
  connects, so the newest caller always wins immediately instead of
  hanging or erroring.

## Bug 1 — Docker bridge unreachable (`host.docker.internal` ≠ `127.0.0.1`)

First symptom: `"QEMU bridge connection closed"` with zero bytes ever
reaching the tap. Root cause: `backend/src/server.ts` connects to
`host.docker.internal:1234` from inside its container; that resolves to
`172.17.0.1` on the host, which nothing was listening on. Not a qemu-gnw
bug, but worth remembering here since it's the first thing to check for
any "web builder can't reach QEMU" report — see the tap's bind-address
note above.

## Bug 2 — mailbox status polling starved the guest CPU (real hang, not slowness)

Symptom: `startStub()` timed out waiting for `IDLE` with status stuck at
`BOOTING` — the stub never got past bringing itself up.

`gnw-web-builder`'s `qemuTransport.ts` (`GdbRemoteClient`) halts the VM
before every memory op and only auto-resumes after a 75ms idle debounce
(needed because — at the time — QEMU's gdbstub genuinely couldn't service
`m`/`M` while running; see Bug 4). `gnw-flasher`'s `waitForIdle()`/
`getContext()`/`waitForContextComplete()` polled every **10ms** — faster
than that 75ms debounce. Every poll cancelled the pending auto-resume
before it ever gave the guest CPU more than a few ms of real run time per
cycle (measured: ~4ms of execution per ~44ms wall-clock cycle). Fixed by
raising the poll interval to 150ms (`STATUS_POLL_INTERVAL_MS` in
`gnw-flasher/src/index.ts`) — comfortably above the debounce window.

## Bug 3 — Nagle's algorithm + delayed ACK (the ~40ms-per-request tax)

Even after Bug 2, every single GDB RSP round trip (a few bytes each way)
was locked to a suspiciously exact ~40-42ms cadence — Linux's classic
Nagle (sender) + delayed-ACK (receiver) interaction. Fixed on every
socket in the chain that this project can modify:
- `backend/src/server.ts`: `sock.setNoDelay(true)` on the QEMU bridge
  socket (~33x speedup for the web UI).
- `gnwmanager/ocdbackend/gdb_backend.py`: same fix, since gnwmanager
  connects directly, not through the Node backend (~12x speedup, `info`
  went from ~39s to ~3s).
- `scripts/gdb_tap.py`: `TCP_NODELAY` plus re-armed `TCP_QUICKACK` (needed
  because NODELAY alone only fixes stalls *this process's* sends would
  cause — when the *other* end has Nagle enabled without NODELAY, as
  gnwmanager's raw socket did before its own fix, the stall instead comes
  from this side's delayed ACK).

## Bug 4 — QEMU's gdbstub halted the VM for every memory read/write

Not just slow — genuinely disruptive: every `m`/`M` packet halted the
whole VM (`gdbstub/gdbstub.c`'s `gdb_read_byte()` called `vm_stop()` on
*any* byte arriving while running, not just Ctrl-C), then relied on the
caller to explicitly resume. Real hardware's SWD debug access doesn't
need to halt the CPU for memory access at all — this was purely an
upstream protocol default, and `handle_read_mem`/`handle_write_mem` use
`cpu_memory_rw_debug()`, which is exactly as safe to call from a running
VM as a halted one (the same accessor QEMU's monitor uses live).

Fixed at the source: the per-byte halt-while-running gate now only fires
for the literal Ctrl-C interrupt gesture; the actual halt decision is
deferred to right before `gdb_handle_packet()` dispatches, and skipped
entirely for `m`/`M`. Every other command (registers, continue/step,
breakpoints, `qXfer`, etc.) still halts first, unchanged. Paired with a
matching change in `gnw-web-builder/qemuTransport.ts`: `readMemory`/
`writeMemory` no longer pre-emptively halt via `withHalted()` — only
register access still does.

## Bug 5 — HASH peripheral was a stub with no side effects (real, permanent hang)

Symptom: any internal-flash write hung solid — not slow, genuinely stuck
— with the `[Stopped]`/running UI flicker still ticking (that was just
the unrelated liveness poll).

`hw/arm/gnw_h7b0_soc.c` had `create_unimplemented_device("HASH", ...)` —
QEMU's generic "not modeled" stub, which always reads 0. gnwmanager's RAM
stub calls `HAL_HASHEx_SHA256_Start(..., HAL_MAX_DELAY)` after every
flash write to verify it — `HAL_MAX_DELAY` means **no timeout at all**,
so the driver polled the digest-complete flag forever, and it never set.

Fixed with a real (if synchronous/instant) MD5/SHA-1/SHA-224/SHA-256
device model: `hw/misc/gnw_h7b0_hash.c`, backed by QEMU's own
`crypto/hash.h` (`qcrypto_hash_bytes`) rather than a hand-rolled
implementation. Models exactly the one code path gnwmanager actually
exercises — accumulate `DIN` writes, finalize on `STR.DCAL`, using
`STR.NBLW` to trim the final word's valid byte count — not DMA, HMAC, or
suspend/resume, since nothing here uses those.

## Bug 6 — internal-flash erase was a complete no-op (real, silent bug)

Symptom: `gnwmanager erase bank1/bank2` finished suspiciously
*instantly* and a rescan showed the content completely unchanged — not a
timing issue, an actual no-op.

`hw/misc/gnw_h7b0_flash_r.c` (`FLASH_R`, the embedded flash controller's
own registers) was a pure auto-generated register stub with **zero
connection to the actual flash memory**. Writing `FLASH_CR1`/`CR2`'s
sector-erase-trigger bits (`SER`+`SNB`+`START`, what
`HAL_FLASHEx_Erase()` actually does) only ever changed the register's own
stored value; the guest-visible flash bytes were never touched. This is
why programming worked fine all along but erasing didn't: `HAL_FLASH_
Program()` writes flash content via a direct CPU memory store, bypassing
these control registers for the data movement entirely — only erase
depends on the controller having a real side effect.

Fixed by wiring `gnw_h7b0_flash_r.c` up to the actual `flash_bank1`/
`flash_bank2` `MemoryRegion`s (`gnw_h7b0_flash_r_set_banks()`, called from
`gnw_h7b0_soc.c`'s `realize()`): a `CR1`/`CR2` write with `SER`+`START`
(or `BER`+`START` for a full-bank erase) now `memset`s the real 8KB
sector (or whole 256KB bank — see `docs/h7b0-flash-discrepancy.md` for
why 8KB, not the SVD's stated sector count) to `0xFF` in the real backing
memory, synchronously, then clears `START`/`SER`/`BER`/`SNB` — matching
real hardware's "hardware resets START when the operation has been
acknowledged" semantics.

Verified with a direct repro (bypassing the browser/gnwmanager entirely):
loaded the RAM stub, submitted erase requests for 3 sequential 256KB
regions alternating context 0/1 exactly like the real client does, and
confirmed via direct memory read that all 3 actually became `0xFF`.
Extflash erase was separately confirmed already working (a different
code path — `hw/misc/gnw_h7b0_ospi.c`'s OSPI erase commands, which
already had a real backing-store side effect from earlier work).

## Feature: optional persistent flash images (now the default)

Added because a user erasing/flashing through the web UI expected the
change to show up in the actual `backup/qemu-images/*.bin` files on
disk, the way flashing a real device changes the device — and found it
didn't, since `flash_bank1`/`flash_bank2`/`extflash` were always
anonymous RAM seeded once at boot via `-device loader`, with writes never
touching the source file.

`gnw_h7b0_soc.c` gained three optional string properties —
`bank1-image`, `bank2-image`, `extflash-image` — settable via `-global
gnw-h7b0-soc.<name>=<path>`. When set, that region is backed directly by
the named file via `memory_region_init_ram_from_file(..., RAM_SHARED,
...)` (the same mmap-backed-RAM mechanism QEMU's own pflash devices use),
so guest writes land in the file live, no explicit flush/dump step
needed. Verified end-to-end: wrote a distinctive byte pattern via gdb,
killed QEMU uncleanly, confirmed the pattern was in the file afterward.

This is a real machine-model change, distinct from the earlier
`scripts/boot_qemu.sh` convenience-flag change that exposes it as the
default launch behavior — see that script's own comments for the
`--ephemeral` opt-out.

## Tooling/workflow notes for next time

- **Preferred live-debugging pattern this session**: launch QEMU with
  `-gdb tcp::<port> -S` on an isolated instance (a scratch copy of the
  images, a port nobody else uses), drive the RAM stub directly over raw
  GDB RSP to reproduce a bug in isolation, *then* apply the same fix to
  the shared/real instance. Much faster than debugging through the full
  browser/backend/tap chain, and doesn't risk the user's live session.
- **Never steal a live GDB connection to "just take a quick look."**
  QEMU's gdbstub allows only one client; connecting a second one either
  gets silently ignored or disconnects the first, corrupting whatever
  operation was in flight. If you need to inspect state while something
  real is running, read guest memory passively through the *existing*
  connection's tap log, or ask the user to pause first.
- **A single stale snapshot of a polling value can look like a hang when
  it isn't.** The "stuck" context-ready register read that kicked off the
  Bug-6 investigation turned out, over a longer window, to be cycling
  cleanly through 18 completed chunks — always check a value's trend
  over multiple samples before concluding "unchanged."
- **`create_unimplemented_device()` isn't just a fallback for truly
  unused peripherals** — it silently converts "hang forever waiting on a
  flag" bugs into "look completely done immediately" bugs, and either can
  masquerade as the other depending on what the guest driver's timeout
  behavior is. Bug 5 (`HAL_MAX_DELAY`, hangs forever) and Bug 6 (blind
  register write, looks instantly done) are opposite failure modes from
  the exact same root cause (missing real device behavior) — check both
  when a peripheral is suspiciously silent.
