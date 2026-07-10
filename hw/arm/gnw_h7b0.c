/*
 * Nintendo Game & Watch (STM32H7B0) Machine Model
 *
 * Phase 0 skeleton: instantiates the bare gnw-h7b0-soc (Cortex-M7 + DTCM +
 * AXI SRAM, no peripherals) and loads a kernel image into AXI SRAM for
 * bring-up testing. See ../../docs/roadmap.md.
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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/qdev-clock.h"
#include "hw/arm/gnw_h7b0_soc.h"
#include "hw/arm/boot.h"

/* Real STM32H7B0 SYSCLK is configurable up to 280MHz; use max for now. */
#define SYSCLK_FRQ 280000000ULL

static void gnw_h7b0_init(MachineState *machine)
{
    DeviceState *dev;
    Clock *sysclk;

    sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, SYSCLK_FRQ);

    dev = qdev_new(TYPE_GNW_H7B0_SOC);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
    qdev_connect_clock_in(dev, "sysclk", sysclk);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    armv7m_load_kernel(ARM_CPU(first_cpu),
                        machine->kernel_filename,
                        AXISRAM_BASE_ADDRESS, AXISRAM_SIZE);
}

static void gnw_h7b0_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m7"),
        NULL
    };

    mc->desc = "Nintendo Game & Watch (STM32H7B0, Cortex-M7)";
    mc->init = gnw_h7b0_init;
    mc->valid_cpu_types = valid_cpu_types;
}

DEFINE_MACHINE("gnw-h7b0", gnw_h7b0_machine_init)
