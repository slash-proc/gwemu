import xml.etree.ElementTree as ET
import sys
import os

def parse_int(val_str):
    if val_str is None:
        return 0
    val_str = val_str.lower().strip()
    if val_str.startswith('0x'):
        return int(val_str, 16)
    elif val_str.startswith('#'):
        # Some SVDs use # for binary or other formats, but usually not ST
        val_str = val_str[1:]
        if val_str.startswith('0x'):
            return int(val_str, 16)
        return int(val_str, 2)
    return int(val_str)

def generate_header(svd_file, peripheral_name, out_dir, output_name=None):
    tree = ET.parse(svd_file)
    root = tree.getroot()
    
    peripherals = root.find('peripherals')
    target = None
    for p in peripherals.findall('peripheral'):
        name = p.find('name').text
        if name == peripheral_name or (p.find('groupName') is not None and p.find('groupName').text == peripheral_name):
            target = p
            break
            
    if target is None:
        print(f"Peripheral {peripheral_name} not found in {svd_file}")
        sys.exit(1)
        
    pname = output_name if output_name else peripheral_name.lower()
    
    out_lines = []
    out_lines.append(f"/* Auto-generated from {os.path.basename(svd_file)} for {peripheral_name} */")
    out_lines.append(f"#ifndef GNW_H7B0_REGS_{peripheral_name.upper()}_H")
    out_lines.append(f"#define GNW_H7B0_REGS_{peripheral_name.upper()}_H")
    out_lines.append("")
    out_lines.append("#include <stdint.h>")
    out_lines.append("")
    
    regs_node = target.find('registers')
    if regs_node is None:
        print("No registers found.")
        sys.exit(1)
        
    registers = {}
    
    for reg in regs_node.findall('register'):
        reg_name = reg.find('name').text
        if reg_name.startswith(peripheral_name.upper() + '_'):
            clean_reg_name = reg_name[len(peripheral_name) + 1:]
        else:
            clean_reg_name = reg_name
        offset = parse_int(reg.find('addressOffset').text)
        
        # default reg access
        reg_access_node = reg.find('access')
        reg_access = reg_access_node.text if reg_access_node is not None else 'read-write'
        
        reset_val_node = reg.find('resetValue')
        reset_val = parse_int(reset_val_node.text) if reset_val_node is not None else 0
        
        fields = reg.find('fields')
        wmask = 0
        
        if fields is not None:
            for field in fields.findall('field'):
                bit_offset = parse_int(field.find('bitOffset').text)
                bit_width = parse_int(field.find('bitWidth').text)
                access_node = field.find('access')
                access = access_node.text if access_node is not None else reg_access
                
                # If access is writable (read-write, write-only, writeOnce)
                if 'write' in access or 'Write' in access:
                    mask = ((1 << bit_width) - 1) << bit_offset
                    wmask |= mask
        else:
            if 'write' in reg_access:
                size_node = reg.find('size')
                size = parse_int(size_node.text) if size_node is not None else 32
                wmask = (1 << size) - 1
                
        if offset in registers:
            _, _, exist_reset, exist_wmask = registers[offset]
            registers[offset] = (clean_reg_name, offset, exist_reset | reset_val, exist_wmask | wmask)
        else:
            registers[offset] = (clean_reg_name, offset, reset_val, wmask)
            
        # define macros
        macro_prefix = f"GNW_H7B0_{peripheral_name.upper()}_{clean_reg_name.replace('%s', '')}"
        out_lines.append(f"#define {macro_prefix}_OFFSET 0x{offset:x}")
        out_lines.append(f"#define {macro_prefix}_RESET  0x{reset_val:08x}")
        out_lines.append(f"#define {macro_prefix}_WMASK  0x{wmask:08x}")
        out_lines.append("")
        
    out_lines.append(f"static inline uint32_t get_{pname}_write_mask(uint32_t offset) {{")
    out_lines.append("    switch (offset) {")
    for offset, (reg_name, _, _, wmask) in registers.items():
        macro_prefix = f"GNW_H7B0_{peripheral_name.upper()}_{reg_name.replace('%s', '')}"
        out_lines.append(f"        case {macro_prefix}_OFFSET:")
        out_lines.append(f"            return 0x{wmask:08x};")
    out_lines.append("        default:")
    out_lines.append("            return 0x00000000; /* Read-only or unmapped by default */")
    out_lines.append("    }")
    out_lines.append("}")
    out_lines.append("")
    
    out_lines.append(f"static inline uint32_t get_{pname}_reset_value(uint32_t offset) {{")
    out_lines.append("    switch (offset) {")
    for offset, (reg_name, _, reset_val, _) in registers.items():
        macro_prefix = f"GNW_H7B0_{peripheral_name.upper()}_{reg_name.replace('%s', '')}"
        out_lines.append(f"        case {macro_prefix}_OFFSET:")
        out_lines.append(f"            return 0x{reset_val:08x};")
    out_lines.append("        default:")
    out_lines.append("            return 0x00000000;")
    out_lines.append("    }")
    out_lines.append("}")
    out_lines.append("")
    out_lines.append(f"#endif /* GNW_H7B0_REGS_{peripheral_name.upper()}_H */")
    
    out_path = os.path.join(out_dir, f"gnw_h7b0_regs_{pname}.h")
    with open(out_path, 'w') as f:
        f.write('\n'.join(out_lines))
        f.write('\n')
        
    print(f"Generated {out_path}")

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: python gen_regs.py <svd_file> <peripheral_name> <out_dir>")
        sys.exit(1)
    output_name = sys.argv[4] if len(sys.argv) > 4 else None
    generate_header(sys.argv[1], sys.argv[2], sys.argv[3], output_name)
