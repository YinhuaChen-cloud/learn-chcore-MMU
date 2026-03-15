/*
 * Copyright (c) 2023 Institute of Parallel And Distributed Systems (IPADS),
 * Shanghai Jiao Tong University (SJTU) Licensed under the Mulan PSL v2. You can
 * use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 * KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the
 * Mulan PSL v2 for more details.
 */

#include <common/util.h>
#include <common/macro.h>
#include <common/kprint.h>
#include <mm/buddy.h>

// 返回伙伴块的指针，如果伙伴块不存在或者不满足合并条件，则返回 NULL
// 只返回伙伴块的指针，不对伙伴块进行任何修改
__maybe_unused static struct page *get_buddy_chunk(struct phys_mem_pool *pool,
                                                   struct page *chunk)
{
        vaddr_t chunk_addr;
        vaddr_t buddy_chunk_addr;
        int order;

        /* Get the address of the chunk. */
        chunk_addr = (vaddr_t)page_to_virt(chunk);
        order = chunk->order;
        /*
         * Calculate the address of the buddy chunk according to the address
         * relationship between buddies.
         */
        buddy_chunk_addr = chunk_addr
                           ^ (1UL << (order + BUDDY_PAGE_SIZE_ORDER));

        /* Check whether the buddy_chunk_addr belongs to pool. */
        if ((buddy_chunk_addr < pool->pool_start_addr)
            || ((buddy_chunk_addr + (1 << order) * BUDDY_PAGE_SIZE)
                > (pool->pool_start_addr + pool->pool_mem_size))) {
                return NULL;
        }

        return virt_to_page((void *)buddy_chunk_addr);
}

/* The most recursion level of split_chunk is decided by the macro of
 * BUDDY_MAX_ORDER. */
// pool: 内存池实例
// order: 需要的块的 order
// chunk: 当前块
// 返回分割后的块的指针，如果不能分割，则返回 chunk 本身
// 注意：哪怕成功切割，返回的也得是 chunk 不能是 buddy，不然会提高 caller(调用这个函数的函数) 实现的复杂度
__maybe_unused static struct page *split_chunk(struct phys_mem_pool *__maybe_unused pool,
                                int __maybe_unused order,
                                struct page *__maybe_unused chunk)
{
        /* LAB 2 TODO 1 BEGIN */
        /*
         * Hint: Recursively put the buddy of current chunk into
         * a suitable free list.
         */
        struct page *buddy;

        BUG_ON(chunk->order < order);

        if (chunk->order == order)
                return chunk;

        // 切割当前块
        chunk->order--;
        buddy = get_buddy_chunk(pool, chunk);
        BUG_ON(buddy == NULL);

        // 把伙伴块加入到对应 order 的空闲链表中
        buddy->order = chunk->order;
        buddy->allocated = 0;
        list_add(&buddy->node, &pool->free_lists[buddy->order].free_list);
        pool->free_lists[buddy->order].nr_free++;

        // 看看能否继续切割
        return split_chunk(pool, order, chunk);
        /* LAB 2 TODO 1 END */
}

/* The most recursion level of merge_chunk is decided by the macro of
 * BUDDY_MAX_ORDER. */
// 返回合并后的块的指针，如果不能合并，则返回 chunk 本身
// 这里不能合并不能直接返回 NULL，因为：
// 1.这里使用递归方法尽量合并大块，有可能合并到一半就不能继续合并了，这时应该返回当前合并到的块，而不是 NULL
__maybe_unused static struct page * merge_chunk(struct phys_mem_pool *__maybe_unused pool,
                                struct page *__maybe_unused chunk)
{
        // 注意：该函数假设传入的 chunk 是一个空闲块，且已经被从原来的 free list 中摘掉了
        /* LAB 2 TODO 1 BEGIN */
        /*
         * Hint: Recursively merge current chunk with its buddy
         * if possible.
         */
        struct page *buddy = NULL;  // 指向找到的伙伴块
        struct page *merged = NULL; // 指向合并后的块

        /* Cannot merge beyond max order */
        BUG_ON(chunk->order > BUDDY_MAX_ORDER - 1);
        if (chunk->order == BUDDY_MAX_ORDER - 1)
                return chunk;

        buddy = get_buddy_chunk(pool, chunk);

        /* Buddy must exist, be free, and be the same order */
        if (buddy == NULL || buddy->allocated != 0)
                return chunk;
        // 我个人认为这里不可能有伙伴块的 order 不同的情况，
        // 因为只有 order 相同的块才可能是伙伴关系，才能合并
        BUG_ON(buddy->order != chunk->order);

        /* Remove buddy from its free list */
        list_del(&buddy->node);
        pool->free_lists[buddy->order].nr_free--;

        /* The merged block starts at the lower address */
        merged = (buddy < chunk) ? buddy : chunk;
        merged->order++;

        // 注意：此时 merged 还没有从原来的 free list 中删除
        // 调用 merge_chunk 的函数需要调整链表
        return merge_chunk(pool, merged);
        /* LAB 2 TODO 1 END */
}

/*
 * The layout of a phys_mem_pool:
 * | page_metadata are (an array of struct page) | alignment pad | usable memory
 * |
 *
 * The usable memory: [pool_start_addr, pool_start_addr + pool_mem_size).
 */
// arg1.管理这段连续物理内存的伙伴系统管理器实例
// arg2.元数据区域的起始地址 (虚拟地址)
// arg3.实际可用物理内存区域的起始地址 (虚拟地址)
// arg4.实际可用物理页的数量
void init_buddy(struct phys_mem_pool *pool, struct page *start_page,
                vaddr_t start_addr, unsigned long page_num)
{
        int order;
        int page_idx;
        struct page *page;

        // 初始化伙伴系统管理器实例的锁，伙伴系统管理器实例中的 lock 
        // 用于保护伙伴系统管理器实例中对物理内存分配和回收相关数据结构的访问，
        // 保证在多核环境下对这些数据结构的访问是线程安全的
        BUG_ON(lock_init(&pool->buddy_lock) != 0);

        /* Init the physical memory pool. */
        pool->pool_start_addr = start_addr; // 实际可用物理内存区域的起始地址 (虚拟地址)
        pool->page_metadata = start_page; // 元数据区域的起始地址 (虚拟地址)
        // 实际可用物理内存区域的大小 (字节)，等于实际可用物理页的数量 * 每个物理页的大小
        pool->pool_mem_size = page_num * BUDDY_PAGE_SIZE; 
        /* This field is for unit test only. */
        pool->pool_phys_page_num = page_num;

        /* Init the free lists */
        // 初始化伙伴系统管理器实例中不同 order 的空闲链表，初始时每个空闲链表都是空的，空闲块数量为 0
        // 管理方式：链表数组，数组的每个元素是指针，指向链表头，链表中每个节点是一个空闲块
        for (order = 0; order < BUDDY_MAX_ORDER; ++order) {
                pool->free_lists[order].nr_free = 0;
                // 链表初始状态：链表头的 next 和 prev 都指向链表头自己，表示链表为空
                init_list_head(&(pool->free_lists[order].free_list));
        }

        /* Clear the page_metadata area. */
        // 清空这段连续物理内存的元数据区域
        memset((char *)start_page, 0, page_num * sizeof(struct page));

        /* Init the page_metadata area. */
        // 初始化这段连续物理内存的元数据区域
        // 一开始假设所有物理内存都分配出去了，所以把它们都标记为 allocated = 1
        // 由于都分配出去了，所有物理页 order = 0，表示每个物理页都是一个独立的块
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                page->allocated = 1;
                page->order = 0;
                page->pool = pool;
        }

        /* Put each physical memory page into the free lists. */
        // 通过释放物理页的方式，把物理页插回到伙伴系统管理器实例中，初始时所有物理页都是独立的块，所以 order = 0
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                buddy_free_pages(pool, page);
        }
}

// 从内存池中获取一个满足 order 要求的块(连续内存块)
// 返回该块的 page 结构体指针，如果没有满足要求的块，则返回 NULL
struct page *buddy_get_pages(struct phys_mem_pool *pool, int order)
{
        int cur_order;
        struct list_head *free_list;
        struct page *page = NULL;

        if (unlikely(order >= BUDDY_MAX_ORDER)) {
                kwarn("ChCore does not support allocating such too large "
                      "continuous physical memory\n");
                return NULL;
        }

        lock(&pool->buddy_lock);

        /* LAB 2 TODO 1 BEGIN */
        /*
         * Hint: Find a chunk that satisfies the order requirement
         * in the free lists, then split it if necessary.
         */
        // 从小到大遍历不同 order 的空闲链表，找到第一个满足要求的块(可以更大的块)
        for (cur_order = order; cur_order < BUDDY_MAX_ORDER; ++cur_order) {
                free_list = &pool->free_lists[cur_order].free_list;
                if (!list_empty(free_list)) {
                        page = list_entry(free_list->next, struct page, node);
                        list_del(&page->node);
                        pool->free_lists[cur_order].nr_free--;
                        break;
                }
        }

        if (page == NULL)
                goto out;

        // 表示这个大页块已经被分配出去了，不在任何 free list 里了
        page->allocated = 1;
        // 哪怕成功切分，依然会返回 page 而不是 buddy，减少 caller 的实现复杂度
        // 所以这里不需要重新给 allocated 赋值
        page = split_chunk(pool, order, page);
        /* LAB 2 TODO 1 END */
out: __maybe_unused
        unlock(&pool->buddy_lock);
        return page;
}

void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
        int order;
        struct list_head *free_list;
        lock(&pool->buddy_lock);

        /* LAB 2 TODO 1 BEGIN */
        /*
         * Hint: Merge the chunk with its buddy and put it into
         * a suitable free list.
         */
        // 此时这个 page 原本是已分配状态，不在任何 free list 里，
        // 所以不需要先从链表摘掉它。
        // 释放该页
        page->allocated = 0;
        /* Merge with buddy recursively to get the largest possible free block */
        page = merge_chunk(pool, page);
        order = page->order;
        // 把合并后的块插回到对应 order 的空闲链表中
        free_list = &pool->free_lists[order].free_list;
        pool->free_lists[order].nr_free++;
        list_add(&page->node, free_list);
        /* LAB 2 TODO 1 END */

        unlock(&pool->buddy_lock);
}

void *page_to_virt(struct page *page)
{
        vaddr_t addr;
        struct phys_mem_pool *pool = page->pool;

        BUG_ON(pool == NULL);

        /* page_idx * BUDDY_PAGE_SIZE + start_addr */
        addr = (page - pool->page_metadata) * BUDDY_PAGE_SIZE
               + pool->pool_start_addr;
        return (void *)addr;
}

struct page *virt_to_page(void *ptr)
{
        struct page *page;
        struct phys_mem_pool *pool = NULL;
        vaddr_t addr = (vaddr_t)ptr;
        int i;

        /* Find the corresponding physical memory pool. */
        for (i = 0; i < physmem_map_num; ++i) {
                if (addr >= global_mem[i].pool_start_addr
                    && addr < global_mem[i].pool_start_addr
                                       + global_mem[i].pool_mem_size) {
                        pool = &global_mem[i];
                        break;
                }
        }

        if (pool == NULL) {
                kdebug("invalid pool in %s", __func__);
                return NULL;
        }

        page = pool->page_metadata
               + (((vaddr_t)addr - pool->pool_start_addr) / BUDDY_PAGE_SIZE);
        return page;
}

unsigned long get_free_mem_size_from_buddy(struct phys_mem_pool *pool)
{
        int order;
        struct free_list *list;
        unsigned long current_order_size;
        unsigned long total_size = 0;

        for (order = 0; order < BUDDY_MAX_ORDER; order++) {
                /* 2^order * 4K */
                current_order_size = BUDDY_PAGE_SIZE * (1 << order);
                list = pool->free_lists + order;
                total_size += list->nr_free * current_order_size;

                /* debug : print info about current order */
                kdebug("buddy memory chunk order: %d, size: 0x%lx, num: %d\n",
                       order,
                       current_order_size,
                       list->nr_free);
        }
        return total_size;
}

unsigned long get_total_mem_size_from_buddy(struct phys_mem_pool *pool)
{
        return pool->pool_mem_size;
}
