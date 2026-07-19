# Register audit: jpeg

> **What this table means (read before acting on it):**
>
> This table was generated mechanically by `scripts/audit_peripheral_regs.py`,
> by parsing this peripheral's register-offset header and the `case`/`if`
> labels in its `gnw_h7b0_*_read()`/`gnw_h7b0_*_write()` functions in the
> device model `.c` file.
>
> - **"explicit"** means this device model has *some* special-cased
>   behavior for that register on that access direction. It does **not**
>   mean the behavior is correct or complete -- read the referenced line(s)
>   yourself.
> - **"shadow"** means the register falls through to the default plain
>   array read/write (or, for read, is never referenced elsewhere and thus
>   just returns the backing array value). It does **NOT** mean the
>   register is safe to ignore. Real firmware may busy-wait on a status
>   bit in a "shadow" register that this model never sets, exactly as
>   found this session in LTDC and SPI/OSPI (confirmed real bugs a purely
>   mechanical pass like this one cannot catch).
>
> This script cannot tell you whether firmware depends on a shadow
> register's real hardware effect. That still requires a manual or
> research-agent pass cross-referencing real firmware source (e.g. a
> sibling checkout at `~/Nerd/git/game-and-watch-retro-go-sd`) against
> this list, following the same method used for this session's LTDC and
> SPI/OSPI audits. Treat every "shadow" row here as an open question, not
> a clean bill of health.


- Device model: `hw/misc/gnw_h7b0_jpeg.c`
- Register header: `include/hw/misc/gnw_h7b0_regs_jpeg.h` (layout: regs_generated)

| Register | Offset | Write | Read | Notes (line ref / snippet) |
|---|---|---|---|---|
| GNW_H7B0_JPEG_CONFR0 | 0x0 | explicit | shadow | write @ hw/misc/gnw_h7b0_jpeg.c:248 `if (addr == GNW_H7B0_JPEG_CONFR0_OFFSET && (val64 & JPEG_CONFR0_START)) {` |
| GNW_H7B0_JPEG_CONFR1 | 0x4 | shadow | shadow |  |
| GNW_H7B0_JPEG_CONFR2 | 0x8 | shadow | shadow |  |
| GNW_H7B0_JPEG_CONFR3 | 0xc | shadow | shadow |  |
| GNW_H7B0_JPEG_CONFRN1 | 0x10 | shadow | shadow |  |
| GNW_H7B0_JPEG_CONFRN2 | 0x14 | shadow | shadow |  |
| GNW_H7B0_JPEG_CONFRN3 | 0x18 | shadow | shadow |  |
| GNW_H7B0_JPEG_CONFRN4 | 0x1c | shadow | shadow |  |
| GNW_H7B0_JPEG_CR | 0x30 | explicit | shadow | write @ hw/misc/gnw_h7b0_jpeg.c:254 `} else if (addr == GNW_H7B0_JPEG_CR_OFFSET) {` |
| GNW_H7B0_JPEG_SR | 0x34 | shadow | shadow |  |
| GNW_H7B0_JPEG_CFR | 0x38 | explicit | shadow | write @ hw/misc/gnw_h7b0_jpeg.c:261 `} else if (addr == GNW_H7B0_JPEG_CFR_OFFSET) {` |
| GNW_H7B0_JPEG_DIR | 0x40 | explicit | shadow | write @ hw/misc/gnw_h7b0_jpeg.c:268 `} else if (addr == GNW_H7B0_JPEG_DIR_OFFSET) {` |
| GNW_H7B0_JPEG_DOR | 0x44 | shadow | explicit | read @ hw/misc/gnw_h7b0_jpeg.c:139 `if (addr == GNW_H7B0_JPEG_DOR_OFFSET) {` |

Summary: 13 registers found. Write: 4 explicit / 9 shadow. Read: 1 explicit / 12 shadow.
