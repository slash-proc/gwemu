# STM32H7B0 internal flash: RM0455 is wrong

## What RM0455 (rev 13) says

`rm0455.pdf` (repo root) is explicit and repeated in multiple places:

- Table 6 (memory map): `0x0800 0000 - 0x080F FFFF` listed as "Flash memory
  bank 1", `0x0810 0000 - 0x081F FFFF` as "Flash memory bank 2" — but a
  footnote on that same page says "Bank 1 is limited to 128 Kbytes on
  STM32H7B0 devices."
- Section 4 (embedded flash): "For STM32H7B0 devices: a 128-Kbyte main
  memory block, organized in a single bank" — bank 2 explicitly does not
  exist on H7B0 ("bank 2 is not available... bank 1 contains 16 sectors of
  8 Kbytes each").
- Table 14 ("Flash memory organization, STM32H7B0 devices"): only 16
  sectors × 8K = 128K shown, `0x08000000`–`0x0801FFFF`.
- Multiple notes elsewhere: "flash bank swapping is not available on
  STM32H7B0 devices."

Taken at face value, RM0455 says real H7B0 hardware has 128K of internal
flash, single bank, full stop.

## What's actually true on real hardware

Per the project owner (2026-07-10), citing the Game & Watch modding
community's own hardware investigation: **real STM32H7B0 silicon actually
has 2×256K dual-bank internal flash** — bank 1 at `0x08000000`, bank 2 at
`0x08100000` — contradicting RM0455 outright. This is described as a
genuinely undocumented aspect of the real chip (not a datasheet typo the
community merely noticed, but real silicon behavior nobody at ST wrote
down anywhere official), discovered through direct hardware work, not
something derivable from any ST document.

## Why this matters here

Phase 1 memory-map work (see `docs/roadmap.md`) needs the real flash
layout to eventually boot unmodified retro-go/GWHB firmware. **Do not
"fix" the flash model back to RM0455's 128K/single-bank numbers** — that
would be reintroducing an error, not correcting one. If a future session
(including a future instance of Claude with no memory of this
conversation) reads RM0455 and notices the SVD's `Flash` peripheral or
this repo's code disagreeing with the PDF, this file is why: the PDF is
the one that's wrong here, not the code.

If this ever needs re-verifying: check community sources (G&W modding
Discord/wiki/forums, not ST documentation) rather than re-deriving from
RM0455, since RM0455 is the thing already shown to be incorrect on this
specific point.
