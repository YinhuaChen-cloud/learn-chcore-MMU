/*
 * Copyright (c) 2023 Institute of Parallel And Distributed Systems (IPADS)
 * ChCore-Lab is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v1. You may obtain a copy of Mulan PSL v1 at:
 *     http://license.coscl.org.cn/MulanPSL
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 * KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the
 * Mulan PSL v1 for more details.
 */

// 包含映像头文件，定义内核基地址KERNEL_VADDR和其他启动相关常量
#include "image.h"

// 64位无符号整数类型别名，用于ARMv8 64位页表项
typedef unsigned long u64;
// 32位无符号整数类型别名
typedef unsigned int u32;

/* Physical memory address space: 0-1G */
// Raspberry Pi 3物理内存地址空间定义（0-1GB）
// 物理内存起始地址：0x0（DRAM开始位置）
#define PHYSMEM_START   (0x0UL)
// 外设基地址：0x3F000000（GPIO、UART等设备寄存器起始）
#define PERIPHERAL_BASE (0x3F000000UL)
// 物理内存结束地址：0x40000000（1GB边界）
#define PHYSMEM_END     (0x40000000UL)

/* The number of entries in one page table page */
// 页表页面中的条目数：512个（2^9，ARMv8使用9位来索引）
// 单个页表页面包含512个64位条目 (8字节)
#define PTP_ENTRIES 512
/* The size of one page table page */
// 页表页面大小：4096字节（4KB）
// 大小计算：512条目 × 8字节/条目 = 4096字节
#define PTP_SIZE 4096
// GCC对齐属性宏：指定数据在内存中按n字节边界对齐
// 在 ARMv8 页表中，页表基址寄存器（TTBR）的低 12 位必须
// 为 0（因为页大小是 4096 = 2^12）。
// 这要求页表基地址必须4KB对齐
#define ALIGN(n) __attribute__((__aligned__(n)))
// TTBR0_EL1的L0级页表（第0级：地址范围0-512GB）
// 用于低地址空间映射（用户空间，虚拟地址0-2^48）
// 512个条目，每个条目8字节，总4096字节，按4K对齐
u64 boot_ttbr0_l0[PTP_ENTRIES] ALIGN(PTP_SIZE);
// TTBR0_EL1的L1级页表（第1级：地址范围0-1GB）
u64 boot_ttbr0_l1[PTP_ENTRIES] ALIGN(PTP_SIZE);
// TTBR0_EL1的L2级页表（第2级：地址范围0-2MB）
u64 boot_ttbr0_l2[PTP_ENTRIES] ALIGN(PTP_SIZE);
// 目前还没有 L3 级页表，可能会在后面实现

// TTBR1_EL1的L0级页表（第0级）
// 用于高地址空间映射（内核空间，虚拟地址2^48-2^64）
u64 boot_ttbr1_l0[PTP_ENTRIES] ALIGN(PTP_SIZE);
// TTBR1_EL1的L1级页表（第1级）
u64 boot_ttbr1_l1[PTP_ENTRIES] ALIGN(PTP_SIZE);
// TTBR1_EL1的L2级页表（第2级）
u64 boot_ttbr1_l2[PTP_ENTRIES] ALIGN(PTP_SIZE);

// 页表项有效位：第0比特位
// 值为1表示该条目有效，CPU可以使用此页表项进行地址转换
#define IS_VALID (1UL << 0)
// 页表项类型位：第1比特位
// 值为1表示该项是指向下级页表的描述符，值为0表示指向物理页面
#define IS_TABLE (1UL << 1)

// Unprivileged eXecute Never：第54比特位
// 值为1禁止用户态（EL0）代码执行此页面，只能内核（EL1+）执行
#define UXN            (0x1UL << 54)
// 访问标志位：第10比特位
// 表示该页面已被CPU访问过（被TLB引用）
// 用于页面置换算法（LRU）的辅助
#define ACCESSED       (0x1UL << 10)
// Not Global：第11比特位
// 值为1表示此映射仅对当前ASID（地址空间标识器）有效
// 值为0表示全局映射，对所有ASID都有效
#define NG             (0x1UL << 11)
// 可共享性属性：第[9:8]比特位，设置为0b11（值3）
// 表示该页可在多个CPU核心之间共享（内部可共享）
// 用于多处理器系统的缓存一致性
#define INNER_SHARABLE (0x3UL << 8)
// 内存属性：第[4:2]比特位，设置为0b100（值4）
// 表示该页面是普通内存（缓存属性）
// 可以被缓存，支持预取等优化
#define NORMAL_MEMORY  (0x4UL << 2)
// 内存属性：第[4:2]比特位，设置为0b000（值0）
// 表示该页面是设备内存（外设寄存器）
// 不可缓存，必须严格按顺序访问
#define DEVICE_MEMORY  (0x0UL << 2)

// 2MB大小：2 * 1024 * 1024 = 2097152字节 = 0x200000
// L2级页表的块映射粒度：512个L2条目 × 2MB = 1GB
// 一个2MB块由512个4KB页组成
#define SIZE_2M (2UL * 1024 * 1024)

// 从虚拟地址x提取L0级页表索引
// 右移30位(12+9+9+9)得到L0地址
// & 0x1ff：取低9位，范围0-511，对应L0页表的512个条目
// 虚拟地址格式：[L0-9bit][L1-9bit][L2-9bit][page offset-12bit]
#define GET_L0_INDEX(x) (((x) >> (12 + 9 + 9 + 9)) & 0x1ff)
// 从虚拟地址x提取L1级页表索引
// 右移21位(12+9+9)得到L1地址
// & 0x1ff：取低9位，范围0-511，对应L1页表的512个条目
#define GET_L1_INDEX(x) (((x) >> (12 + 9 + 9)) & 0x1ff)
// 从虚拟地址x提取L2级页表索引
// 右移21位(12+9)得到L2地址
// & 0x1ff：取低9位，范围0-511，对应L2页表的512个条目
#define GET_L2_INDEX(x) (((x) >> (12 + 9)) & 0x1ff)

// 初始化内核页表函数
// 设置TTBR0和TTBR1的三级页表，建立虚拟地址到物理地址的映射
void init_kernel_pt(void)
{
        // 初始化虚拟地址为物理内存起始地址
        // 此处使用恒等映射：va = pa（虚拟地址=物理地址）
        // PHYSMEM_START = 0x0UL
        // 虚拟地址变量，用于逐个映射页面
        u64 vaddr = PHYSMEM_START;

        /* TTBR0_EL1 0-1G */
        // 配置TTBR0_EL1的页表（低地址空间用户地址空间）
        // TTBR0控制虚拟地址0x0-0x2^48的映射
        // L0页表项设置：
        // - 获取vaddr的L0索引，指向该虚拟地址在L0页表中的位置
        // - 项值 = boot_ttbr0_l1的物理地址：指向L1页表
        // - | IS_TABLE：标记为页表描述符（不是最终映射）
        // - | IS_VALID：标记该条目有效，MMU必须处理此项
        // - | NG：标记为非全局（非ASID共享）
        boot_ttbr0_l0[GET_L0_INDEX(vaddr)] = ((u64)boot_ttbr0_l1) | IS_TABLE
                                             | IS_VALID | NG;
        // L1页表项设置：
        // - 获取vaddr的L1索引，指向该虚拟地址在L1页表中的位置
        // - 项值 = boot_ttbr0_l2的物理地址：指向L2页表
        // - | IS_TABLE：标记为页表描述符（指向下一级）
        // - | IS_VALID：标记该条目有效
        // - | NG：标记为非全局
        boot_ttbr0_l1[GET_L1_INDEX(vaddr)] = ((u64)boot_ttbr0_l2) | IS_TABLE
                                             | IS_VALID | NG;
        // 结果：L0->L1->L2形成三级页表链

        /* Normal memory: PHYSMEM_START ~ PERIPHERAL_BASE */
        // 映射普通内存区域（0x0到0x3F000000）
        // 这部分是可执行的DRAM（包括代码和数据）
        /* Map with 2M granularity */
        // 使用2MB大小的块进行映射（L2级块映射）
        // 范围：0x0 ~ 0x3F000000 = 1008MB ÷ 2MB = 504次迭代
        for (; vaddr < PERIPHERAL_BASE; vaddr += SIZE_2M) {
                // 循环条件：虚拟地址 < 外设基地址0x3F000000
                // 每次增加2MB，覆盖整个普通内存区域
        
                // 获取vaddr的L2索引，设置L2页表项
                // 这是最后一级页表，指向物理页面或2MB块
                boot_ttbr0_l2[GET_L2_INDEX(vaddr)] =
                
                        // 页表项的低12位和[48:12]保存物理地址
                        // vaddr即是物理地址（恒等映射）
                        // 例如：vaddr=0x200000，则物理地址也是0x200000
                        (vaddr) /* low mem, va = pa */
                        
                        // 禁止用户态(EL0)执行此页
                        // 即使虚拟地址可读写，用户代码也不能在此页执行
                        | UXN /* Unprivileged execute never */
                        
                        // 设置访问标志（第10位）
                        // 表示此页已被访问，可用于LRU页置换
                        | ACCESSED /* Set access flag */
                        
                        // Not Global：此映射不全局共享
                        // 只对当前ASID（线程/进程）有效
                        | NG /* Mark as not global */
                        
                        // 内部可共享：多核CPU间可共享此页
                        // 保证多核缓存一致性
                        | INNER_SHARABLE /* Shareability */
                        
                        // 内存类型为普通内存
                        // 支持缓存、预取等CPU优化
                        | NORMAL_MEMORY /* Normal memory */
                        
                        // 标记此条目有效
                        // MMU识别此项并进行地址转换
                        | IS_VALID;
        }
        // 循环结束：已映射0x0到0x3F000000的所有2MB块: Normal Memory

        /* Peripheral memory: PERIPHERAL_BASE ~ PHYSMEM_END */
        // 映射外设内存区域（0x3F000000到0x40000000）
        // 这部分包含GPIO、UART、定时器等外设寄存器
        // 大小：0x40000000 - 0x3F000000 = 0x1000000 = 16MB
        /* Map with 2M granularity */
        // 同样使用2MB粒度映射
        // 16MB ÷ 2MB = 8次迭代
        for (vaddr = PERIPHERAL_BASE; vaddr < PHYSMEM_END; vaddr += SIZE_2M) {
                // 循环从PERIPHERAL_BASE开始，每次增加2MB
                // 直到达到PHYSMEM_END(0x40000000)
        
                // 设置外设区域的L2页表项
                boot_ttbr0_l2[GET_L2_INDEX(vaddr)] =
                
                        // 物理地址=虚拟地址（恒等映射）
                        (vaddr) /* low mem, va = pa */
                        
                        // 禁止用户态执行外设页
                        // 外设寄存器不可执行
                        | UXN /* Unprivileged execute never */
                        
                        // 设置访问标志位
                        | ACCESSED /* Set access flag */
                        
                        // 非全局映射，仅对当前ASID有效
                        | NG /* Mark as not global */
                        
                        // 内存类型为设备内存
                        // 外设寄存器必须设为设备内存
                        // - 不可缓存
                        // - 禁止预取
                        // - 按顺序访问（no reordering）
                        // 确保对寄存器的访问立即生效
                        | DEVICE_MEMORY /* Device memory */
                        
                        // 标记此条目有效
                        | IS_VALID;
        }
        // 循环结束：已映射0x3F000000到0x40000000的所有2MB块

        /* TTBR1_EL1 0-1G */
        /* LAB 1 TODO 5 BEGIN */
        /* Step 1: set L0 and L1 page table entry */
        vaddr = KERNEL_VADDR + PHYSMEM_START;
        boot_ttbr1_l0[GET_L0_INDEX(vaddr)] = ((u64)boot_ttbr1_l1) | IS_TABLE
                                             | IS_VALID | NG;
        boot_ttbr1_l1[GET_L1_INDEX(vaddr)] = ((u64)boot_ttbr1_l2) | IS_TABLE
                                             | IS_VALID | NG;

        /* Step 2: map PHYSMEM_START ~ PERIPHERAL_BASE with 2MB granularity */
        for (vaddr = KERNEL_VADDR + PHYSMEM_START; vaddr < KERNEL_VADDR + PERIPHERAL_BASE; vaddr += SIZE_2M) {
                boot_ttbr1_l2[GET_L2_INDEX(vaddr)] =
                        (vaddr - KERNEL_VADDR) /* Physical address */
                        | UXN /* Unprivileged execute never */
                        | ACCESSED /* Set access flag */
                        | NG /* Mark as not global */
                        | INNER_SHARABLE /* Shareability */
                        | NORMAL_MEMORY /* Normal memory */
                        | IS_VALID;
        }

        /* Step 3: map PERIPHERAL_BASE ~ PHYSMEM_END with 2MB granularity */
        for (vaddr = KERNEL_VADDR + PERIPHERAL_BASE; vaddr < KERNEL_VADDR + PHYSMEM_END; vaddr += SIZE_2M) {
                boot_ttbr1_l2[GET_L2_INDEX(vaddr)] =
                        (vaddr - KERNEL_VADDR) /* Physical address */
                        | UXN /* Unprivileged execute never */
                        | ACCESSED /* Set access flag */
                        | NG /* Mark as not global */
                        | DEVICE_MEMORY /* Device memory */
                        | IS_VALID;
        }
        /* LAB 1 TODO 5 END */

        /*
         * Local peripherals, e.g., ARM timer, IRQs, and mailboxes
         *
         * 0x4000_0000 .. 0xFFFF_FFFF
         * 1G is enough (for Mini-UART). Map 1G page here.
         */
        vaddr = KERNEL_VADDR + PHYSMEM_END;
        boot_ttbr1_l1[GET_L1_INDEX(vaddr)] = PHYSMEM_END | UXN /* Unprivileged
                                                                  execute never
                                                                */
                                             | ACCESSED /* Set access flag */
                                             | NG /* Mark as not global */
                                             | DEVICE_MEMORY /* Device memory */
                                             | IS_VALID;

}
