/*
 * Rate-limited LOG_UNIMP helper for "plain shadow register" hits
 * (Nintendo Game & Watch)
 *
 * Some plain-shadow registers (e.g. SPI1's CR2/CFG1/CFG2/IER, rewritten
 * by real firmware on literally every SPI byte transferred) get hit
 * often enough during ordinary operation that an unconditional
 * qemu_log_mask() per write turns on a real perf problem the moment
 * `-d guest_errors,unimp` is enabled, silently defeating this project's
 * primary debugging tool. These registers are fine to leave unmodeled;
 * only the logging needs to stop being O(every access). Log once per
 * offset per device instance instead.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_MISC_GNW_H7B0_STUB_LOG_H
#define HW_MISC_GNW_H7B0_STUB_LOG_H

#include "qemu/log.h"

/*
 * `logged` is expected to be a `bool logged_unimp[REGS_SIZE / 4]` array
 * parallel to a device's `regs[]`, indexed the same way (addr >> 2), so
 * each offset gets its own one-shot flag.
 */
static inline void gnw_h7b0_stub_log_unimp_ratelimited(bool *logged,
                                                        const char *func,
                                                        hwaddr addr)
{
    if (logged[addr >> 2]) {
        return;
    }
    logged[addr >> 2] = true;
    qemu_log_mask(LOG_UNIMP,
                  "%s: offset 0x%"HWADDR_PRIx" is a plain read/write shadow, "
                  "no real transfer modeled (further hits on this offset "
                  "this session are suppressed)\n", func, addr);
}

#endif
