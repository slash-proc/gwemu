# Register audit: dbgmcu

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


- Device model: `hw/misc/gnw_h7b0_dbgmcu.c`
- Register header: `include/hw/misc/gnw_h7b0_regs_dbgmcu.h` (layout: regs_generated)

| Register | Offset | Write | Read | Notes (line ref / snippet) |
|---|---|---|---|---|
| GNW_H7B0_DBGMCU_IDC | 0x0 | shadow | shadow |  |
| GNW_H7B0_DBGMCU_CR | 0x4 | shadow | shadow |  |
| GNW_H7B0_DBGMCU_APB3FZ1 | 0x34 | shadow | shadow |  |
| GNW_H7B0_DBGMCU_APB1LFZ1 | 0x3c | shadow | shadow |  |
| GNW_H7B0_DBGMCU_APB2FZ1 | 0x4c | shadow | shadow |  |
| GNW_H7B0_DBGMCU_APB4FZ1 | 0x54 | shadow | shadow |  |

Summary: 6 registers found. Write: 0 explicit / 6 shadow. Read: 0 explicit / 6 shadow.
