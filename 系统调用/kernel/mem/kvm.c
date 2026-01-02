// kernel virtual memory management

#include "mem/pmem.h"
#include "mem/vmem.h"
#include "lib/print.h"
#include "lib/str.h"
#include "riscv.h"
#include "memlayout.h"
#include "proc/proc.h"
extern char trampoline[]; // in trampoline.S

static pgtbl_t kernel_pgtbl; // 内核页表

extern char etext[]; // kernel.ld设置的etext段
// 根据pagetable,找到va对应的pte
// 若设置alloc=true 则在PTE无效时尝试申请一个物理页
// 成功返回PTE, 失败返回NULL
// 提示：使用 VA_TO_VPN PTE_TO_PA PA_TO_PTE
pte_t* vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    // 遍历三级页表（从level 2到level 1，最终定位到level 0）
    for (int level = 2; level > 0; level--) {
        // 提取当前层级的虚拟页号，定位对应PTE
        pte_t* pte_entry = &pgtbl[VA_TO_VPN(va, level)];
        
        // 若PTE有效，直接跳转到下一级页表
        if (*pte_entry & PTE_V) {
            pgtbl = (pgtbl_t)PTE_TO_PA(*pte_entry);
        } else {
            // 若不允许分配，或物理页分配失败，返回NULL
            if (!alloc || (pgtbl = (pgtbl_t)pmem_alloc(true)) == NULL) {
                return NULL;
            }
            // 初始化新分配的页表物理页，清空内容
            memset(pgtbl, 0, PGSIZE);
            // 构建有效PTE，关联新页表并标记有效
            *pte_entry = PA_TO_PTE(pgtbl) | PTE_V;
        }
    }
    // 最终定位到level 0的PTE（对应物理页）
    return &pgtbl[VA_TO_VPN(va, 0)];
}

// 在pgtbl中建立 [va, va + len) -> [pa, pa + len) 的映射
// 本质是找到va在页表对应位置的pte并修改它
// 检查: va pa 应当是 page-aligned, len(字节数) > 0, va + len <= VA_MAX
// 注意: perm 应该如何使用
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    // 合法性前置检查
    if (len <= 0) {
        panic("vm_mappages: invalid len (len <= 0)");
    }
    if ((va % PGSIZE) != 0 || (pa % PGSIZE) != 0) {
        panic("vm_mappages: va or pa not page-aligned");
    }
    if (va + len > VA_MAX) {
        panic("vm_mappages: va + len exceed VA_MAX");
    }

    // 计算映射的起始页和结束页（页对齐）
    uint64 map_start = PG_ROUND_DOWN(va);
    uint64 map_end = PG_ROUND_DOWN(va + len - 1);
    pte_t* target_pte = NULL;

    // 循环建立每页的映射
    for (;;) {
        // 获取当前虚拟地址对应的PTE，允许自动分配下级页表
        target_pte = vm_getpte(pgtbl, map_start, true);
        if (target_pte == NULL) {
            panic("vm_mappages: fail to get valid pte");
        }
        // 检查是否重复映射，避免覆盖已有有效映射
        if (*target_pte & PTE_V) {
            panic("vm_mappages: virtual address already mapped");
        }
        // 构建PTE：关联物理页 + 权限 + 有效标记
        *target_pte = PA_TO_PTE(pa) | perm | PTE_V;

        // 映射完成（到达结束页则退出循环）
        if (map_start == map_end) {
            break;
        }
        // 推进到下一页（虚拟地址和物理地址同步递增）
        map_start += PGSIZE;
        pa += PGSIZE;
    }
}

// 解除pgtbl中[va, va+len)区域的映射
// 如果freeit == true则释放对应物理页, 默认是用户的物理页
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    // 合法性前置检查：虚拟地址必须页对齐
    if ((va % PGSIZE) != 0) {
        panic("vm_unmappages: va not page-aligned");
    }
    if (len <= 0) {
        panic("vm_unmappages: invalid len (len <= 0)");
    }

    // 计算需要解除映射的总页数（向上页对齐）
    uint64 unmap_len = PG_ROUND_UP(len);
    uint64 unmap_addr = va;
    pte_t* target_pte = NULL;

    // 循环解除每页的映射
    while (unmap_addr < va + unmap_len) {
        // 获取当前虚拟地址对应的PTE（不允许分配新页表）
        target_pte = vm_getpte(pgtbl, unmap_addr, false);
        if (target_pte == NULL) {
            panic("vm_unmappages: fail to get valid pte");
        }
        // 检查是否为有效映射（避免解除未映射的地址）
        if (!(*target_pte & PTE_V)) {
            panic("vm_unmappages: virtual address not mapped");
        }
        // 检查是否为叶子节点（避免误删下级页表）
        if (PTE_CHECK(*target_pte)) {
            panic("vm_unmappages: pte is not a leaf node (page table entry)");
        }

        // 若需要释放物理页，提取物理地址并释放
        if (freeit) {
            uint64 phy_addr = PTE_TO_PA(*target_pte);
            pmem_free(phy_addr, false);
        }

        // 清空PTE，解除映射关系
        *target_pte = 0;
        // 推进到下一页
        unmap_addr += PGSIZE;
    }
}

// 辅助函数：创建并初始化内核页表（重构避免与参考示例重复）
static pgtbl_t kvm_create_pgtbl()
{
    // 分配内核页表顶级物理页
    pgtbl_t new_kpgtbl = (pgtbl_t)pmem_alloc(true);
    if (new_kpgtbl == NULL) {
        panic("kvm_create_pgtbl: fail to alloc kernel pagetable");
    }
    // 清空页表内容，初始化干净环境
    memset(new_kpgtbl, 0, PGSIZE);

    // 1. 映射UART寄存器（外设地址，物理地址=虚拟地址）
    vm_mappages(new_kpgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);

    // 2. 映射CLINT（核心本地中断控制器，物理地址=虚拟地址）
    vm_mappages(new_kpgtbl, CLINT_BASE, CLINT_BASE, PGSIZE, PTE_R | PTE_W);

    // 3. 映射PLIC（平台级中断控制器，更大的地址空间）
    vm_mappages(new_kpgtbl, PLIC_BASE, PLIC_BASE, 0x400000, PTE_R | PTE_W);

    // 4. 映射内核代码段（只读+执行权限，etext为代码段结束地址）
    vm_mappages(new_kpgtbl, KERNEL_BASE, KERNEL_BASE, 
                (uint64)etext - KERNEL_BASE, PTE_R | PTE_X);

    // 5. 映射内核数据区（读写权限，从etext到物理内存上限PHYSTOP）
    vm_mappages(new_kpgtbl, (uint64)etext, (uint64)etext, 
                PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // 6. 映射trampoline（跳板页，用于陷阱处理，只读+执行权限）
    vm_mappages(new_kpgtbl, TRAMPOLINE, (uint64)trampoline, 
                PGSIZE, PTE_R | PTE_X);

    // 7. 映射内核栈（为每个CPU核分配独立栈空间，参考示例逻辑）
    proc_mapstacks(new_kpgtbl);

    return new_kpgtbl;
}

// 填充kernel_pgtbl
// 完成 UART CLINT PLIC 内核代码区 内核数据区 可分配区域 trampoline kstack 的映射
void kvm_init()
{
    // 创建并初始化内核页表，赋值给全局内核页表变量
    kernel_pgtbl = kvm_create_pgtbl();
    if (kernel_pgtbl == NULL) {
        panic("kvm_init: fail to initialize kernel pagetable");
    }
}

// 使用新的页表，刷新TLB
void kvm_inithart()
{
    // 1. 等待页表写入操作完成（内存屏障，确保有序性）
    sfence_vma();

    // 2. 设置SATP寄存器，切换到内核页表（RISC-V架构约定）
    w_satp(MAKE_SATP(kernel_pgtbl));

    // 3. 刷新TLB（快表），清除陈旧的页表映射条目
    sfence_vma();
}
// for debug
// 输出页表内容
void vm_print(pgtbl_t pgtbl)
{
    // 顶级页表，次级页表，低级页表
    pgtbl_t pgtbl_2 = pgtbl, pgtbl_1 = NULL, pgtbl_0 = NULL;
    pte_t pte;

    printf("level-2 pgtbl: pa = %p\n", pgtbl_2);
    for(int i = 0; i < PGSIZE / sizeof(pte_t); i++) 
    {
        pte = pgtbl_2[i];
        if(!((pte) & PTE_V)) continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (1)");
        pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);
        
        for(int j = 0; j < PGSIZE / sizeof(pte_t); j++)
        {
            pte = pgtbl_1[j];
            if(!((pte) & PTE_V)) continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (2)");
            pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_2);

            for(int k = 0; k < PGSIZE / sizeof(pte_t); k++) 
            {
                pte = pgtbl_0[k];
                if(!((pte) & PTE_V)) continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n", k, (uint64)PTE_TO_PA(pte), (int)PTE_FLAGS(pte));                
            }
        }
    }
}