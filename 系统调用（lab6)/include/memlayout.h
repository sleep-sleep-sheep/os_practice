/* memory leyout */
#ifndef __MEMLAYOUT_H__
#define __MEMLAYOUT_H__

// 内核基地址
#define KERNEL_BASE 0x80000000ul

// UART 相关
#define UART_BASE  0x10000000ul
#define UART_IRQ   10

// platform-level interrupt controller(PLIC)
#define PLIC_BASE 0x0c000000ul
// PLIC寄存器区域的起始基地址，PILC是处理外部中断等的枢纽
#define PLIC_PRIORITY(id) (PLIC_BASE + (id) * 4)
#define PLIC_PENDING (PLIC_BASE + 0x1000)
#define PLIC_MENABLE(hart) (PLIC_BASE + 0x2000 + (hart)*0x100)
#define PLIC_SENABLE(hart) (PLIC_BASE + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC_BASE + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC_BASE + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC_BASE + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC_BASE + 0x201004 + (hart)*0x2000)

// core local interruptor(CLINT)
// CLINT处理本地中断，主要是定时器中断
#define CLINT_BASE 0x2000000ul
#define CLINT_MSIP(hartid) (CLINT_BASE + 4 * (hartid))
#define CLINT_MTIMECMP(hartid) (CLINT_BASE + 0x4000 + 8 * (hartid))
#define CLINT_MTIME (CLINT_BASE + 0xBFF8)

// 物理内存结束地址 (128MB)
#define PHYSTOP (KERNEL_BASE + 128*1024*1024)

// 用户态虚拟地址空间布局
// 最大虚拟地址 (SV39: 2^38)
#define VA_MAX (1ul << 38)

// 跳板页：映射在虚拟地址空间最高处
// 内核和用户态共享同一虚拟地址
#define TRAMPOLINE (VA_MAX - PGSIZE)

// trapframe页：紧邻跳板页下方
#define TRAPFRAME (TRAMPOLINE - PGSIZE)

// 内核栈：每个CPU一个，从 TRAMPOLINE 往下分配
// 每个栈由一个guard页和一个栈页组成
#define KSTACK(cpu) (TRAMPOLINE - ((cpu) + 1) * 2 * PGSIZE)

#endif

/*
PLIC和CLINT是实现中断处理的两个关键硬件组件
CLINT产生时钟中断和软件中断，每个核心有独立的定时器
PLIC负责管理外部设备中断（如UART键盘输入），负责中断优先级仲裁
*/