/* SPDX-License-Identifier: MIT */

#include "memory.h"
#include "utils.h"

#define PAGE_SIZE 0x4000
#define CACHE_LINE_SIZE 64

#define CACHE_RANGE_OP(func, op)                                               \
    void func(void *addr, size_t length)                                       \
    {                                                                          \
        u64 p = (u64)addr;                                                     \
        u64 end = p + length;                                                  \
        while (p < end) {                                                      \
            cacheop(op, p);                                                    \
            p += CACHE_LINE_SIZE;                                              \
        }                                                                      \
    }

CACHE_RANGE_OP(ic_ivau_range, "ic ivau")
CACHE_RANGE_OP(dc_ivac_range, "dc ivac")
CACHE_RANGE_OP(dc_zva_range, "dc zva")
CACHE_RANGE_OP(dc_cvac_range, "dc cvac")
CACHE_RANGE_OP(dc_cvau_range, "dc cvau")
CACHE_RANGE_OP(dc_civac_range, "dc civac")

static inline uint64_t read_sctl(void)
{
    sysop("isb");
    return mrs(SCTLR_EL2);
}

static inline void write_sctl(uint64_t val)
{
    msr(SCTLR_EL2, val);
    sysop("isb");
}

/*
 * We have to use 16KB pages on the M1 which would usually result in the
 * following virtual address space:
 *
 * [L0 index]  [L1 index]  [L2 index]  [L3 index] [page offset]
 *   1 bit      11 bits      11 bits     11 bits    14 bits
 *
 * To simplify things we only allow 32MB mappings directly from
 * the L2 tables such that in m1n1 all virtual addresses will look like this
 * instead (Block maps from L0 or L1 are not possible with 16KB pages):
 *
 * [L0 index]  [L1 index]  [L2 index]  [page offset]
 *   1 bit      11 bits      11 bits     25 bits
 *
 * We initalize two L1 tables which cover the entire virtual memory space,
 * point to them in the singe L0 table and then create L2 tables on demand.
 */
#define VADDR_PAGE_OFFSET_BITS 25
#define VADDR_L2_INDEX_BITS 11
#define VADDR_L1_INDEX_BITS 11
#define VADDR_L0_INDEX_BITS 1

#define MAX_L2_TABLES 10
#define ENTRIES_PER_TABLE 2048
#define L2_PAGE_SIZE 0x2000000

static uint64_t __pagetable_L0[2] ALIGNED(PAGE_SIZE);
static uint64_t __pagetable_L1[2][ENTRIES_PER_TABLE] ALIGNED(PAGE_SIZE);
static uint64_t
    __pagetable_L2[MAX_L2_TABLES][ENTRIES_PER_TABLE] ALIGNED(PAGE_SIZE);
static uint32_t __pagetable_L2_next = 0;

static uint64_t _mmu_make_block_pte(uintptr_t addr, uint8_t attribute_index)
{
    uint64_t pte = PTE_TYPE_BLOCK;
    pte |= addr;
    pte |= PTE_FLAG_ACCESS;
    pte |= PTE_MAIR_INDEX(attribute_index);

    return pte;
}

static uint64_t _mmu_make_table_pte(uint64_t *addr)
{
    uint64_t pte = PTE_TYPE_TABLE;
    pte |= (uintptr_t)addr;
    pte |= PTE_FLAG_ACCESS;
    return pte;
}

static void _mmu_init_pagetables(void)
{
    memset64(__pagetable_L0, 0, sizeof __pagetable_L0);
    memset64(__pagetable_L1, 0, sizeof __pagetable_L1);
    memset64(__pagetable_L2, 0, sizeof __pagetable_L2);

    __pagetable_L0[0] = _mmu_make_table_pte(&__pagetable_L1[0][0]);
    __pagetable_L0[1] = _mmu_make_table_pte(&__pagetable_L1[1][0]);
}

static uint64_t _mmu_extract_L0_index(uintptr_t addr)
{
    addr >>= VADDR_PAGE_OFFSET_BITS;
    addr >>= VADDR_L2_INDEX_BITS;
    addr >>= VADDR_L1_INDEX_BITS;
    addr &= (1 << VADDR_L0_INDEX_BITS) - 1;
    return (uint8_t)addr;
}

static uint64_t _mmu_extract_L1_index(uintptr_t addr)
{
    addr >>= VADDR_PAGE_OFFSET_BITS;
    addr >>= VADDR_L2_INDEX_BITS;
    addr &= (1 << VADDR_L1_INDEX_BITS) - 1;
    return (uint32_t)addr;
}

static uint64_t _mmu_extract_L2_index(uintptr_t addr)
{
    addr >>= VADDR_PAGE_OFFSET_BITS;
    addr &= (1 << VADDR_L2_INDEX_BITS) - 1;
    return (uint32_t)addr;
}

static uintptr_t _mmu_extract_addr(uint64_t pte)
{
    // https://armv8-ref.codingbelief.com/en/chapter_d4/d43_1_vmsav8-64_translation_table_descriptor_formats.html
    // need to extract bits [47:14]
    pte &= ((1ULL << 48) - 1);
    pte &= ~((1ULL << 14) - 1);
    return (uintptr_t)pte;
}

static uint64_t *_mmu_get_L1_table(uintptr_t addr)
{
    return __pagetable_L1[_mmu_extract_L0_index(addr)];
}

static uint64_t *_mmu_get_L2_table(uintptr_t addr)
{
    uint64_t *tbl_l1 = _mmu_get_L1_table(addr);

    uint64_t l1_idx = _mmu_extract_L1_index(addr);
    uint64_t desc_l1 = tbl_l1[l1_idx];

    if (desc_l1 == 0) {
        if (__pagetable_L2_next == MAX_L2_TABLES)
            panic("MMU: not enough space to create an additional L2 table to "
                  "map %lx",
                  addr);

        desc_l1 = _mmu_make_table_pte(
            (uint64_t *)&__pagetable_L2[__pagetable_L2_next++]);
        tbl_l1[l1_idx] = desc_l1;
    }

    return (uint64_t *)_mmu_extract_addr(desc_l1);
}

static void _mmu_add_single_mapping(uintptr_t from, uintptr_t to,
                                    uint8_t attribute_index)
{
    uint64_t *tbl_l2 = _mmu_get_L2_table(from);
    uint64_t l2_idx = _mmu_extract_L2_index(from);

    if (tbl_l2[l2_idx])
        panic("MMU: mapping for %lx already exists", from);

    tbl_l2[l2_idx] = _mmu_make_block_pte(to, attribute_index);
}

static void _mmu_add_mapping(uintptr_t from, uintptr_t to, size_t size,
                             uint8_t attribute_index)
{
    if (from % L2_PAGE_SIZE)
        panic("_mmu_add_mapping: from address not aligned: %lx", from);
    if (to % L2_PAGE_SIZE)
        panic("_mmu_add_mapping: to address not aligned: %lx", to);
    if (size % L2_PAGE_SIZE)
        panic("_mmu_add_mapping: size not aligned: %lx", size);

    while (size > 0) {
        _mmu_add_single_mapping(from, to, attribute_index);
        from += L2_PAGE_SIZE;
        to += L2_PAGE_SIZE;
        size -= L2_PAGE_SIZE;
    }
}

static void _mmu_add_default_mappings(void)
{
    /* create MMIO mapping as both nGnRnE (identity) and nGnRE (starting at
     * 0xf0_0000_0000)
     */
    _mmu_add_mapping(0x0000000000, 0x0000000000, 0x0800000000,
                     MAIR_INDEX_DEVICE_nGnRnE);
    _mmu_add_mapping(0xf000000000, 0x0000000000, 0x0800000000,
                     MAIR_INDEX_DEVICE_nGnRE);

    /* create identity mapping for 16GB RAM from 0x08_0000_0000 to
     * 0x0c_0000_0000 */
    _mmu_add_mapping(0x0800000000, 0x0800000000, 0x0400000000,
                     MAIR_INDEX_NORMAL);
}

static void _mmu_configure(void)
{
    msr(MAIR_EL2, (MAIR_ATTR_NORMAL_DEFAULT << MAIR_SHIFT_NORMAL) |
                      (MAIR_ATTR_DEVICE_nGnRnE << MAIR_SHIFT_DEVICE_nGnRnE) |
                      (MAIR_ATTR_DEVICE_nGnRE << MAIR_SHIFT_DEVICE_nGnRE));
    msr(TCR_EL2, TG0_16K | PS_1TB);
    msr(TTBR0_EL2, (uintptr_t)__pagetable_L0);

    // Armv8-A Address Translation, 100940_0101_en, page 28
    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
}

void mmu_init(void)
{
    printf("MMU: Initializing...\n");

    _mmu_init_pagetables();
    _mmu_add_default_mappings();
    _mmu_configure();

    uint64_t sctl_old = read_sctl();
    uint64_t sctl_new = sctl_old | SCTL_I | SCTL_C | SCTL_M;

    printf("MMU: SCTL_EL2: %x -> %x\n", sctl_old, sctl_new);
    write_sctl(sctl_new);
    printf("MMU: running with MMU and caches enabled!\n");
}

void mmu_shutdown(void)
{
    printf("MMU: shutting down...\n");
    write_sctl(read_sctl() & ~(SCTL_I | SCTL_C | SCTL_M));
    printf("MMU: shutdown successful, clearing caches\n");
    dcsw_op_all(DCSW_OP_DCCISW);
}
