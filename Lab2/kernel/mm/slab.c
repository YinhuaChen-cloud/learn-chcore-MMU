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

#include <common/macro.h>
#include <common/types.h>
#include <common/kprint.h>
#include <common/lock.h>
#include <common/debug.h>
#include <mm/kmalloc.h>
#include <mm/slab.h>
#include <mm/buddy.h>

/* slab_pool is also static. We do not add the static modifier due to unit test.
 */
// 每个 ORDER 对应一个 slab_pool，包含一个 current slab 和一个 partial slab 链表
struct slab_pointer slab_pool[SLAB_MAX_ORDER + 1];
static struct lock slabs_locks[SLAB_MAX_ORDER + 1];

/*
static inline int order_to_index(int order)
{
        return order - SLAB_MIN_ORDER;
}
*/

static inline int size_to_order(unsigned long size)
{
        unsigned long order = 0;
        unsigned long tmp = size;

        while (tmp > 1) {
                tmp >>= 1;
                order += 1;
        }
        if (size > (1 << order))
                order += 1;

        return (int)order;
}

static inline unsigned long order_to_size(int order)
{
        return 1UL << order;
}

/* @set_or_clear: true for set and false for clear. */
static void set_or_clear_slab_in_page(void *addr, unsigned long size,
                                      bool set_or_clear)
{
        struct page *page;
        int order;
        unsigned long page_num;
        int i;
        void *page_addr;

        order = size_to_order(size / BUDDY_PAGE_SIZE);
        page_num = order_to_size(order);

        /* Set/Clear the `slab` field in each struct page. */
        for (i = 0; i < page_num; i++) {
                page_addr = (void *)((unsigned long)addr + i * BUDDY_PAGE_SIZE);
                page = virt_to_page(page_addr);
                if (!page) {
                        kdebug("invalid page in %s", __func__);
                        return;
                }
                if (set_or_clear)
                        page->slab = addr;
                else
                        page->slab = NULL;
        }
}

static void *alloc_slab_memory(unsigned long size)
{
        void *addr;
        int order;

        /* Allocate memory pages from the buddy system as a new slab. */
        order = size_to_order(size / BUDDY_PAGE_SIZE);
        addr = _get_pages(order, false);

        if (unlikely(addr == NULL)) {
                kwarn("%s failed due to out of memory\n", __func__);
                return NULL;
        }

        set_or_clear_slab_in_page(addr, size, true);

        return addr;
}

static struct slab_header *init_slab_cache(int order, int size)
{
        void *addr;
        struct slab_slot_list *slot;
        struct slab_header *slab;
        unsigned long cnt, obj_size;
        int i;

        /*
         * 初始化一个新的 slab：
         * 1) 从 buddy 分配一整块 slab 内存；
         * 2) 将首个对象槽作为 slab_header 元数据；
         * 3) 将其余对象槽串成单向空闲链表。
         */
        addr = alloc_slab_memory(size);
        if (unlikely(addr == NULL))
                /* Fail: no available memory. */
                return NULL;
        slab = (struct slab_header *)addr;

        /* 单个对象槽大小（2^order）。 */
        obj_size = order_to_size(order);
        /* The first slot is used as metadata (struct slab_header). */
        BUG_ON(obj_size == 0);

        /* 可分配槽位总数 = slab 总大小 / 槽大小 - 1（扣除 header 槽）。 */
        cnt = size / obj_size - 1;

        /* 第一个可用槽紧跟在 header 槽之后。 */
        slot = (struct slab_slot_list *)((vaddr_t)addr + obj_size);
        slab->free_list_head = (void *)slot;
        slab->order = order;
        slab->total_free_cnt = cnt;
        slab->current_free_cnt = cnt;

        /* 依次链接每个空闲槽，形成 free list。 */
        for (i = 0; i < cnt - 1; ++i) {
                slot->next_free = (void *)((unsigned long)slot + obj_size);
                slot = (struct slab_slot_list *)((unsigned long)slot
                                                 + obj_size);
        }
        /* 空闲链表末尾置空。 */
        slot->next_free = NULL;

        return slab;
}

// 从 partial slab 链表中选择一个 slab 作为新的 current slab，并将其从 partial slab 链表中删除
static void choose_new_current_slab(struct slab_pointer * __maybe_unused pool)
{
        /* LAB 2 TODO 2 BEGIN */
        /* Hint: Choose a partial slab to be a new current slab. */
        struct slab_header *new_current_slab;
        // 如果 partial slab 链表是空的，说明没有可用的 slab 了，将 current slab 置为 NULL
        if (list_empty(&pool->partial_slab_list)) {
                // 问题: 这里不需要使用 buddy 分配新的 slab 吗？
                // 回答: 因为在 alloc_in_slab_impl 中，
                // 如果 current slab 是 NULL，
                // 就会调用 init_slab_cache 来分配新的 slab，所以这里不需要再分配一次了。
                pool->current_slab = NULL;
                return;
        }
        // 从 partial slab 链表中选择一个 slab 作为新的 current slab，并将其从 partial slab 链表中删除
        new_current_slab = list_entry(pool->partial_slab_list.next,
                                      struct slab_header,
                                      node);
        list_del(&new_current_slab->node);
        pool->current_slab = new_current_slab;
        /* LAB 2 TODO 2 END */
}

// 从 current slab 的 free list 中分配一个 slot，如果 current slab 已经没有 free slot 了，
// 就从 partial slab 链表中选择一个 slab 作为新的 current slab，并继续分配
// order: 5, 6, 7, 8, 9, 10, 11    表示分配 32, 64, 128, 256, 512, 1024, 2048 字节的内存
// 分配失败返回 NULL，分配成功返回分配的内存地址
static void *alloc_in_slab_impl(int order)
{
        struct slab_header *current_slab;
        struct slab_slot_list *free_list;
        void *next_slot;

        // 给对应 order 的 slab_pool 加锁，保证线程安全
        lock(&slabs_locks[order]);

        current_slab = slab_pool[order].current_slab;
        /* When serving the first allocation request. */
        // 如果是第一次分配，current slab 还没有被分配出来，就调用 init_slab_cache 来分配一个新的 slab，
        // 并将其设置为 current slab
        if (unlikely(current_slab == NULL)) {
                current_slab = init_slab_cache(order, SIZE_OF_ONE_SLAB);
                if (current_slab == NULL) {
                        unlock(&slabs_locks[order]);
                        return NULL;
                }
                slab_pool[order].current_slab = current_slab;
        }
        // 如果不是第一次分配，说明 current slab 已经被分配出来了，
        // 就直接从 current slab 的 free list 中分配一个 slot

        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Find a free slot from the free list of current slab.
         * If current slab is full, choose a new slab as the current one.
         */
        free_list = current_slab->free_list_head;
        // 如果当前 slot 的 free list 为空，说明 current slab 已经没有 free slot 了，
        if (unlikely(free_list == NULL)) {
                // 从 partial slab 链表中选择一个 slab 作为新的 current slab，并继续分配
                choose_new_current_slab(&slab_pool[order]);
                current_slab = slab_pool[order].current_slab;
                // 如果 partial slab 链表中也没有可用的 slab 了，那么 current slab == NULL
                // 就调用 init_slab_cache 来分配一个新的 slab，并将其设置为 current slab
                if (unlikely(current_slab == NULL)) {
                        current_slab = init_slab_cache(order,
                                                       SIZE_OF_ONE_SLAB);
                        // 如果分配失败，说明没有可用的内存了，返回 NULL
                        if (current_slab == NULL) {
                                unlock(&slabs_locks[order]);
                                return NULL;
                        }
                        slab_pool[order].current_slab = current_slab;
                }
                free_list = current_slab->free_list_head;
        }
        // 如果当前 slot 的 free list 不为空，说明 current slab 还有 free slot，可以直接分配
        next_slot = free_list->next_free;
        current_slab->free_list_head = next_slot;
        current_slab->current_free_cnt -= 1;
        /* LAB 2 TODO 2 END */

        unlock(&slabs_locks[order]);

        return (void *)free_list;
}

#if ENABLE_DETECTING_DOUBLE_FREE_IN_SLAB == ON
static int check_slot_is_free(struct slab_header *slab_header,
                              struct slab_slot_list *slot)
{
        struct slab_slot_list *cur_slot;
        struct slab_header *next_slab;

        cur_slot = (struct slab_slot_list *)(slab_header->free_list_head);

        while (cur_slot != NULL) {
                if (cur_slot == slot)
                        return 1;
                cur_slot = (struct slab_slot_list *)cur_slot->next_free;
        }

        return 0;
}
#endif

static void try_insert_full_slab_to_partial(struct slab_header *slab)
{
        /* @slab is not a full one. */
        if (slab->current_free_cnt != 0)
                return;

        int order;
        order = slab->order;

        list_append(&slab->node, &slab_pool[order].partial_slab_list);
}

static void try_return_slab_to_buddy(struct slab_header *slab, int order)
{
        /* The slab is whole free now. */
        if (slab->current_free_cnt != slab->total_free_cnt)
                return;

        if (slab == slab_pool[order].current_slab)
                choose_new_current_slab(&slab_pool[order]);
        else
                list_del(&slab->node);

        /* Clear the slab field in the page structures before freeing them. */
        set_or_clear_slab_in_page(slab, SIZE_OF_ONE_SLAB, false);
        free_pages_without_record(slab);
}

/* Interfaces exported to the kernel/mm module */

void init_slab(void)
{
        int order;

        // 32 bytes ----> 2KB
        /* slab obj size: 32, 64, 128, 256, 512, 1024, 2048 */
        for (order = SLAB_MIN_ORDER; order <= SLAB_MAX_ORDER; order++) {
                lock_init(&slabs_locks[order]);
                // 初始时每个 order 的 current slab 都是 NULL，表示还没有 slab 被分配出来
                slab_pool[order].current_slab = NULL;
                // 初始化每个 order 的 partial slab 链表，初始时每个链表都是空的
                init_list_head(&(slab_pool[order].partial_slab_list));
        }
        kdebug("mm: finish initing slab allocators\n");
}

void *alloc_in_slab(unsigned long size, size_t *real_size)
{
        int order;

        BUG_ON(size > order_to_size(SLAB_MAX_ORDER));

        order = (int)size_to_order(size);
        if (order < SLAB_MIN_ORDER)
                order = SLAB_MIN_ORDER;

#if ENABLE_MEMORY_USAGE_COLLECTING == ON
        if (real_size)
                *real_size = 1 << order;
#endif

        return alloc_in_slab_impl(order);
}

// 释放一个 slot，首先需要找到这个 slot 所在的 slab,
// 然后将这个 slot 插入到 slab 的 free list 中，
void free_in_slab(void *addr)
{
        struct page *page;
        struct slab_header *slab;
        struct slab_slot_list *slot;
        int order;

        slot = (struct slab_slot_list *)addr;
        page = virt_to_page(addr);
        if (!page) {
                kdebug("invalid page in %s", __func__);
                return;
        }

        slab = page->slab;
        order = slab->order;
        lock(&slabs_locks[order]);

        // 释放一个 slot 后，如果这个 slab 原来是 full 的，那么需要将这个 slab 插入到 partial slab 链表中，
        // 因为这个 slab 现在有 free slot 了，可以被分配了
        try_insert_full_slab_to_partial(slab);

#if ENABLE_DETECTING_DOUBLE_FREE_IN_SLAB == ON
        /*
         * SLAB double free detection: check whether the slot to free is
         * already in the free list.
         */
        if (check_slot_is_free(slab, slot) == 1) {
                kinfo("SLAB: double free detected. Address is %p\n",
                      (unsigned long)slot);
                BUG_ON(1);
        }
#endif

        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Free an allocated slot and put it back to the free list.
         */
        // 加到表头
        slot->next_free = slab->free_list_head;
        slab->free_list_head = slot;
        slab->current_free_cnt += 1;
        /* LAB 2 TODO 2 END */

        // 如果这个 slab 现在是 whole free 的了，说明这个 slab 里面的所有 slot 都没有被分配出去了，
        // 就可以将这个 slab 还给 buddy 系统了，释放掉这个 slab
        try_return_slab_to_buddy(slab, order);

        unlock(&slabs_locks[order]);
}

/* This interface is not marked as static because it is needed in the unit test.
 */
unsigned long get_free_slot_number(int order)
{
        struct slab_header *slab;
        struct slab_slot_list *slot;
        unsigned long current_slot_num = 0;
        unsigned long check_slot_num = 0;

        lock(&slabs_locks[order]);

        slab = slab_pool[order].current_slab;
        if (slab) {
                slot = (struct slab_slot_list *)slab->free_list_head;
                while (slot != NULL) {
                        current_slot_num++;
                        slot = slot->next_free;
                }
                check_slot_num += slab->current_free_cnt;
        }

        if (list_empty(&slab_pool[order].partial_slab_list))
                goto out;

        for_each_in_list (slab,
                          struct slab_header,
                          node,
                          &slab_pool[order].partial_slab_list) {
                slot = (struct slab_slot_list *)slab->free_list_head;
                while (slot != NULL) {
                        current_slot_num++;
                        slot = slot->next_free;
                }
                check_slot_num += slab->current_free_cnt;
        }

out:
        unlock(&slabs_locks[order]);

        BUG_ON(check_slot_num != current_slot_num);

        return current_slot_num;
}

/* Get the size of free memory in slab */
unsigned long get_free_mem_size_from_slab(void)
{
        int order;
        unsigned long current_slot_size;
        unsigned long slot_num;
        unsigned long total_size = 0;

        for (order = SLAB_MIN_ORDER; order <= SLAB_MAX_ORDER; order++) {
                current_slot_size = order_to_size(order);
                slot_num = get_free_slot_number(order);
                total_size += (current_slot_size * slot_num);

                kdebug("slab memory chunk size : 0x%lx, num : %d\n",
                       current_slot_size,
                       slot_num);
        }

        return total_size;
}
