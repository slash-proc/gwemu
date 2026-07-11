import sys
import os

if len(sys.argv) < 3:
    print("Usage: python gen_stub.py <PeripheralNameLower> <PeripheralNameCamel>")
    sys.exit(1)

p_lower = sys.argv[1]
p_camel = sys.argv[2]
p_upper = p_lower.upper()

header = f"""/* Auto-generated stub for {p_upper} */
#ifndef HW_MISC_GNW_H7B0_{p_upper}_H
#define HW_MISC_GNW_H7B0_{p_upper}_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GNW_H7B0_{p_upper} "gnw-h7b0-{p_lower}"
OBJECT_DECLARE_SIMPLE_TYPE(GnwH7B0{p_camel}State, GNW_H7B0_{p_upper})

#define GNW_H7B0_{p_upper}_SIZE 0x400

struct GnwH7B0{p_camel}State {{
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[GNW_H7B0_{p_upper}_SIZE / 4];
}};

#endif
"""

c_code = f"""/* Auto-generated stub for {p_upper} */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/gnw_h7b0_{p_lower}.h"
#include "hw/misc/gnw_h7b0_regs_{p_lower}.h"

static void gnw_h7b0_{p_lower}_reset(DeviceState *dev)
{{
    GnwH7B0{p_camel}State *s = GNW_H7B0_{p_upper}(dev);
    for (int i = 0; i < (GNW_H7B0_{p_upper}_SIZE / 4); i++) {{
        s->regs[i] = get_{p_lower}_reset_value(i * 4);
    }}
}}

static uint64_t gnw_h7b0_{p_lower}_read(void *opaque, hwaddr addr, unsigned int size)
{{
    GnwH7B0{p_camel}State *s = GNW_H7B0_{p_upper}(opaque);
    if (addr >= GNW_H7B0_{p_upper}_SIZE) {{
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\\n", __func__, addr);
        return 0;
    }}
    return s->regs[addr >> 2];
}}

static void gnw_h7b0_{p_lower}_write(void *opaque, hwaddr addr, uint64_t val64, unsigned int size)
{{
    GnwH7B0{p_camel}State *s = GNW_H7B0_{p_upper}(opaque);
    if (addr >= GNW_H7B0_{p_upper}_SIZE) {{
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%"HWADDR_PRIx"\\n", __func__, addr);
        return;
    }}
    uint32_t mask = get_{p_lower}_write_mask(addr);
    s->regs[addr >> 2] = (s->regs[addr >> 2] & ~mask) | ((uint32_t)val64 & mask);
}}

static const MemoryRegionOps gnw_h7b0_{p_lower}_ops = {{
    .read = gnw_h7b0_{p_lower}_read,
    .write = gnw_h7b0_{p_lower}_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {{ .min_access_size = 1, .max_access_size = 4 }},
    .impl = {{ .min_access_size = 1, .max_access_size = 4 }},
}};

static void gnw_h7b0_{p_lower}_init(Object *obj)
{{
    GnwH7B0{p_camel}State *s = GNW_H7B0_{p_upper}(obj);
    memory_region_init_io(&s->mmio, obj, &gnw_h7b0_{p_lower}_ops, s, TYPE_GNW_H7B0_{p_upper}, GNW_H7B0_{p_upper}_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}}

static const VMStateDescription vmstate_gnw_h7b0_{p_lower} = {{
    .name = TYPE_GNW_H7B0_{p_upper},
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {{
        VMSTATE_UINT32_ARRAY(regs, GnwH7B0{p_camel}State, GNW_H7B0_{p_upper}_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }}
}};

static void gnw_h7b0_{p_lower}_class_init(ObjectClass *klass, void *data)
{{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &vmstate_gnw_h7b0_{p_lower};
    device_class_set_legacy_reset(dc, gnw_h7b0_{p_lower}_reset);
}}

static const TypeInfo gnw_h7b0_{p_lower}_info = {{
    .name          = TYPE_GNW_H7B0_{p_upper},
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GnwH7B0{p_camel}State),
    .instance_init = gnw_h7b0_{p_lower}_init,
    .class_init    = gnw_h7b0_{p_lower}_class_init,
}};

static void gnw_h7b0_{p_lower}_register_types(void)
{{
    type_register_static(&gnw_h7b0_{p_lower}_info);
}}
type_init(gnw_h7b0_{p_lower}_register_types)
"""

with open(f"include/hw/misc/gnw_h7b0_{p_lower}.h", "w") as f:
    f.write(header)

with open(f"hw/misc/gnw_h7b0_{p_lower}.c", "w") as f:
    f.write(c_code)

print(f"Generated {p_lower} stub")
