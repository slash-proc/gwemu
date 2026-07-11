# Register audit: spi

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


- Device model: `hw/misc/gnw_h7b0_spi.c`
- Register header: `include/hw/misc/gnw_h7b0_spi.h` (layout: inline)

| Register | Offset | Write | Read | Notes (line ref / snippet) |
|---|---|---|---|---|
| GNW_H7B0_SPI_CR1 | 0x0 | explicit | shadow | write @ hw/misc/gnw_h7b0_spi.c:152 `case GNW_H7B0_SPI_CR1:` |
| GNW_H7B0_SPI_SR | 0x14 | explicit | shadow | write @ hw/misc/gnw_h7b0_spi.c:191 `case GNW_H7B0_SPI_SR:` |
| GNW_H7B0_SPI_IFCR | 0x18 | explicit | shadow | write @ hw/misc/gnw_h7b0_spi.c:187 `case GNW_H7B0_SPI_IFCR:` |
| GNW_H7B0_SPI_TXDR | 0x20 | explicit | shadow | write @ hw/misc/gnw_h7b0_spi.c:164 `case GNW_H7B0_SPI_TXDR:` |
| GNW_H7B0_SPI_RXDR | 0x30 | shadow | shadow |  |
| GNW_H7B0_SPI_CFG1 | 0x8 | shadow | shadow |  |

Summary: 6 registers found. Write: 4 explicit / 2 shadow. Read: 0 explicit / 6 shadow.
