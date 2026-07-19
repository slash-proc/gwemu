/*
 * Symbol table for mario (name -> address), pre-extracted from
 * gnwmanager/cli/gnw_patch/binaries/mario/0x08032000.elf's .symtab
 * (remove-keystone-engine branch) -- the "bootloader" ELF variant, since
 * this project's C port always patches with bootloader=True. Symbol
 * addresses genuinely differ by a few bytes between the default and
 * 0x08032000 ELF variants (confirmed empirically), so this must stay
 * paired with that specific variant's .bin, not swapped for "default".
 *
 * Sidesteps runtime ELF parsing entirely -- same approach gnw-web-builder
 * (this project's sibling TypeScript implementation of the same patch
 * pipeline) already uses for its own symbols_mario.json, for the same
 * reason: these are simple static symbol tables that never change unless
 * gnwmanager's own binaries/mario/0x08032000.elf is rebuilt, so a runtime
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
 * gnwmanager's binaries/mario/0x08032000.elf ever changes.
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
#ifndef GNW_SYMBOLS_MARIO_H
#define GNW_SYMBOLS_MARIO_H

#include <stdint.h>
#include <stddef.h>

typedef struct { const char *name; uint32_t addr; } GnwSymbolEntry;

static const GnwSymbolEntry gnw_symbols_mario[] = {
    {"__EH_FRAME_BEGIN__", 0x0801a044},
    {"$t", 0x08018100},
    {"deregister_tm_clones", 0x08018101},
    {"$d", 0x08018110},
    {"$t", 0x0801811c},
    {"register_tm_clones", 0x0801811d},
    {"$d", 0x08018134},
    {"$t", 0x08018140},
    {"__do_global_dtors_aux", 0x08018141},
    {"$d", 0x0801815c},
    {"completed.1", 0x30010010},
    {"$d", 0x0801a088},
    {"__do_global_dtors_aux_fini_array_entry", 0x0801a088},
    {"$t", 0x08018168},
    {"frame_dummy", 0x08018169},
    {"$d", 0x08018180},
    {"object.0", 0x30010014},
    {"$d", 0x0801a084},
    {"__frame_dummy_init_array_entry", 0x0801a084},
    {"$d", 0x30010010},
    {"$d", 0x30010014},
    {"$t", 0x0801818c},
    {"start_app", 0x0801818d},
    {"$t", 0x08018192},
    {"SzAlloc", 0x08018193},
    {"$t", 0x08018196},
    {"SzFree", 0x08018197},
    {"$t", 0x08018198},
    {"$d", 0x0801828c},
    {"$t", 0x080182ac},
    {"$d", 0x080182f0},
    {"$t", 0x08018308},
    {"$d", 0x08018348},
    {"$t", 0x08018350},
    {"$t", 0x08018368},
    {"$d", 0x080183a8},
    {"$t", 0x080183bc},
    {"$t", 0x080183c0},
    {"lzma_heap", 0x3001002c},
    {"$d", 0x3001002c},
    {"$d", 0x0801a070},
    {"$d", 0x0801a05c},
    {"$t", 0x080183c4},
    {"LzmaDec_WriteRem", 0x080183c5},
    {"$t", 0x08018418},
    {"LzmaDec_DecodeReal2", 0x08018419},
    {"$d", 0x08018458},
    {"$t", 0x0801845c},
    {"$t", 0x0801942c},
    {"LzmaDec_TryDummy", 0x0801942d},
    {"$t", 0x08019820},
    {"$t", 0x08019832},
    {"$t", 0x08019a38},
    {"$t", 0x08019a4a},
    {"LzmaDec_AllocateProbs2.isra.0", 0x08019a4b},
    {"$t", 0x08019a8a},
    {"$t", 0x08019aca},
    {"$t", 0x08019afe},
    {"$t", 0x08019b80},
    {"$t", 0x08019b84},
    {"$d", 0x08019bc4},
    {"$t", 0x08019bd0},
    {"$d", 0x08019c1c},
    {"$d", 0x3001000c},
    {"$t", 0x08019c2c},
    {"$d", 0x08019c4c},
    {"$t", 0x08019c50},
    {"$d", 0x08019cac},
    {"$t", 0x08019cb4},
    {"$d", 0x08019cd8},
    {"$t", 0x08019cdc},
    {"$d", 0x08019e98},
    {"$t", 0x08019ea0},
    {"$t", 0x08019eac},
    {"$d", 0x08019eb8},
    {"$t", 0x08019ebc},
    {"$d", 0x08019fe4},
    {"$t", 0x0801a000},
    {"$t", 0x0801a00c},
    {"$t", 0x0801a018},
    {"$t", 0x0801a028},
    {"$t", 0x0801a044},
    {"$t", 0x0801a050},
    {"$t", 0x0801a048},
    {"$t", 0x0801a054},
    {"$d", 0x30010000},
    {"$d", 0x30010004},
    {"__init_array_end", 0x0801a088},
    {"__preinit_array_end", 0x0801a084},
    {"__init_array_start", 0x0801a084},
    {"__preinit_array_start", 0x0801a084},
    {"$d", 0x0801a068},
    {"$d", 0x0801a074},
    {"$d", 0x30010008},
    {"rwdata_inflate", 0x08018351},
    {"HAL_RTCEx_BKUPRead", 0x0801a00d},
    {"HAL_NVIC_SetPriority", 0x08019c51},
    {"HardFault_Handler", 0x080183c1},
    {"_Min_Stack_Size", 0x00000400},
    {"_sidata", 0x0801a08c},
    {"NMI_Handler", 0x080183bd},
    {"__exidx_end", 0x0801a084},
    {"HAL_RCC_GetSysClockFreq", 0x08019ebd},
    {"LzmaDec_DecodeToDic", 0x08019833},
    {"bss_rwdata_init", 0x08018369},
    {"HAL_MspInit", 0x08019b81},
    {"_etext", 0x0801a05c},
    {"_sbss", 0x30010010},
    {"HAL_GPIO_Init", 0x08019cdd},
    {"memcpy", 0x0801a029},
    {"__TMC_END__", 0x30010010},
    {"SystemCoreClock", 0x30010004},
    {"uwTickFreq", 0x30010008},
    {"__RAM_ORIGIN__", 0x30010000},
    {"bootloader", 0x08018199},
    {"__bss_start__", 0x30010010},
    {"LzmaDec_Init", 0x08019821},
    {"HAL_GPIO_ReadPin", 0x08019ea1},
    {"_sdata", 0x30010000},
    {"SystemD2Clock", 0x30010000},
    {"HAL_SYSTICK_Config", 0x08019cb5},
    {"__exidx_start", 0x0801a084},
    {"D1CorePrescTable", 0x0801a074},
    {"_init", 0x0801a045},
    {"_ebss", 0x30013fac},
    {"uwTickPrio", 0x3001000c},
    {"HAL_Init", 0x08019bd1},
    {"read_buttons", 0x080182ad},
    {"__STOCK_ROM_END__", 0x00018100},
    {"end", 0x30013fb0},
    {"LZMA_PROP_DATA", 0x0801a068},
    {"__bss_end__", 0x30013fac},
    {"memcpy_inflate", 0x08018309},
    {"_Min_Heap_Size", 0x00000200},
    {"LzmaDec_AllocateProbs", 0x08019acb},
    {"LzmaDec_FreeProbs", 0x08019a39},
    {"HAL_NVIC_SetPriorityGrouping", 0x08019c2d},
    {"LzmaProps_Decode", 0x08019a8b},
    {"memset", 0x0801a019},
    {"HAL_PWR_EnableBkUpAccess", 0x08019ead},
    {"HAL_RTCEx_BKUPWrite", 0x0801a001},
    {"_fini", 0x0801a051},
    {"HAL_InitTick", 0x08019b85},
    {"_estack", 0x20011330},
    {"_edata", 0x30010010},
    {"SMB1_ROM", 0x0801a070},
    {"LzmaDecode", 0x08019aff},
    {"__RAM_LENGTH__", 0x0000e000},
};
static const size_t gnw_symbols_mario_count =
    sizeof(gnw_symbols_mario) / sizeof(gnw_symbols_mario[0]);

#endif
