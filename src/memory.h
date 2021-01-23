/* SPDX-License-Identifier: MIT */

#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

/*
 * https://armv8-ref.codingbelief.com/en/chapter_d4/d43_2_armv8_translation_table_level_3_descriptor_formats.html
 */
#define PTE_TYPE_BLOCK 0b01
#define PTE_TYPE_TABLE 0b11
#define PTE_FLAG_ACCESS (1 << 10) // AF / access flag
#define PTE_MAIR_INDEX(i) ((i & 7) << 2)

/*
 * https://developer.arm.com/docs/ddi0595/g/aarch64-system-registers/sctlr_el2
 * SCTL_I enables instruction caches.
 * SCTL_C enables data caches.
 * SCTL_M enables the MMU.
 */
#define SCTL_I (1UL << 12)
#define SCTL_C (1UL << 2)
#define SCTL_M (1UL)

/*
 * https://developer.arm.com/documentation/100442/0100/register-descriptions/aarch64-system-registers/tcr-el2--translation-control-register--el2
 */
#define PS_1TB ((0b010UL) << 16)
#define TG0_16K ((0b10UL) << 14)

/*
 * aarch64 allows to configure attribute sets for up to eight different memory
 * types. we need normal memory and two types of device memory (nGnRnE and
 * nGnRE) in m1n1.
 * The indexes here are selected arbitrarily: A page table entry
 * contains fields for one of these which will then be used
 * to select the corresponding memory access flags from MAIR.
 */
#define MAIR_INDEX_NORMAL 0
#define MAIR_INDEX_DEVICE_nGnRnE 1
#define MAIR_INDEX_DEVICE_nGnRE 2

#define MAIR_SHIFT_NORMAL (MAIR_INDEX_NORMAL * 8)
#define MAIR_SHIFT_DEVICE_nGnRnE (MAIR_INDEX_DEVICE_nGnRnE * 8)
#define MAIR_SHIFT_DEVICE_nGnRE (MAIR_INDEX_DEVICE_nGnRE * 8)

/*
 * https://developer.arm.com/documentation/ddi0500/e/system-control/aarch64-register-descriptions/memory-attribute-indirection-register--el1
 *
 * MAIR_ATTR_NORMAL_DEFAULT sets Normal Memory, Outer Write-back non-transient,
 * Inner Write-back non-transient, R=1, W=1
 *
 * MAIR_ATTR_DEVICE_nGnRnE sets Device-nGnRnE memory
 *
 * MAIR_ATTR_DEVICE_nGnRnE  sets Device-nGnRE memory
 */
#define MAIR_ATTR_NORMAL_DEFAULT 0xffUL
#define MAIR_ATTR_DEVICE_nGnRnE 0x00UL
#define MAIR_ATTR_DEVICE_nGnRE 0x04UL

void ic_ivau_range(void *addr, size_t length);
void dc_ivac_range(void *addr, size_t length);
void dc_zva_range(void *addr, size_t length);
void dc_cvac_range(void *addr, size_t length);
void dc_cvau_range(void *addr, size_t length);
void dc_civac_range(void *addr, size_t length);

#define DCSW_OP_DCISW 0x0
#define DCSW_OP_DCCISW 0x1
#define DCSW_OP_DCCSW 0x2
void dcsw_op_all(uint64_t op_type);

void mmu_init(void);
void mmu_shutdown(void);
#endif
