#ifndef __MEMLAYOUT_H__
#define __MEMLAYOUT_H__

#include "common.h"  // 假设包含 uint64、bool 等基础类型定义

// ======================================
// 1. 页表核心类型定义
// ======================================
// 页表项（64位，存储虚拟页→物理页映射及权限）
typedef uint64 pte_t;
// 页表（指向页表项数组的指针，可表示顶级/次级/低级页表）
typedef pte_t* pgtbl_t;


// ======================================
// 2. 页面基础参数（保持不变，4KB页）
// ======================================
#define PAGE_SHIFT 12        // 页内偏移位数（4KB = 2^12）
#define PGSIZE     (1UL << PAGE_SHIFT)  // 页大小（4KB）
// 地址按页对齐（向下取整）：清除低12位偏移
#define PGROUNDDOWN(x) ((x) & ~(PGSIZE - 1))
// 地址按页对齐（向上取整）：不足一页时补满一页
#define PGROUNDUP(x) (((x) + PGSIZE - 1) & ~(PGSIZE - 1))


// ======================================
// 3. 物理内存区域划分（内核专用 vs 用户可用）
//   内核固定区：物理地址 [PHYS_BASE, end)（内核代码/数据）
//   内核运行区：物理地址 [end, KERN_REGION_END)（内核堆、页表等）
//   用户物理区：物理地址 [KERN_REGION_END, PHYS_TOP)（用户进程内存）
// ======================================
// 内核运行区预留4MB（1024页 × 4KB），需确保与实际内存大小匹配
#define KERNEL_PAGES 1024    // 内核预留物理页数（4MB = 1024 × 4KB）
// 内核运行区起始：内核数据段结束（end）向上页对齐（需在链接脚本中定义end符号）
extern char end[];  // 由链接器提供，指向内核数据段结束地址
#define KERN_REGION_BEGIN PGROUNDUP((uint64)end)
#define KERN_REGION_END   (KERN_REGION_BEGIN + KERNEL_PAGES * PGSIZE)
// 用户物理区：从内核运行区结束到物理内存顶端
#define USER_REGION_BEGIN KERN_REGION_END
#define USER_REGION_END   PHYS_TOP


// ======================================
// 4. SATP寄存器相关（Sv39模式）
// ======================================
#define SATP_MODE_SV39 (8UL << 60)  // SV39分页模式（高4位为1000）
// 构造satp值：模式位 + 页表物理页号（PPN = 物理地址 >> 12）
#define MAKE_SATP(pgtbl) (SATP_MODE_SV39 | ((uint64)(pgtbl) >> PAGE_SHIFT))


// ======================================
// 5. 虚拟地址（VA）分解（Sv39模式，39位有效地址）
// ======================================
#define VA_LEVEL_BITS 9                  // 每级页表索引（VPN）位数（2^9=512项）
#define VA_SHIFT(level) (PAGE_SHIFT + VA_LEVEL_BITS * (level))  // 某级VPN的位移
// 提取虚拟地址中的某级VPN（level：0=低级，1=次级，2=顶级）
#define VA_TO_VPN(va, level) (((uint64)(va) >> VA_SHIFT(level)) & 0x1FF)
// SV39最大虚拟地址（2^39 - 1），最后一个虚拟页的最后一个字节
#define VA_MAX  0x7FFFFFFFFF


// ======================================
// 6. 物理地址（PA）与页表项（PTE）转换（Sv39规范）
//   物理地址：39位 = PPN[26:0]（27位） + offset[11:0]（12位）
//   PTE结构：64位 = 保留位[63:10] + PPN[26:0][9:0] + 标志位[9:0]
// ======================================
// 物理地址转PTE：提取PPN（右移12位），左移10位到PTE的PPN字段
#define PA_TO_PTE(pa) ((((uint64)(pa)) >> PAGE_SHIFT) << 10)
// PTE转物理地址：提取PPN（右移10位），左移12位补全页内偏移
#define PTE_TO_PA(pte) (((pte) >> 10) << PAGE_SHIFT)
// 从PTE中提取物理页号（PPN）
#define PTE_TO_PPN(pte) ((pte) >> 10)
// 从物理地址中提取物理页号（PPN）
#define PA_TO_PPN(pa)   ((uint64)(pa) >> PAGE_SHIFT)
// 从物理页号（PPN）恢复物理地址
#define PPN_TO_PA(ppn)  ((uint64)(ppn) << PAGE_SHIFT)


// ======================================
// 7. 页表项（PTE）标志位（RISC-V标准）
// ======================================
#define PTE_V (1 << 0)  // 有效位：1 = 映射有效
#define PTE_R (1 << 1)  // 读权限：1 = 允许读
#define PTE_W (1 << 2)  // 写权限：1 = 允许写
#define PTE_X (1 << 3)  // 执行权限：1 = 允许执行
#define PTE_U (1 << 4)  // 用户态访问：1 = 用户态可访问
#define PTE_G (1 << 5)  // 全局映射：1 = 所有进程可见（不刷新TLB）
#define PTE_A (1 << 6)  // 访问位：硬件置1表示已访问
#define PTE_D (1 << 7)  // 脏位：硬件置1表示已修改
// 保留位（8-9）：未使用，需置0
#define PTE_FLAGS(pte) ((pte) & 0x3FF)  // 提取所有标志位（低10位）
// 判断PTE是否指向中间页表（非叶子节点）：R/W/X全0且V=1
#define PTE_IS_TABLE(pte) (((pte) & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V)


// ======================================
// 8. 物理内存布局（128MB总大小）
// ======================================
#define PHYS_BASE  0x80000000ul  // 物理内存基地址（RISC-V内核加载起始）
#define PHYS_SIZE  (128 * 1024 * 1024)  // 物理内存总大小（128MB）
//#define PHYS_SIZE  (100* 1024)  // 物理内存总大小（128MB）
#define PHYS_TOP   (PHYS_BASE + PHYS_SIZE)  // 物理内存上限（0x88000000）


// ======================================
// 9. 虚拟内存布局（Sv39，512GB总空间，用户/内核各256GB）
// ======================================
// 用户虚拟空间：0 ~ 256GB-1（低256GB）
#define USER_BASE   0x000000000000ul
#define USER_TOP    0x00007FFFFFFFFFUL  // 用户虚拟地址上限（256GB-1）
// 内核虚拟空间：256GB ~ 512GB-1（高256GB，通过偏移映射物理内存）
#define KERNEL_BASE 0xFFFF800000000000ul  // 内核虚拟基地址（高256GB起始）
#define KERNTOP     0xFFFFFFFFFFFFFFFFul  // 内核虚拟地址上限（512GB-1）
// 内核虚拟地址 = 物理地址 + 偏移量（确保映射到高256GB）
#define KERN_VA_OFFSET (KERNEL_BASE - PHYS_BASE)  // 内核VA与PA的偏移量


// ======================================
// 10. 设备地址布局（物理地址，直接映射到虚拟地址）
// ======================================
// 设备物理地址范围（QEMU预留的"假地址"）
#define DEV_PHYS_BASE 0x0C000000ul    // 设备起始物理地址（PLIC等设备）
#define DEV_PHYS_TOP  0x20000000ul    // 设备结束物理地址（CLINT等设备）
#define DEV_SIZE      (DEV_PHYS_TOP - DEV_PHYS_BASE)  // 设备区域总大小

// UART（串口）设备
#define UART_BASE  0x10000000ul  // UART物理基地址
#define UART_IRQ   10            // UART中断号

// PLIC（平台级中断控制器）
#define PLIC_BASE  0x0c000000ul  // PLIC物理基地址
#define PLIC_PRIORITY(id)    (PLIC_BASE + (id) * 4)        // 中断优先级寄存器
#define PLIC_PENDING         (PLIC_BASE + 0x1000)          // 中断挂起寄存器
#define PLIC_MENABLE(hart)   (PLIC_BASE + 0x2000 + (hart)*0x100)  // 机器模式中断使能
#define PLIC_SENABLE(hart)   (PLIC_BASE + 0x2080 + (hart)*0x100)  // 监督模式中断使能
#define PLIC_MPRIORITY(hart) (PLIC_BASE + 0x200000 + (hart)*0x2000)  // 机器模式优先级阈值
#define PLIC_SPRIORITY(hart) (PLIC_BASE + 0x201000 + (hart)*0x2000)  // 监督模式优先级阈值
#define PLIC_MCLAIM(hart)    (PLIC_BASE + 0x200004 + (hart)*0x2000)  // 机器模式中断认领
#define PLIC_SCLAIM(hart)    (PLIC_BASE + 0x201004 + (hart)*0x2000)  // 监督模式中断认领

// CLINT（核心本地中断控制器）
#define CLINT_BASE 0x2000000ul   // CLINT物理基地址
#define CLINT_MSIP(hartid)    (CLINT_BASE + 4 * (hartid))  // 机器模式软件中断
#define CLINT_MTIMECMP(hartid) (CLINT_BASE + 0x4000 + 8 * (hartid))  // 机器模式定时器比较值
#define CLINT_MTIME            (CLINT_BASE + 0xBFF8)  // 全局定时器值

#endif  // __MEMLAYOUT_H__