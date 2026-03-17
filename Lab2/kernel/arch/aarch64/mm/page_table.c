/*
 * Copyright (c) 2023 Institute of Parallel And Distributed Systems (IPADS), Shanghai Jiao Tong University (SJTU)
 * Licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifdef CHCORE
#include <common/util.h>
#include <mm/kmalloc.h>
#endif

#include <common/vars.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/errno.h>
#include <lib/printk.h>
#include <mm/vmspace.h>
#include <mm/mm.h>
#include <mm/common_pte.h>
#include <arch/mmu.h>
#include <arch/sync.h>

#include <arch/mm/page_table.h>

/* Page_table.c: Use simple impl for now. */

void set_page_table(paddr_t pgtbl)
{
        set_ttbr0_el1(pgtbl);
}

static int __vmr_prot_to_ap(vmr_prop_t prot)
{
        if ((prot & VMR_READ) && !(prot & VMR_WRITE)) {
                return AARCH64_MMU_ATTR_PAGE_AP_HIGH_RO_EL0_RO;
        } else if (prot & VMR_WRITE) {
                return AARCH64_MMU_ATTR_PAGE_AP_HIGH_RW_EL0_RW;
        }
        return 0;
}

static int __ap_to_vmr_prot(int ap)
{
        if (ap == AARCH64_MMU_ATTR_PAGE_AP_HIGH_RO_EL0_RO) {
                return VMR_READ;
        } else if (ap == AARCH64_MMU_ATTR_PAGE_AP_HIGH_RW_EL0_RW) {
                return VMR_READ | VMR_WRITE;
        }
        return 0;
}

#define USER_PTE   0
#define KERNEL_PTE 1

/*
 * the 3rd arg means the kind of PTE.
 */
__maybe_unused static int set_pte_flags(pte_t *entry, vmr_prop_t flags,
                                        int kind)
{
        BUG_ON(kind != USER_PTE && kind != KERNEL_PTE);

        /*
         * Current access permission (AP) setting:
         * Mapped pages are always readable (No considering XOM).
         * EL1 can directly access EL0 (No restriction like SMAP
         * as ChCore is a microkernel).
         */
        entry->l3_page.AP = __vmr_prot_to_ap(flags);

        // kernel PTE
        if (kind == KERNEL_PTE) {
                if (!(flags & VMR_EXEC))
                        entry->l3_page.PXN = AARCH64_MMU_ATTR_PAGE_PXN;
                entry->l3_page.UXN = AARCH64_MMU_ATTR_PAGE_UXN;

        }
        // User PTE
        else {
                if (!(flags & VMR_EXEC))
                        entry->l3_page.UXN = AARCH64_MMU_ATTR_PAGE_UXN;
                // EL1 cannot directly execute EL0 accessible region.
                entry->l3_page.PXN = AARCH64_MMU_ATTR_PAGE_PXN;
        }

        // Set AF (access flag) in advance.
        entry->l3_page.AF = AARCH64_MMU_ATTR_PAGE_AF_ACCESSED;
        // Mark the mapping as not global
        entry->l3_page.nG = 1;
        // Mark the mappint as inner sharable
        entry->l3_page.SH = INNER_SHAREABLE;
        // Set the memory type
        if (flags & VMR_DEVICE) {
                entry->l3_page.attr_index = DEVICE_MEMORY;
                entry->l3_page.SH = 0;
        } else if (flags & VMR_NOCACHE) {
                entry->l3_page.attr_index = NORMAL_MEMORY_NOCACHE;
        } else {
                entry->l3_page.attr_index = NORMAL_MEMORY;
        }

        return 0;
}

#define GET_PADDR_IN_PTE(entry) \
        (((u64)(entry)->table.next_table_addr) << PAGE_SHIFT)
#define GET_NEXT_PTP(entry) phys_to_virt(GET_PADDR_IN_PTE(entry))

#define NORMAL_PTP (0)
#define BLOCK_PTP  (1)

/*
 * Find next page table page for the "va".
 *
 * cur_ptp: current page table page
 * level:   current ptp level
 *
 * next_ptp: returns "next_ptp"
 * pte     : returns "pte" (points to next_ptp) in "cur_ptp"
 *
 * alloc: if true, allocate a ptp when missing
 *
 */
static int get_next_ptp(ptp_t *cur_ptp, u32 level, vaddr_t va, ptp_t **next_ptp,
                        pte_t **pte, bool alloc, __maybe_unused long *rss)
{
        u32 index = 0;
        pte_t *entry;

        if (cur_ptp == NULL)
                return -ENOMAPPING;

        switch (level) {
        case L0:
                index = GET_L0_INDEX(va);
                break;
        case L1:
                index = GET_L1_INDEX(va);
                break;
        case L2:
                index = GET_L2_INDEX(va);
                break;
        case L3:
                index = GET_L3_INDEX(va);
                break;
        default:
                BUG("unexpected level\n");
                return -EINVAL;
        }

        entry = &(cur_ptp->ent[index]);
        if (IS_PTE_INVALID(entry->pte)) {
                if (alloc == false) {
                        return -ENOMAPPING;
                } else {
                        /* alloc a new page table page */
                        ptp_t *new_ptp;
                        paddr_t new_ptp_paddr;
                        pte_t new_pte_val;

                        /* alloc a single physical page as a new page table page
                         */
                        new_ptp = get_pages(0);
                        if (new_ptp == NULL)
                                return -ENOMEM;
                        memset((void *)new_ptp, 0, PAGE_SIZE);

                        new_ptp_paddr = virt_to_phys((vaddr_t)new_ptp);

                        new_pte_val.pte = 0;
                        new_pte_val.table.is_valid = 1;
                        new_pte_val.table.is_table = 1;
                        new_pte_val.table.next_table_addr = new_ptp_paddr
                                                            >> PAGE_SHIFT;

                        /* same effect as: cur_ptp->ent[index] = new_pte_val; */
                        entry->pte = new_pte_val.pte;
                }
        }

        *next_ptp = (ptp_t *)GET_NEXT_PTP(entry);
        *pte = entry;
        if (IS_PTE_TABLE(entry->pte))
                return NORMAL_PTP;
        else
                return BLOCK_PTP;
}

int debug_query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry)
{
        ptp_t *l0_ptp, *l1_ptp, *l2_ptp, *l3_ptp;
        ptp_t *phys_page;
        pte_t *pte;
        int ret;

        // L0 page table
        l0_ptp = (ptp_t *)pgtbl;
        ret = get_next_ptp(l0_ptp, L0, va, &l1_ptp, &pte, false, NULL);
        if (ret < 0) {
                printk("[debug_query_in_pgtbl] L0 no mapping.\n");
                return ret;
        }
        printk("L0 pte is 0x%lx\n", pte->pte);

        // L1 page table
        ret = get_next_ptp(l1_ptp, L1, va, &l2_ptp, &pte, false, NULL);
        if (ret < 0) {
                printk("[debug_query_in_pgtbl] L1 no mapping.\n");
                return ret;
        }
        printk("L1 pte is 0x%lx\n", pte->pte);

        // L2 page table
        ret = get_next_ptp(l2_ptp, L2, va, &l3_ptp, &pte, false, NULL);
        if (ret < 0) {
                printk("[debug_query_in_pgtbl] L2 no mapping.\n");
                return ret;
        }
        printk("L2 pte is 0x%lx\n", pte->pte);

        // L3 page table
        ret = get_next_ptp(l3_ptp, L3, va, &phys_page, &pte, false, NULL);
        if (ret < 0) {
                printk("[debug_query_in_pgtbl] L3 no mapping.\n");
                return ret;
        }
        printk("L3 pte is 0x%lx\n", pte->pte);

        *pa = virt_to_phys((vaddr_t)phys_page) + GET_VA_OFFSET_L3(va);
        *entry = pte;
        return 0;
}

#ifdef CHCORE
void free_page_table(void *pgtbl)
{
        ptp_t *l0_ptp, *l1_ptp, *l2_ptp, *l3_ptp;
        pte_t *l0_pte, *l1_pte, *l2_pte;
        int i, j, k;

        if (pgtbl == NULL) {
                kwarn("%s: input arg is NULL.\n", __func__);
                return;
        }

        /* L0 page table */
        l0_ptp = (ptp_t *)pgtbl;

        /* Iterate each entry in the l0 page table*/
        for (i = 0; i < PTP_ENTRIES; ++i) {
                l0_pte = &l0_ptp->ent[i];
                if (IS_PTE_INVALID(l0_pte->pte))
                        continue;
                l1_ptp = (ptp_t *)GET_NEXT_PTP(l0_pte);

                /* Iterate each entry in the l1 page table*/
                for (j = 0; j < PTP_ENTRIES; ++j) {
                        l1_pte = &l1_ptp->ent[j];
                        if (IS_PTE_INVALID(l1_pte->pte))
                                continue;
                        l2_ptp = (ptp_t *)GET_NEXT_PTP(l1_pte);

                        /* Iterate each entry in the l2 page table*/
                        for (k = 0; k < PTP_ENTRIES; ++k) {
                                l2_pte = &l2_ptp->ent[k];
                                if (IS_PTE_INVALID(l2_pte->pte))
                                        continue;
                                l3_ptp = (ptp_t *)GET_NEXT_PTP(l2_pte);
                                /* Free the l3 page table page */
                                kfree(l3_ptp);
                        }

                        /* Free the l2 page table page */
                        kfree(l2_ptp);
                }

                /* Free the l1 page table page */
                kfree(l1_ptp);
        }

        kfree(l0_ptp);
}
#endif

/*
 * Translate a va to pa, and get its pte for the flags
 */
// 在页表 pgtbl 里搜索 va 的映射，返回对应的 pa 和 pte
// 返回值：
//  - 0 成功找到映射，*pa 和 *entry 已经被设置为对应的物理地址和页表项
//  - -ENOMAPPING 没有找到映射
int query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry)
{
        /* LAB 2 TODO 4 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * return the pa and pte until a L2/L3 block or page, return
         * `-ENOMAPPING` if the va is not mapped.
         */
        ptp_t *l0_ptp, *l1_ptp, *l2_ptp, *l3_ptp;
        ptp_t *phys_page;
        pte_t *pte;
        int ret;

        l0_ptp = (ptp_t *)pgtbl;

        ret = get_next_ptp(l0_ptp, L0, va, &l1_ptp, &pte, false, NULL);
        if (ret < 0)
                return ret;
        if (ret == BLOCK_PTP) {
                *pa = ((paddr_t)pte->l1_block.pfn << L1_INDEX_SHIFT)
                      + GET_VA_OFFSET_L1(va);
                *entry = pte;
                return 0;
        }

        ret = get_next_ptp(l1_ptp, L1, va, &l2_ptp, &pte, false, NULL);
        if (ret < 0)
                return ret;
        if (ret == BLOCK_PTP) {
                *pa = ((paddr_t)pte->l2_block.pfn << L2_INDEX_SHIFT)
                      + GET_VA_OFFSET_L2(va);
                *entry = pte;
                return 0;
        }

        ret = get_next_ptp(l2_ptp, L2, va, &l3_ptp, &pte, false, NULL);
        if (ret < 0)
                return ret;
        if (ret == BLOCK_PTP) {
                *pa = ((paddr_t)pte->l3_page.pfn << PAGE_SHIFT)
                      + GET_VA_OFFSET_L3(va);
                *entry = pte;
                return 0;
        }

        ret = get_next_ptp(l3_ptp, L3, va, &phys_page, &pte, false, NULL);
        if (ret < 0)
                return ret;

        *pa = virt_to_phys((vaddr_t)phys_page) + GET_VA_OFFSET_L3(va);
        *entry = pte;
        /* LAB 2 TODO 4 END */
        return 0;
}

// 在页表 pgtbl 里为 va 映射 pa，长度为 len，权限由 flags 指定
// kind: USER_PTE or KERNEL_PTE，决定了权限设置的细节
// rss: 如果不为 NULL，成功映射一个之前未映射的页时递增 *rss
// 返回值：
//  - 0 成功映射
//  - -ENOMEM 内存不足，无法分配新的页表页
static int map_range_in_pgtbl_common(void *pgtbl, vaddr_t va, paddr_t pa,
                                     size_t len, vmr_prop_t flags, int kind,
                                     __maybe_unused long *rss)
{
        /* LAB 2 TODO 4 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * create new page table page if necessary, fill in the final level
         * pte with the help of `set_pte_flags`. Iterate until all pages are
         * mapped.
         * Since we are adding new mappings, there is no need to flush TLBs.
         * Return 0 on success.
         */
        // 由于这里是添加映射，不涉及修改已有映射，这些映射本来就不在 TLB 里，因此不需要 flush TLB
        ptp_t *l1_ptp, *l2_ptp, *l3_ptp;
        pte_t *pte;
        int ret;
        vaddr_t cur_va = va;
        paddr_t cur_pa = pa;
        size_t n_pages = ROUND_UP(len, PAGE_SIZE) / PAGE_SIZE;

        // 简单实现：不根据 n_pages/len 数量决定在哪一级页表做 block mapping，而是直接映射到 L3 页表项
        for (size_t i = 0; i < n_pages; i++, cur_va +=PAGE_SIZE, cur_pa += PAGE_SIZE) {
                ptp_t *l0_ptp = (ptp_t *)pgtbl;

                ret = get_next_ptp(l0_ptp, L0, cur_va, &l1_ptp, &pte, true, rss);
                if (ret < 0)
                        return ret;
                ret = get_next_ptp(l1_ptp, L1, cur_va, &l2_ptp, &pte, true, rss);
                if (ret < 0)
                        return ret;
                ret = get_next_ptp(l2_ptp, L2, cur_va, &l3_ptp, &pte, true, rss);
                if (ret < 0)
                        return ret;

                /* Fill in the L3 page entry */
                pte_t *l3_entry = &l3_ptp->ent[GET_L3_INDEX(cur_va)];
                if (IS_PTE_INVALID(l3_entry->pte) && rss != NULL)
                        (*rss)++;
                l3_entry->pte = 0;
                l3_entry->l3_page.is_valid = 1;
                l3_entry->l3_page.is_page = 1;
                l3_entry->l3_page.pfn = cur_pa >> PAGE_SHIFT;
                set_pte_flags(l3_entry, flags, kind);
        }
        /* LAB 2 TODO 4 END */
        // 多核系统上，每个核各自有一套 MMU/TLB，但(可以)指向同一份页表
        // 每个 CPU 核有自己的 TTBR0_EL1/TTBR1_EL1、TLB、页表遍历硬件状态。
        // 页表本身在内存里，是数据结构；多个核把 TTBR 设成同一个基址时，就“共用页表”。
        // 操作系统常见做法：内核空间映射基本共享；用户空间映射按进程切换（不同核可运行不同进程，因此 TTBR 可不同）。
        // dsb(ishst)：Data Synchronization Barrier，作用域是 Inner Shareable，只约束 store。它保证前面所有页表写入（PTE 修改）在继续之前对其他核可见。
        // Inner Shareable = “这块内存对同一一致性域内的核共享，且屏障/可见性保证至少覆盖这些核”。
        // isb()：Instruction Synchronization Barrier，刷新后续指令执行上下文，让 CPU 之后取指/执行时看到最新的系统状态（比如页表相关状态变化）。
        // core0 刚往内存里写了一个新的页表项（PTE），
        // 其它 3 个核根本不知道这件事。
        // 会出现两个问题：
        // 内存一致性：新 PTE 可能还在 Cache 里，没写到内存，别的核看不见
        // TLB 一致性：别的核 TLB 里可能还缓存着旧映射 / 无效映射
        // CPU 流水线：本核后面的指令可能已经乱序取指 / 译码，用了旧地址
        // 内存一致性用 dsb 解决，CPU流水线用 isb 解决
        // 这个函数是添加映射，不是修改/移除已有映射，添加的映射本来就不在 TLB 里，因此不需要 flush TLB
        // ish: inner shareable
        // st: store， ishst 就是 store 的内存屏障，保证前面所有 store（页表修改）对同一 shareable 域内的核可见
        dsb(ishst);
        isb();
        return 0;
}

/* Map vm range in kernel */
int map_range_in_pgtbl_kernel(void *pgtbl, vaddr_t va, paddr_t pa, size_t len,
                              vmr_prop_t flags)
{
        return map_range_in_pgtbl_common(
                pgtbl, va, pa, len, flags, KERNEL_PTE, NULL);
}

/* Map vm range in user */
int map_range_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t pa, size_t len,
                       vmr_prop_t flags, __maybe_unused long *rss)
{
        return map_range_in_pgtbl_common(
                pgtbl, va, pa, len, flags, USER_PTE, rss);
}

/*
 * Try to release a lower level page table page (low_ptp).
 * @high_ptp: the higher level page table page
 * @low_ptp: the next level page table page
 * @index: the index of low_ptp in high ptp entries
 * @return:
 * 	- zero if lower page table page is not all empty
 * 	- nonzero otherwise
 */
static int try_release_ptp(ptp_t *high_ptp, ptp_t *low_ptp, int index,
                           __maybe_unused long *rss)
{
        int i;

        for (i = 0; i < PTP_ENTRIES; i++) {
                if (low_ptp->ent[i].pte != PTE_DESCRIPTOR_INVALID) {
                        return 0;
                }
        }

        BUG_ON(index < 0 || index >= PTP_ENTRIES);
        high_ptp->ent[index].pte = PTE_DESCRIPTOR_INVALID;
        kfree(low_ptp);

        return 1;
}

__maybe_unused static void recycle_pgtable_entry(ptp_t *l0_ptp, ptp_t *l1_ptp,
                                                 ptp_t *l2_ptp, ptp_t *l3_ptp,
                                                 vaddr_t va,
                                                 __maybe_unused long *rss)
{
        if (!try_release_ptp(l2_ptp, l3_ptp, GET_L2_INDEX(va), rss))
                return;

        if (!try_release_ptp(l1_ptp, l2_ptp, GET_L1_INDEX(va), rss))
                return;

        try_release_ptp(l0_ptp, l1_ptp, GET_L0_INDEX(va), rss);
}

// 在页表 pgtbl 里取消 va 的映射，长度为 len
// rss: 如果不为 NULL，成功取消一个之前映射的页时递减
int unmap_range_in_pgtbl(void *pgtbl, vaddr_t va, size_t len,
                         __maybe_unused long *rss)
{
        /* LAB 2 TODO 4 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * mark the final level pte as invalid. Iterate until all pages are
         * unmapped.
         * You don't need to flush tlb here since tlb is now flushed after
         * this function is called.
         * Return 0 on success.
         */
        // 调用这个函数的 caller 会 flush TLB，因此这里不需要 flush TLB
        ptp_t *l0_ptp, *l1_ptp, *l2_ptp, *l3_ptp;
        ptp_t *phys_page;
        pte_t *pte;
        int ret;
        vaddr_t cur_va = va;
        size_t n_pages = ROUND_UP(len, PAGE_SIZE) / PAGE_SIZE;

        l0_ptp = (ptp_t *)pgtbl;

        for (size_t i = 0; i < n_pages; i++, cur_va += PAGE_SIZE) {
                ret = get_next_ptp(l0_ptp, L0, cur_va, &l1_ptp, &pte, false, rss);
                if (ret < 0)
                        return ret;
                ret = get_next_ptp(l1_ptp, L1, cur_va, &l2_ptp, &pte, false, rss);
                if (ret < 0)
                        return ret;
                ret = get_next_ptp(l2_ptp, L2, cur_va, &l3_ptp, &pte, false, rss);
                if (ret < 0)
                        return ret;
                ret = get_next_ptp(l3_ptp, L3, cur_va, &phys_page, &pte, false, rss);
                if (ret < 0)
                        return ret;

                // 取消 L3 页表的映射
                if (!IS_PTE_INVALID(pte->pte)) {
                        pte->pte = PTE_DESCRIPTOR_INVALID;
                        if (rss != NULL)
                                *rss -= PAGE_SIZE;
                }

                // 看看 L2, L1, L0 页表页需不需要回收
                recycle_pgtable_entry(l0_ptp, l1_ptp, l2_ptp, l3_ptp, cur_va, rss);
        }
        /* LAB 2 TODO 4 END */

        dsb(ishst);
        isb();

        return 0;
}

// 在页表 pgtbl 里修改 va 的映射权限，长度为 len，权限由 flags 指定
// 返回值: 0 表示成功，负值表示失败
// 当前 chcore 假设内核页映射权限不变，只需要修改用户页映射权限
int mprotect_in_pgtbl(void *pgtbl, vaddr_t va, size_t len, vmr_prop_t flags)
{
        /* LAB 2 TODO 4 BEGIN */
        /*
         * Hint: Walk through each level of page table using `get_next_ptp`,
         * modify the permission in the final level pte using `set_pte_flags`.
         * The `kind` argument of `set_pte_flags` should always be `USER_PTE`.
         * Return 0 on success.
         */
        ptp_t *l0_ptp, *l1_ptp, *l2_ptp, *l3_ptp;
        ptp_t *phys_page;
        pte_t *pte;
        int ret;
        vaddr_t cur_va = va;
        size_t n_pages = ROUND_UP(len, PAGE_SIZE) / PAGE_SIZE;

        l0_ptp = (ptp_t *)pgtbl;

        // 找到 va 在 pgtbl 中的映射，修改对应的 PTE 权限
        for (size_t i = 0; i < n_pages; i++, cur_va += PAGE_SIZE) {
                ret = get_next_ptp(l0_ptp, L0, cur_va, &l1_ptp, &pte, false, NULL);
                if (ret < 0)
                        return ret;
                ret = get_next_ptp(l1_ptp, L1, cur_va, &l2_ptp, &pte, false, NULL);
                if (ret < 0)
                        return ret;
                ret = get_next_ptp(l2_ptp, L2, cur_va, &l3_ptp, &pte, false, NULL);
                if (ret < 0)
                        return ret;
                ret = get_next_ptp(l3_ptp, L3, cur_va, &phys_page, &pte, false, NULL);
                if (ret < 0)
                        return ret;

                set_pte_flags(pte, flags, USER_PTE);
        }
        /* LAB 2 TODO 4 END */
        return 0;
}

void parse_pte_to_common(pte_t *pte, unsigned int level,
                         struct common_pte_t *ret)
{
        switch (level) {
        case L3:
                ret->ppn = pte->l3_page.pfn;
                ret->perm = 0;
                ret->_unused = 0;
                ret->perm |= (pte->l3_page.UXN ? 0 : VMR_EXEC);
                ret->perm |= __ap_to_vmr_prot(pte->l3_page.AP);

                ret->perm |= (pte->l3_page.attr_index == DEVICE_MEMORY ?
                                      VMR_DEVICE :
                                      0);

                ret->access = pte->l3_page.AF;
                ret->dirty = pte->l3_page.DBM;
                ret->valid = pte->l3_page.is_valid;
                break;
        default:
                BUG("parse upper level PTEs is not supported now!\n");
        }
}

void update_pte(pte_t *dest, unsigned int level, struct common_pte_t *src)
{
        switch (level) {
        case L3:
                dest->l3_page.pfn = src->ppn;
                dest->l3_page.AP = __vmr_prot_to_ap(src->perm);

                dest->l3_page.UXN = ((src->perm & VMR_EXEC) ?
                                             AARCH64_MMU_ATTR_PAGE_UX :
                                             AARCH64_MMU_ATTR_PAGE_UXN);

                dest->l3_page.is_valid = src->valid;
#if !(defined(CHCORE_PLAT_RASPI3) || defined(CHCORE_PLAT_RASPI4) \
      || defined(CHCORE_PLAT_FT2000))
                /**
                 * Some platforms do not support setting AF and DBM
                 * by hardware, so on these platforms we ignored them.
                 */
                dest->l3_page.AF = src->access;
                dest->l3_page.DBM = src->dirty;
#endif
                break;
        default:
                BUG("update upper level PTEs is not supported now!\n");
        }
}
