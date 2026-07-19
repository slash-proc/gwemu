/*
 * Symbol table for zelda (name -> address), pre-extracted from
 * gnwmanager/cli/gnw_patch/binaries/zelda/0x08032000.elf's .symtab
 * (remove-keystone-engine branch) -- the "bootloader" ELF variant, since
 * this project's C port always patches with bootloader=True. Symbol
 * addresses genuinely differ by a few bytes between the default and
 * 0x08032000 ELF variants (confirmed empirically), so this must stay
 * paired with that specific variant's .bin, not swapped for "default".
 *
 * Sidesteps runtime ELF parsing entirely -- same approach gnw-web-builder
 * (this project's sibling TypeScript implementation of the same patch
 * pipeline) already uses for its own symbols_zelda.json, for the same
 * reason: these are simple static symbol tables that never change unless
 * gnwmanager's own binaries/zelda/0x08032000.elf is rebuilt, so a runtime
 * ELF parser is a whole class of bugs this project doesn't need to own.
 *
 * Deliberately kept in original ELF .symtab order (NOT sorted) and looked
 * up via first-match linear scan -- gnwmanager's own address() takes
 * symbols[0] from pyelftools' get_symbol_by_name(), i.e. first match in
 * table order, and this ELF genuinely has duplicate-named entries (the
 * ARM mapping symbols "$t"/"$d", which appear once per code/data region
 * boundary) where match order matters for exact equivalence, even though
 * no real patch-sequence code in mario.py/zelda.py happens to reference
 * those specific duplicated names.
 *
 * Regenerate by re-running the extraction against a fresh .elf if
 * gnwmanager's binaries/zelda/0x08032000.elf ever changes.
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
#ifndef GNW_SYMBOLS_ZELDA_H
#define GNW_SYMBOLS_ZELDA_H

#include <stdint.h>
#include <stddef.h>

typedef struct { const char *name; uint32_t addr; } GnwSymbolEntry;

static const GnwSymbolEntry gnw_symbols_zelda[] = {
    {"__EH_FRAME_BEGIN__", 0x0801d324},
    {"$t", 0x0801b3e0},
    {"deregister_tm_clones", 0x0801b3e1},
    {"$d", 0x0801b3f0},
    {"$t", 0x0801b3fc},
    {"register_tm_clones", 0x0801b3fd},
    {"$d", 0x0801b414},
    {"$t", 0x0801b420},
    {"__do_global_dtors_aux", 0x0801b421},
    {"$d", 0x0801b43c},
    {"completed.1", 0x240ec534},
    {"$d", 0x0801d364},
    {"__do_global_dtors_aux_fini_array_entry", 0x0801d364},
    {"$t", 0x0801b448},
    {"frame_dummy", 0x0801b449},
    {"$d", 0x0801b460},
    {"object.0", 0x240ec538},
    {"$d", 0x0801d360},
    {"__frame_dummy_init_array_entry", 0x0801d360},
    {"$d", 0x240ec534},
    {"$d", 0x240ec538},
    {"$t", 0x0801b46c},
    {"start_app", 0x0801b46d},
    {"$t", 0x0801b472},
    {"SzAlloc", 0x0801b473},
    {"$t", 0x0801b476},
    {"SzFree", 0x0801b477},
    {"$t", 0x0801b478},
    {"$d", 0x0801b56c},
    {"$t", 0x0801b58c},
    {"$d", 0x0801b5d0},
    {"$t", 0x0801b5e8},
    {"$d", 0x0801b628},
    {"$t", 0x0801b630},
    {"$t", 0x0801b648},
    {"$d", 0x0801b688},
    {"$t", 0x0801b69c},
    {"$t", 0x0801b6a0},
    {"lzma_heap", 0x240ec550},
    {"$d", 0x240ec550},
    {"$d", 0x0801d33c},
    {"$t", 0x0801b6a4},
    {"LzmaDec_WriteRem", 0x0801b6a5},
    {"$t", 0x0801b6f8},
    {"LzmaDec_DecodeReal2", 0x0801b6f9},
    {"$d", 0x0801b738},
    {"$t", 0x0801b73c},
    {"$t", 0x0801c70c},
    {"LzmaDec_TryDummy", 0x0801c70d},
    {"$t", 0x0801cb00},
    {"$t", 0x0801cb12},
    {"$t", 0x0801cd18},
    {"$t", 0x0801cd2a},
    {"LzmaDec_AllocateProbs2.isra.0", 0x0801cd2b},
    {"$t", 0x0801cd6a},
    {"$t", 0x0801cdaa},
    {"$t", 0x0801cdde},
    {"$t", 0x0801ce60},
    {"$t", 0x0801ce64},
    {"$d", 0x0801cea4},
    {"$t", 0x0801ceb0},
    {"$d", 0x0801cefc},
    {"$d", 0x240ec530},
    {"$t", 0x0801cf0c},
    {"$d", 0x0801cf2c},
    {"$t", 0x0801cf30},
    {"$d", 0x0801cf8c},
    {"$t", 0x0801cf94},
    {"$d", 0x0801cfb8},
    {"$t", 0x0801cfbc},
    {"$d", 0x0801d178},
    {"$t", 0x0801d180},
    {"$t", 0x0801d18c},
    {"$d", 0x0801d198},
    {"$t", 0x0801d19c},
    {"$d", 0x0801d2c4},
    {"$t", 0x0801d2e0},
    {"$t", 0x0801d2ec},
    {"$t", 0x0801d2f8},
    {"$t", 0x0801d308},
    {"$t", 0x0801d324},
    {"$t", 0x0801d330},
    {"$t", 0x0801d328},
    {"$t", 0x0801d334},
    {"$d", 0x240ec524},
    {"$d", 0x240ec528},
    {"__init_array_end", 0x0801d364},
    {"__preinit_array_end", 0x0801d360},
    {"__init_array_start", 0x0801d360},
    {"__preinit_array_start", 0x0801d360},
    {"$d", 0x0801d348},
    {"$d", 0x0801d34d},
    {"$d", 0x240ec52c},
    {"rwdata_inflate", 0x0801b631},
    {"HAL_RTCEx_BKUPRead", 0x0801d2ed},
    {"HAL_NVIC_SetPriority", 0x0801cf31},
    {"HardFault_Handler", 0x0801b6a1},
    {"_Min_Stack_Size", 0x00000400},
    {"_sidata", 0x0801d368},
    {"NMI_Handler", 0x0801b69d},
    {"__exidx_end", 0x0801d360},
    {"HAL_RCC_GetSysClockFreq", 0x0801d19d},
    {"LzmaDec_DecodeToDic", 0x0801cb13},
    {"bss_rwdata_init", 0x0801b649},
    {"HAL_MspInit", 0x0801ce61},
    {"_etext", 0x0801d33c},
    {"_sbss", 0x240ec534},
    {"HAL_GPIO_Init", 0x0801cfbd},
    {"memcpy", 0x0801d309},
    {"__TMC_END__", 0x240ec534},
    {"SystemCoreClock", 0x240ec528},
    {"uwTickFreq", 0x240ec52c},
    {"__RAM_ORIGIN__", 0x240ec524},
    {"bootloader", 0x0801b479},
    {"__bss_start__", 0x240ec534},
    {"LzmaDec_Init", 0x0801cb01},
    {"HAL_GPIO_ReadPin", 0x0801d181},
    {"_sdata", 0x240ec524},
    {"SystemD2Clock", 0x240ec524},
    {"HAL_SYSTICK_Config", 0x0801cf95},
    {"__exidx_start", 0x0801d360},
    {"D1CorePrescTable", 0x0801d34d},
    {"_init", 0x0801d325},
    {"_ebss", 0x240f04d0},
    {"uwTickPrio", 0x240ec530},
    {"HAL_Init", 0x0801ceb1},
    {"read_buttons", 0x0801b58d},
    {"__STOCK_ROM_END__", 0x0001b3e0},
    {"end", 0x240f04d0},
    {"LZMA_PROP_DATA", 0x0801d348},
    {"__bss_end__", 0x240f04d0},
    {"memcpy_inflate", 0x0801b5e9},
    {"_Min_Heap_Size", 0x00000200},
    {"LzmaDec_AllocateProbs", 0x0801cdab},
    {"LzmaDec_FreeProbs", 0x0801cd19},
    {"HAL_NVIC_SetPriorityGrouping", 0x0801cf0d},
    {"LzmaProps_Decode", 0x0801cd6b},
    {"memset", 0x0801d2f9},
    {"HAL_PWR_EnableBkUpAccess", 0x0801d18d},
    {"HAL_RTCEx_BKUPWrite", 0x0801d2e1},
    {"_fini", 0x0801d331},
    {"HAL_InitTick", 0x0801ce65},
    {"_estack", 0x20011330},
    {"_edata", 0x240ec534},
    {"LzmaDecode", 0x0801cddf},
    {"__RAM_LENGTH__", 0x00010ad4},
};
static const size_t gnw_symbols_zelda_count =
    sizeof(gnw_symbols_zelda) / sizeof(gnw_symbols_zelda[0]);

#endif
