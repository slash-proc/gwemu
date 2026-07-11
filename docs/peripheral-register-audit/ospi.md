# Register audit: ospi

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


- Device model: `hw/misc/gnw_h7b0_ospi.c`
- Register header: `include/hw/misc/gnw_h7b0_ospi.h` (layout: inline)

| Register | Offset | Write | Read | Notes (line ref / snippet) |
|---|---|---|---|---|
| GNW_H7B0_OSPI_SR | 0x20 | explicit | shadow | write @ hw/misc/gnw_h7b0_ospi.c:368 `case GNW_H7B0_OSPI_SR:` |
| GNW_H7B0_OSPI_FCR | 0x24 | explicit | shadow | write @ hw/misc/gnw_h7b0_ospi.c:364 `case GNW_H7B0_OSPI_FCR:` |
| GNW_H7B0_OSPI_DLR | 0x40 | shadow | shadow |  |
| GNW_H7B0_OSPI_AR | 0x48 | explicit | shadow | write @ hw/misc/gnw_h7b0_ospi.c:267 `case GNW_H7B0_OSPI_AR:` |
| GNW_H7B0_OSPI_DR | 0x50 | explicit | explicit | write @ hw/misc/gnw_h7b0_ospi.c:343 `case GNW_H7B0_OSPI_DR:`<br>read @ hw/misc/gnw_h7b0_ospi.c:218 `if (addr == GNW_H7B0_OSPI_DR) {` |
| GNW_H7B0_OSPI_CCR | 0x100 | explicit | shadow | write @ hw/misc/gnw_h7b0_ospi.c:361 `case GNW_H7B0_OSPI_CCR:` |
| GNW_H7B0_OSPI_IR | 0x110 | explicit | shadow | write @ hw/misc/gnw_h7b0_ospi.c:266 `case GNW_H7B0_OSPI_IR:` |

Summary: 7 registers found. Write: 6 explicit / 1 shadow. Read: 1 explicit / 6 shadow.
