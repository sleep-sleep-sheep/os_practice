#include "mem/kvm.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "lib/print.h"
#include "riscv.h"
#include "memlayout.h"
#include "trap-h/trap.h"  // 用于 trampoline 外部声明

// 外部声明跳板页（来自 trampoline.S）
extern char trampoline[];

pgtbl_t kernel_pgtbl; // 内核页表全局变量

// 辅助函数：多页映射（循环调用单页映射vm_map，自动页对齐）
static int vm_mappages_wrapper(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, uint64 perm) {
    // 检查地址对齐（如果未对齐，自动向上对齐）
    va = PGROUNDUP(va);
    pa = PGROUNDUP(pa);
    len = PGROUNDUP(len);  // 长度向上对齐到页大小

    // 循环映射每一页
    for (uint64 i = 0; i < len; i += PGSIZE) {
        if (vm_map(pgtbl, va + i, pa + i, perm) != 0) {
            panic("vm_mappages_wrapper: map failed at va=0x%lx, pa=0x%lx", va + i, pa + i);
            return -1;
        }
    }
    return 0;
}

// 映射硬件设备寄存器区域（物理地址到虚拟地址的直接映射）
static void kvm_map_devices(pgtbl_t pgtbl) {
    // 设备虚拟地址 = 设备物理地址（直接映射，无需偏移）
    uint64 dev_va = DEV_PHYS_BASE;
    uint64 dev_pa = DEV_PHYS_BASE;
    uint64 dev_len = DEV_SIZE;

    // 映射设备区域（可读可写，用户态不可访问）
    vm_mappages_wrapper(pgtbl, dev_va, dev_pa, dev_len, PTE_R | PTE_W);
    printf("kvm_map_devices: mapped device [pa=0x%lx-0x%lx] to [va=0x%lx-0x%lx]\n",
           dev_pa, dev_pa + dev_len, dev_va, dev_va + dev_len);
}

// 映射内核代码、数据、物理内存、跳板页、首进程内核栈
static void kvm_map_kernel(pgtbl_t pgtbl) {
    extern char etext[], end[];  // 链接器定义：代码段结束、数据段结束
    uint64 va_offset = KERNEL_BASE - PHYS_BASE;  // 虚拟地址 = 物理地址 + 偏移

    // 1. 映射内核代码段（.text）：可读可执行，全局TLB（PTE_G）
    uint64 text_pa = PHYS_BASE;
    uint64 text_va = text_pa + va_offset;  // 0xFFFF800000000000 + ...
    uint64 text_len = (uint64)etext - text_pa;
    vm_mappages_wrapper(pgtbl, text_va, text_pa, text_len, PTE_R | PTE_X | PTE_G);
    printf("kvm_map_kernel: mapped text [pa=0x%lx-0x%lx] to [va=0x%lx-0x%lx]\n",
           text_pa, text_pa + text_len, text_va, text_va + text_len);

    // 2. 映射内核数据段（.data + .bss）：可读可写，全局TLB
    uint64 data_pa = (uint64)etext;
    uint64 data_va = data_pa + va_offset;
    uint64 data_len = (uint64)end - data_pa;
    vm_mappages_wrapper(pgtbl, data_va, data_pa, data_len, PTE_R | PTE_W | PTE_G);
    printf("kvm_map_kernel: mapped data [pa=0x%lx-0x%lx] to [va=0x%lx-0x%lx]\n",
           data_pa, data_pa + data_len, data_va, data_va + data_len);

    // 3. 映射剩余可用物理内存（供内核动态分配）
    uint64 mem_pa = (uint64)end;
    uint64 mem_va = mem_pa + va_offset;
    uint64 mem_len = PHYS_TOP - mem_pa;
    vm_mappages_wrapper(pgtbl, mem_va, mem_pa, mem_len, PTE_R | PTE_W | PTE_G);
    printf("kvm_map_kernel: mapped free mem [pa=0x%lx-0x%lx] to [va=0x%lx-0x%lx]\n",
           mem_pa, mem_pa + mem_len, mem_va, mem_va + mem_len);

    // 4. 新增：映射跳板页（trampoline）—— 首进程U↔S切换必需
    uint64 trampoline_pa = (uint64)trampoline;  // 跳板页物理地址
    uint64 trampoline_va = TRAMPOLINE;          // 跳板页虚拟地址（之前memlayout.h定义）
    vm_mappages_wrapper(pgtbl, trampoline_va, trampoline_pa, PGSIZE, PTE_R | PTE_X | PTE_G);
    printf("kvm_map_kernel: mapped trampoline [pa=0x%lx] to [va=0x%lx]\n",
           trampoline_pa, trampoline_va);

    // 5. 新增：映射proczero的内核栈—— 首进程内核态执行必需
    uint64 proczero_kstack_va = KSTACK(0);  // 首进程内核栈虚拟地址（pid=0）
    void* proczero_kstack_pa = pmem_alloc(true);// 分配1页物理内存作为内核栈
    vm_mappages_wrapper(pgtbl, proczero_kstack_va, (uint64)proczero_kstack_pa, PGSIZE, PTE_R | PTE_W | PTE_G);
    printf("kvm_map_kernel: mapped proczero kstack [pa=0x%lx] to [va=0x%lx]\n",
           (uint64)proczero_kstack_pa, proczero_kstack_va);
}

// 初始化内核虚拟内存（创建页表并完成映射）
void kvm_init() {
    // 创建内核页表（分配一页物理内存存储根页表）
    kernel_pgtbl = pgtbl_create();
    if (!kernel_pgtbl) {
        panic("kvm_init: failed to create kernel page table");
    }

    // 映射内核代码、数据、物理内存、跳板页、首进程内核栈
    kvm_map_kernel(kernel_pgtbl);
    // 映射设备寄存器区域
    kvm_map_devices(kernel_pgtbl);

    printf("kvm_init: kernel page table created at pa=0x%lx\n", kernel_pgtbl);
}

// 启用当前CPU核心的虚拟内存（设置satp寄存器）
void kvm_inithart() {
    if (!kernel_pgtbl) {
        panic("kvm_inithart: kernel page table not initialized");
    }

    // 构造satp寄存器值（Sv39模式，页表物理地址）
    uint64 satp = MAKE_SATP(kernel_pgtbl);
    w_satp(satp);                  // 写入satp寄存器，启用MMU
    asm volatile("sfence.vma");    // 刷新TLB，确保页表生效
    printf("kvm_inithart: CPU%d virtual memory enabled, satp=0x%lx\n", mycpuid(), satp);
}

// 获取当前活跃的内核页表
pgtbl_t kvm_get_pgtbl() {
    return kernel_pgtbl;
}