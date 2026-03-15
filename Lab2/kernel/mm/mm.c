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

#include <mm/mm.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <mm/slab.h>
#include <mm/buddy.h>
#include <arch/mmu.h>

/* The following two will be filled by parse_mem_map. */
paddr_t physmem_map[N_PHYS_MEM_POOLS][2];
int physmem_map_num;

/*
 * 每个元素对应一段连续物理内存的伙伴系统管理器；
 * mm_init() 会根据 physmem_map 的解析结果初始化这些内存池，后续物理页分配与统计都通过它们完成。
 * phys_mem_pool 的数量和 physmem_map_num 是对应的，每段连续物理内存对应一个 phys_mem_pool 实例。
 */
struct phys_mem_pool global_mem[N_PHYS_MEM_POOLS];

/*
 * The layout of each physmem:
 * | metadata (npages * sizeof(struct page)) | start_vaddr ... (npages *
 * PAGE_SIZE) |
 * 每一段物理内存的布局是：
 * 开头是一个元数据区域，大小为 npages * sizeof(struct page)，表示每个物理页的元信息
 * 后面是实际可用的物理内存区域，大小为 npages * PAGE_SIZE，表示每个物理页的大小
 */
static void init_buddy_for_one_physmem_map(int physmem_map_idx)
{
        // 该函数做的事情：
        // 1.计算这段连续物理内存能支撑多少个物理页
        // 2.计算这段连续物理内存的元数据区域的起始地址，和实际可用物理内存区域的起始地址
        // 注意：后续内核实际要用的是它们的虚拟地址，所以要进行物理地址到虚拟地址的转换
        // 3.调用 init_buddy 函数，初始化这段连续物理内存的伙伴系统管理器实例，
        // 参数包括：
        // 1.管理这段连续物理内存的伙伴系统管理器实例
        // 2.元数据区域的起始地址 (虚拟地址)
        // 3.实际可用物理内存区域的起始地址 (虚拟地址)
        // 4.实际可用物理页的数量
        paddr_t free_mem_start = 0;
        paddr_t free_mem_end = 0;
        struct page *page_meta_start = NULL;
        unsigned long npages = 0;
        unsigned long npages1 = 0;
        paddr_t free_page_start = 0;

        // parse_mem_map 修改了全局变量 physmem_map，physmem_map 是一个二维数组
        free_mem_start = physmem_map[physmem_map_idx][0];
        free_mem_end = physmem_map[physmem_map_idx][1];
        kdebug("mem pool %d, free_mem_start: 0x%lx, free_mem_end: 0x%lx\n",
               physmem_map_idx,
               free_mem_start,
               free_mem_end);
#ifdef KSTACK_BASE
        /* KSTACK_BASE should not locate in free_mem_start ~ free_mem_end */
        BUG_ON(KSTACK_BASE >= phys_to_virt(free_mem_start) && KSTACK_BASE < phys_to_virt(free_mem_end));
#endif
        npages = (free_mem_end - free_mem_start)
                 / (PAGE_SIZE + sizeof(struct page));
        free_page_start = ROUND_UP(
                free_mem_start + npages * sizeof(struct page), PAGE_SIZE);

        /* Recalculate npages after alignment. */
        npages1 = (free_mem_end - free_page_start) / PAGE_SIZE;
        npages = npages < npages1 ? npages : npages1;

        page_meta_start = (struct page *)phys_to_virt(free_mem_start);
        kdebug("page_meta_start: 0x%lx, npages: 0x%lx, meta_size: 0x%lx, free_page_start: 0x%lx\n",
               page_meta_start,
               npages,
               sizeof(struct page),
               free_page_start);

        /* Initialize the buddy allocator based on this free memory region. */
        // 初始化这段连续物理内存的伙伴系统管理器实例，参数包括：
        // arg1.管理这段连续物理内存的伙伴系统管理器实例
        // arg2.元数据区域的起始地址 (虚拟地址)
        // arg3.实际可用物理内存区域的起始地址 (虚拟地址)
        // arg4.实际可用物理页的数量
        init_buddy(&global_mem[physmem_map_idx],
                   page_meta_start,
                   phys_to_virt(free_page_start),
                   npages);
}

void mm_init(void *physmem_info)
{
        int physmem_map_idx;

        /* Step-1: parse the physmem_info to get each continuous range of the
         * physmem. */
        physmem_map_num = 0;
        // 架构相关函数，告知操作系统硬件上的物理内存布局，物理内存的起始地址和结束地址等信息
        // parse_mem_map 函数会将物理内存的起始地址和结束地址存储在全局变量 physmem_map 中，
        // physmem_map 是一个二维数组，
        // 每一行表示一个连续的物理内存区域，第一列是起始地址，第二列是结束地址
        // parse_mem_map 函数还会更新全局变量 physmem_map_num，表示物理内存区域的数量
        parse_mem_map(physmem_info);

        /* Step-2: init the buddy allocators for each continuous range of the
         * physmem. */
        // 针对解析出来的每段物理内存，进行伙伴系统的初始化，伙伴系统是一种内存分配算法，用于管理物理内存的分配和回收
        for (physmem_map_idx = 0; physmem_map_idx < physmem_map_num;
             ++physmem_map_idx)
                init_buddy_for_one_physmem_map(physmem_map_idx);

        /* Step-3: init the slab allocator. */
        init_slab();
}

unsigned long get_free_mem_size(void)
{
        unsigned long size;
        int i;

        size = get_free_mem_size_from_slab();
        for (i = 0; i < physmem_map_num; ++i)
                size += get_free_mem_size_from_buddy(&global_mem[i]);

        return size;
}

unsigned long get_total_mem_size(void)
{
        unsigned long size = 0;
        int i;

        for (i = 0; i < physmem_map_num; ++i)
                size += get_total_mem_size_from_buddy(&global_mem[i]);

        return size;
}
