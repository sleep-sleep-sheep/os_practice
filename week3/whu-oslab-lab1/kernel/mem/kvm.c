#include "mem/kvm.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "lib/print.h"
#include "riscv.h"

pgtbl_t kernel_pgtbl; // 内核页表全局变量

// 映射硬件设备寄存器区域（物理地址到虚拟地址的直接映射）
static void kvm_map_devices(pgtbl_t pgtbl) {
    // 假设设备物理地址范围为 DEV_PHYS_BASE ~ DEV_PHYS_BASE + DEV_SIZE
    // 虚拟地址与物理地址直接映射（va = pa）
    vm_mappages(pgtbl,
               DEV_PHYS_BASE+KERNEL_BASE,       // 设备虚拟地址 = 物理地址（直接映射）
               DEV_PHYS_BASE,       // 设备物理地址起始
               DEV_SIZE,            // 设备区域大小（需页对齐）
               PTE_R | PTE_W);      // 设备寄存器通常可读可写，无执行权限
}

// 映射内核代码、数据及物理内存区域（线性映射：va = pa + KERNEL_VA_OFFSET）
static void kvm_map_kernel(pgtbl_t pgtbl) {
    extern char etext[], end[];  // 链接器定义的符号：代码段结束、数据段结束

    // 内核虚拟地址 = 物理地址 + 偏移量（确保内核虚拟地址在高地址空间，如 0xFFFF800000000000 以上）
    // 例如：PHYS_BASE（物理基地址，如 0x80000000）映射到 KERNEL_BASE（虚拟基地址，如 0xFFFF800080000000）
    uint64 va_offset = KERNEL_BASE - PHYS_BASE;  // 虚拟地址与物理地址的偏移量

    // 1. 映射内核代码段（.text）：物理地址 [PHYS_BASE, etext) → 虚拟地址 [KERNEL_BASE, KERNEL_BASE + (etext - PHYS_BASE))
    uint64 text_va = PHYS_BASE + va_offset;      // 代码段虚拟起始地址（= KERNEL_BASE）
    uint64 text_pa = PHYS_BASE;                  // 代码段物理起始地址
    uint64 text_len = (uint64)etext - text_pa;   // 代码段长度（需页对齐）
    vm_mappages(pgtbl, text_va, text_pa, text_len, PTE_R | PTE_X | PTE_G);

    // 2. 映射内核数据段（.data + .bss）：物理地址 [etext, end) → 虚拟地址 [etext + va_offset, end + va_offset)
    uint64 data_va = (uint64)etext + va_offset;  // 数据段虚拟起始地址
    uint64 data_pa = (uint64)etext;              // 数据段物理起始地址
    uint64 data_len = (uint64)end - data_pa;     // 数据段长度（需页对齐）
    vm_mappages(pgtbl, data_va, data_pa, data_len, PTE_R | PTE_W | PTE_G);
     
    
    



    
    // 3. 映射剩余可用物理内存（供内核代码/数据占用的部分）：物理地址 [end, PHYS_TOP) → 虚拟地址 [end + va_offset, PHYS_TOP + va_offset)
    uint64 mem_va = (uint64)end + va_offset;     // 剩余内存虚拟起始地址
    uint64 mem_pa = (uint64)end;                 // 剩余内存物理起始地址
    uint64 mem_len = PHYS_TOP - mem_pa;          // 剩余内存长度（需页对齐）
    vm_mappages(pgtbl, mem_va, mem_pa, mem_len, PTE_R | PTE_W | PTE_G);
    
}

// 初始化内核虚拟内存（创建页表并完成映射）
void kvm_init() {
    // 创建内核页表（分配一页物理内存存储根页表）
    kernel_pgtbl = vm_create();
    if (!kernel_pgtbl) {
        panic("kvm_init: failed to create kernel page table");
    }

    // 映射内核代码、数据及物理内存
    kvm_map_kernel(kernel_pgtbl);
    // 映射设备寄存器区域
    kvm_map_devices(kernel_pgtbl);

    printf("kvm_init: kernel page table created at pa=%p\n", kernel_pgtbl);
}

// 启用当前CPU核心的虚拟内存（设置satp寄存器）
void kvm_inithart() {
    if (!kernel_pgtbl) {
        panic("kvm_inithart: kernel page table not initialized");
    }

    // 构造satp寄存器值（Sv39模式，页表物理地址）
    uint64 satp = MAKE_SATP(kernel_pgtbl);
    w_satp(satp);                  // 写入satp寄存器，启用MMU
    asm volatile("sfence.vma");    // 刷新TLB，确保页表生效（简化版，刷新所有TLB）
    printf("kvm_inithart: virtual memory enabled, satp=0x%lx\n", satp);
}

// 获取当前活跃的内核页表
pgtbl_t kvm_get_pgtbl() {
    return kernel_pgtbl;
}