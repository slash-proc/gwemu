# Register audit: ltdc

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


- Device model: `hw/display/gnw_h7b0_ltdc.c`
- Register header: `include/hw/display/gnw_h7b0_regs_ltdc.h` (layout: regs_generated)

| Register | Offset | Write | Read | Notes (line ref / snippet) |
|---|---|---|---|---|
| GNW_H7B0_LTDC_SSCR | 0x8 | shadow | shadow |  |
| GNW_H7B0_LTDC_BPCR | 0xc | shadow | shadow |  |
| GNW_H7B0_LTDC_AWCR | 0x10 | shadow | shadow |  |
| GNW_H7B0_LTDC_TWCR | 0x14 | shadow | shadow |  |
| GNW_H7B0_LTDC_GCR | 0x18 | shadow | shadow |  |
| GNW_H7B0_LTDC_SRCR | 0x24 | explicit | shadow | write @ hw/display/gnw_h7b0_ltdc.c:432 `case GNW_H7B0_LTDC_SRCR:` |
| GNW_H7B0_LTDC_BCCR | 0x2c | shadow | shadow |  |
| GNW_H7B0_LTDC_IER | 0x34 | explicit | shadow | write @ hw/display/gnw_h7b0_ltdc.c:469 `case GNW_H7B0_LTDC_IER:` |
| GNW_H7B0_LTDC_ISR | 0x38 | explicit | shadow | write @ hw/display/gnw_h7b0_ltdc.c:478 `case GNW_H7B0_LTDC_ISR:` |
| GNW_H7B0_LTDC_ICR | 0x3c | explicit | shadow | write @ hw/display/gnw_h7b0_ltdc.c:473 `case GNW_H7B0_LTDC_ICR:` |
| GNW_H7B0_LTDC_LIPCR | 0x40 | shadow | shadow |  |
| GNW_H7B0_LTDC_CPSR | 0x44 | shadow | shadow |  |
| GNW_H7B0_LTDC_CDSR | 0x48 | shadow | shadow |  |
| GNW_H7B0_LTDC_L1CR | 0x84 | shadow | shadow |  |
| GNW_H7B0_LTDC_L1WHPCR | 0x88 | shadow | shadow |  |
| GNW_H7B0_LTDC_L1WVPCR | 0x8c | shadow | shadow |  |
| GNW_H7B0_LTDC_L1CKCR | 0x90 | shadow | shadow |  |
| GNW_H7B0_LTDC_L1PFCR | 0x94 | shadow | shadow |  |
| GNW_H7B0_LTDC_L1CACR | 0x98 | shadow | shadow |  |
| GNW_H7B0_LTDC_L1DCCR | 0x9c | shadow | shadow |  |
| GNW_H7B0_LTDC_L1BFCR | 0xa0 | shadow | shadow |  |
| GNW_H7B0_LTDC_L1CFBAR | 0xac | shadow | shadow |  |
| GNW_H7B0_LTDC_L1CFBLR | 0xb0 | shadow | shadow |  |
| GNW_H7B0_LTDC_L1CFBLNR | 0xb4 | shadow | shadow |  |
| GNW_H7B0_LTDC_L1CLUTWR | 0xc4 | explicit | shadow | write @ hw/display/gnw_h7b0_ltdc.c:483 `case GNW_H7B0_LTDC_L1CLUTWR:` |
| GNW_H7B0_LTDC_L2CR | 0x104 | shadow | shadow |  |
| GNW_H7B0_LTDC_L2WHPCR | 0x108 | shadow | shadow |  |
| GNW_H7B0_LTDC_L2WVPCR | 0x10c | shadow | shadow |  |
| GNW_H7B0_LTDC_L2CKCR | 0x110 | shadow | shadow |  |
| GNW_H7B0_LTDC_L2PFCR | 0x114 | shadow | shadow |  |
| GNW_H7B0_LTDC_L2CACR | 0x118 | shadow | shadow |  |
| GNW_H7B0_LTDC_L2DCCR | 0x11c | shadow | shadow |  |
| GNW_H7B0_LTDC_L2BFCR | 0x120 | shadow | shadow |  |
| GNW_H7B0_LTDC_L2CFBAR | 0x12c | shadow | shadow |  |
| GNW_H7B0_LTDC_L2CFBLR | 0x130 | shadow | shadow |  |
| GNW_H7B0_LTDC_L2CFBLNR | 0x134 | shadow | shadow |  |
| GNW_H7B0_LTDC_L2CLUTWR | 0x144 | shadow | shadow |  |

Summary: 37 registers found. Write: 5 explicit / 32 shadow. Read: 0 explicit / 37 shadow.
