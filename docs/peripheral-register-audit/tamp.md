# Register audit: tamp

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


- Device model: `hw/misc/gnw_h7b0_tamp.c`
- Register header: `include/hw/misc/gnw_h7b0_regs_tamp.h` (layout: regs_generated)

| Register | Offset | Write | Read | Notes (line ref / snippet) |
|---|---|---|---|---|
| GNW_H7B0_TAMP_CR1 | 0x0 | shadow | shadow |  |
| GNW_H7B0_TAMP_CR2 | 0x4 | shadow | shadow |  |
| GNW_H7B0_TAMP_FLTCR | 0xc | shadow | shadow |  |
| GNW_H7B0_TAMP_ATCR1 | 0x10 | shadow | shadow |  |
| GNW_H7B0_TAMP_ATSEEDR | 0x14 | shadow | shadow |  |
| GNW_H7B0_TAMP_ATOR | 0x18 | shadow | shadow |  |
| GNW_H7B0_TAMP_IER | 0x2c | shadow | shadow |  |
| GNW_H7B0_TAMP_SR | 0x30 | shadow | shadow |  |
| GNW_H7B0_TAMP_MISR | 0x34 | shadow | shadow |  |
| GNW_H7B0_TAMP_SCR | 0x3c | shadow | shadow |  |
| GNW_H7B0_TAMP_COUNTR | 0x40 | shadow | shadow |  |
| GNW_H7B0_TAMP_CFGR | 0x50 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP0R | 0x100 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP1R | 0x104 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP2R | 0x108 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP3R | 0x10c | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP4R | 0x110 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP5R | 0x114 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP6R | 0x118 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP7R | 0x11c | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP8R | 0x120 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP9R | 0x124 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP10R | 0x128 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP11R | 0x12c | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP12R | 0x130 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP13R | 0x134 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP14R | 0x138 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP15R | 0x13c | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP16R | 0x140 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP17R | 0x144 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP18R | 0x148 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP19R | 0x14c | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP20R | 0x150 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP21R | 0x154 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP22R | 0x158 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP23R | 0x15c | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP24R | 0x160 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP25R | 0x164 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP26R | 0x168 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP27R | 0x16c | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP28R | 0x170 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP29R | 0x174 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP30R | 0x178 | shadow | shadow |  |
| GNW_H7B0_TAMP_BKP31R | 0x17c | shadow | shadow |  |

Summary: 44 registers found. Write: 0 explicit / 44 shadow. Read: 0 explicit / 44 shadow.
