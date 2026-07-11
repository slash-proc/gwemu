# Register audit: tim2

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


- Device model: `hw/misc/gnw_h7b0_tim2.c`
- Register header: `include/hw/misc/gnw_h7b0_regs_tim2.h` (layout: regs_generated)

| Register | Offset | Write | Read | Notes (line ref / snippet) |
|---|---|---|---|---|
| GNW_H7B0_TIM2_CR1 | 0x0 | explicit | shadow | write @ hw/misc/gnw_h7b0_tim2.c:150 `if ((addr & 0x3ffu) == GNW_H7B0_TIM2_CR1_OFFSET) {` |
| GNW_H7B0_TIM2_CR2 | 0x4 | shadow | shadow |  |
| GNW_H7B0_TIM2_SMCR | 0x8 | shadow | shadow |  |
| GNW_H7B0_TIM2_DIER | 0xc | shadow | shadow |  |
| GNW_H7B0_TIM2_SR | 0x10 | shadow | shadow |  |
| GNW_H7B0_TIM2_EGR | 0x14 | explicit | shadow | write @ hw/misc/gnw_h7b0_tim2.c:133 `if ((addr & 0x3ffu) == GNW_H7B0_TIM2_EGR_OFFSET && (val64 & 0x1u)) {` |
| GNW_H7B0_TIM2_CCMR1_Output | 0x18 | shadow | shadow |  |
| GNW_H7B0_TIM2_CCMR1_Input | 0x18 | shadow | shadow |  |
| GNW_H7B0_TIM2_CCMR2_Output | 0x1c | shadow | shadow |  |
| GNW_H7B0_TIM2_CCMR2_Input | 0x1c | shadow | shadow |  |
| GNW_H7B0_TIM2_CCER | 0x20 | shadow | shadow |  |
| GNW_H7B0_TIM2_CNT | 0x24 | shadow | shadow |  |
| GNW_H7B0_TIM2_PSC | 0x28 | shadow | shadow |  |
| GNW_H7B0_TIM2_ARR | 0x2c | shadow | shadow |  |
| GNW_H7B0_TIM2_CCR1 | 0x34 | shadow | shadow |  |
| GNW_H7B0_TIM2_CCR2 | 0x38 | shadow | shadow |  |
| GNW_H7B0_TIM2_CCR3 | 0x3c | shadow | shadow |  |
| GNW_H7B0_TIM2_CCR4 | 0x40 | shadow | shadow |  |
| GNW_H7B0_TIM2_DCR | 0x48 | shadow | shadow |  |
| GNW_H7B0_TIM2_DMAR | 0x4c | shadow | shadow |  |
| GNW_H7B0_TIM2_AF1 | 0x60 | shadow | shadow |  |
| GNW_H7B0_TIM2_TISEL | 0x68 | shadow | shadow |  |

Summary: 22 registers found. Write: 2 explicit / 20 shadow. Read: 0 explicit / 22 shadow.
