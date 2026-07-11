# Register audit: rcc

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


- Device model: `hw/misc/gnw_h7b0_rcc.c`
- Register header: `include/hw/misc/gnw_h7b0_regs_rcc.h` (layout: regs_generated)

| Register | Offset | Write | Read | Notes (line ref / snippet) |
|---|---|---|---|---|
| GNW_H7B0_RCC_CR | 0x0 | explicit | shadow | write @ hw/misc/gnw_h7b0_rcc.c:76 `case GNW_H7B0_RCC_CR:` |
| GNW_H7B0_RCC_HSICFGR | 0x4 | shadow | shadow |  |
| GNW_H7B0_RCC_CRRCR | 0x8 | shadow | shadow |  |
| GNW_H7B0_RCC_CSICFGR | 0xc | shadow | shadow |  |
| GNW_H7B0_RCC_CFGR | 0x10 | explicit | shadow | write @ hw/misc/gnw_h7b0_rcc.c:110 `case GNW_H7B0_RCC_CFGR:` |
| GNW_H7B0_RCC_CDCFGR1 | 0x18 | shadow | shadow |  |
| GNW_H7B0_RCC_CDCFGR2 | 0x1c | shadow | shadow |  |
| GNW_H7B0_RCC_SRDCFGR | 0x20 | shadow | shadow |  |
| GNW_H7B0_RCC_PLLCKSELR | 0x28 | shadow | shadow |  |
| GNW_H7B0_RCC_PLLCFGR | 0x2c | shadow | shadow |  |
| GNW_H7B0_RCC_PLL1DIVR | 0x30 | shadow | shadow |  |
| GNW_H7B0_RCC_PLL1FRACR | 0x34 | shadow | shadow |  |
| GNW_H7B0_RCC_PLL2DIVR | 0x38 | shadow | shadow |  |
| GNW_H7B0_RCC_PLL2FRACR | 0x3c | shadow | shadow |  |
| GNW_H7B0_RCC_PLL3DIVR | 0x40 | shadow | shadow |  |
| GNW_H7B0_RCC_PLL3FRACR | 0x44 | shadow | shadow |  |
| GNW_H7B0_RCC_CDCCIPR | 0x4c | shadow | shadow |  |
| GNW_H7B0_RCC_CDCCIP1R | 0x50 | shadow | shadow |  |
| GNW_H7B0_RCC_CDCCIP2R | 0x54 | shadow | shadow |  |
| GNW_H7B0_RCC_SRDCCIPR | 0x58 | shadow | shadow |  |
| GNW_H7B0_RCC_CIER | 0x60 | shadow | shadow |  |
| GNW_H7B0_RCC_CIFR | 0x64 | shadow | shadow |  |
| GNW_H7B0_RCC_CICR | 0x68 | shadow | shadow |  |
| GNW_H7B0_RCC_BDCR | 0x70 | explicit | shadow | write @ hw/misc/gnw_h7b0_rcc.c:126 `case GNW_H7B0_RCC_BDCR:` |
| GNW_H7B0_RCC_CSR | 0x74 | explicit | shadow | write @ hw/misc/gnw_h7b0_rcc.c:117 `case GNW_H7B0_RCC_CSR:` |
| GNW_H7B0_RCC_AHB3RSTR | 0x7c | shadow | shadow |  |
| GNW_H7B0_RCC_AHB1RSTR | 0x80 | shadow | shadow |  |
| GNW_H7B0_RCC_AHB2RSTR | 0x84 | shadow | shadow |  |
| GNW_H7B0_RCC_AHB4RSTR | 0x88 | shadow | shadow |  |
| GNW_H7B0_RCC_APB3RSTR | 0x8c | shadow | shadow |  |
| GNW_H7B0_RCC_APB1LRSTR | 0x90 | shadow | shadow |  |
| GNW_H7B0_RCC_APB1HRSTR | 0x94 | shadow | shadow |  |
| GNW_H7B0_RCC_APB2RSTR | 0x98 | shadow | shadow |  |
| GNW_H7B0_RCC_APB4RSTR | 0x9c | shadow | shadow |  |
| GNW_H7B0_RCC_SRDAMR | 0xa8 | shadow | shadow |  |
| GNW_H7B0_RCC_CKGAENR | 0xb0 | shadow | shadow |  |
| GNW_H7B0_RCC_RSR | 0x130 | explicit | shadow | write @ hw/misc/gnw_h7b0_rcc.c:140 `case GNW_H7B0_RCC_RSR:` |
| GNW_H7B0_RCC_AHB3ENR | 0x134 | shadow | shadow |  |
| GNW_H7B0_RCC_AHB1ENR | 0x138 | shadow | shadow |  |
| GNW_H7B0_RCC_AHB2ENR | 0x13c | shadow | shadow |  |
| GNW_H7B0_RCC_AHB4ENR | 0x140 | shadow | shadow |  |
| GNW_H7B0_RCC_APB3ENR | 0x144 | shadow | shadow |  |
| GNW_H7B0_RCC_APB1LENR | 0x148 | shadow | shadow |  |
| GNW_H7B0_RCC_APB1HENR | 0x14c | shadow | shadow |  |
| GNW_H7B0_RCC_APB2ENR | 0x150 | shadow | shadow |  |
| GNW_H7B0_RCC_APB4ENR | 0x154 | shadow | shadow |  |
| GNW_H7B0_RCC_AHB3LPENR | 0x15c | shadow | shadow |  |
| GNW_H7B0_RCC_AHB1LPENR | 0x160 | shadow | shadow |  |
| GNW_H7B0_RCC_AHB2LPENR | 0x164 | shadow | shadow |  |
| GNW_H7B0_RCC_AHB4LPENR | 0x168 | shadow | shadow |  |
| GNW_H7B0_RCC_APB3LPENR | 0x16c | shadow | shadow |  |
| GNW_H7B0_RCC_APB1LLPENR | 0x170 | shadow | shadow |  |
| GNW_H7B0_RCC_APB1HLPENR | 0x174 | shadow | shadow |  |
| GNW_H7B0_RCC_APB2LPENR | 0x178 | shadow | shadow |  |
| GNW_H7B0_RCC_APB4LPENR | 0x17c | shadow | shadow |  |

Summary: 55 registers found. Write: 5 explicit / 50 shadow. Read: 0 explicit / 55 shadow.
