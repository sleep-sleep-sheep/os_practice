/*
 * vmem.c - 虚拟内存管理
 * 
 * 实现RISC-V Sv39三级页表操作
 */

#include "riscv.h"
#include "memlayout.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "lib/print.h"
#include "lib/str.h"

// 内核页表
pgtbl_t kernel_pagetable;

// 来自kernel.ld，把内核内存分为两半，前面是代码（只读），后面是数据（可读写），这个变量就是分界线
extern char etext[];

// 来自trampoline.S，跳板页的物理地址
extern char trampoline[];


/*
 * vm_getpte - 获取虚拟地址对应的页表项
 * 
 * @pgtbl: 顶级页表
 * @va: 虚拟地址
 * @alloc: 是否创建不存在的中间页表
 * @return: PTE指针，失败返回NULL
 */
pte_t* vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    if (va >= VA_MAX) {
        panic("vm_getpte: virtual address too large");
    }

    // 遍历三级页表 (level 2 -> level 1 -> level 0)
    for (int level = 2; level > 0; level--) {
        // 获取当前级别的页表索引
        uint64 index = VA_TO_VPN(va, level);
        pte_t *pte = &pgtbl[index];

        if (*pte & PTE_V) {
            // PTE有效，获取下一级页表
            pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
        } else {
            // PTE无效
            if (!alloc) {
                return NULL;
            }
            // 分配新的页表页
            pgtbl_t new_table = (pgtbl_t)pmem_alloc(false);
            if (new_table == NULL) {
                return NULL;
            }
            memset(new_table, 0, PGSIZE);
            // 设置PTE指向新页表，准备下一轮循环
            *pte = PA_TO_PTE((uint64)new_table) | PTE_V;
            pgtbl = new_table;
        }
    }

    // 返回叶子级PTE
    uint64 leaf_index = VA_TO_VPN(va, 0);
    return &pgtbl[leaf_index];
}

/*
 * vm_mappages - 建立虚拟地址到物理地址的映射
 * 
 * @pgtbl: 页表
 * @va: 虚拟起始地址
 * @pa: 物理起始地址
 * @len: 映射长度
 * @perm: 权限位
 */
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    uint64 current_va, end_va;
    pte_t *pte;

    if (len == 0) {
        panic("vm_mappages: size cannot be zero");
    }

    // 对齐到页边界
    current_va = PG_ROUND_DOWN(va);
    end_va = PG_ROUND_DOWN(va + len - 1);

    for (;;) {
        //建立映射过程，中间没有页表，那么帮我新建一个
        pte = vm_getpte(pgtbl, current_va, true);
        if (pte == NULL) {
            panic("vm_mappages: failed to get PTE");
        }

        if (*pte & PTE_V) {
            // 页面已映射 - 更新权限
            *pte = PA_TO_PTE(pa) | perm | PTE_V;
        } else {
            // 新映射
            *pte = PA_TO_PTE(pa) | perm | PTE_V;
        }
        //不管之前有无映射，加上权限 perm,再加上有效位 PTE_V

        if (current_va == end_va) {
            break;
        }
        //处理下一页
        current_va += PGSIZE;
        pa += PGSIZE;
    }
}

/*
 * vm_unmappages - 解除虚拟地址映射
 * 
 * @pgtbl: 页表
 * @va: 虚拟起始地址
 * @len: 解除长度
 * @freeit: 是否释放物理页
 */
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    uint64 current_va;
    pte_t *pte;

    if ((va % PGSIZE) != 0) {
        panic("vm_unmappages: address not page aligned");
    }

    uint64 npages = (PG_ROUND_UP(len)) / PGSIZE;
    if (npages == 0) npages = 1;

    for (current_va = va; current_va < va + npages * PGSIZE; current_va += PGSIZE) {
        //找到页表项，注意这里传false，因为只是删除，不需要分配新页表
        pte = vm_getpte(pgtbl, current_va, false);
        if (pte == NULL) {
            panic("vm_unmappages: walk failed");
        }
        if (!(*pte & PTE_V)) {
            panic("vm_unmappages: page not mapped");
        }
        if (PTE_FLAGS(*pte) == PTE_V) {
            panic("vm_unmappages: not a leaf page");
        }

        // 如果要求释放物理内存，就把对应的物理页还给内存分配器
        if (freeit) {
            uint64 pa = PTE_TO_PA(*pte);
            pmem_free(pa, false);
        }
        *pte = 0;
        // 把PTE清0，代表映射断了
    }
}

/*
 * vm_print_recursive - 递归打印页表
 */
static void vm_print_recursive(pgtbl_t pgtbl, int level)
{
    for (int i = 0; i < 512; i++) {
        pte_t pte = pgtbl[i];
        if (pte & PTE_V) {
            // 打印缩进
            for (int j = 0; j < (2 - level); j++) {
                printf(" ");
            }
            printf("..%d: pte 0x%p pa 0x%p", i, pte, PTE_TO_PA(pte));
            
            // 打印权限
            if (pte & PTE_R) printf(" R");
            if (pte & PTE_W) printf(" W");
            if (pte & PTE_X) printf(" X");
            if (pte & PTE_U) printf(" U");
            printf("\n");

            // 如果不是叶子节点，递归打印下一级
            if (PTE_CHECK(pte) && level > 0) {
                vm_print_recursive((pgtbl_t)PTE_TO_PA(pte), level - 1);
            }
        }
    }
}

/*
 * vm_print - 打印页表内容（调试用）
 */
void vm_print(pgtbl_t pgtbl)
{
    printf("page table 0x%p\n", pgtbl);
    vm_print_recursive(pgtbl, 2);
}

//老师给出版本
void vm_print_2(pgtbl_t pgtbl)
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
/*
 * kvm_map - 向内核页表添加映射
 */
static void kvm_map(pgtbl_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
    vm_mappages(kpgtbl, va, pa, sz, perm);
}

/*
 * kvm_make - 创建内核页表
 */
static pgtbl_t kvm_make(void)
{
    pgtbl_t kpgtbl;

    // 分配顶级页表
    kpgtbl = (pgtbl_t)pmem_alloc(false);
    if (kpgtbl == NULL) {
        panic("kvm_make: failed to allocate kernel page table");
    }
    memset(kpgtbl, 0, PGSIZE);

    // 映射 UART 寄存器，物理地址和虚拟地址都是 UART_BASE，内核往 UART_BASE写数据，CPU就会操作串口硬件
    kvm_map(kpgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);

    // 映射 PLIC 中断控制器（4MB区域）
    kvm_map(kpgtbl, PLIC_BASE, PLIC_BASE, 0x400000, PTE_R | PTE_W);

    // 映射 CLINT 定时器控制器
    kvm_map(kpgtbl, CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W);

    // 映射内核代码段（只读可执行）,不能写防止内核代码被意外修改
    kvm_map(kpgtbl, KERNEL_BASE, KERNEL_BASE, (uint64)etext - KERNEL_BASE, PTE_R | PTE_X);

    // 映射内核数据段和剩余物理内存（可读可写）
    kvm_map(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // 映射跳板页（trampoline）- 内核和用户态共享同一虚拟地址
    // trampoline 代码在用户态和内核态切换时使用
    kvm_map(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 为每个CPU分配和映射内核栈
    for (int i = 0; i < NCPU; i++) {
        uint64 pa = (uint64)pmem_alloc(false);
        if (pa == 0) {
            panic("kvm_make: failed to allocate kernel stack");
        }
        kvm_map(kpgtbl, KSTACK(i), pa, PGSIZE, PTE_R | PTE_W);
    }

    return kpgtbl;
}

/*
 * kvm_init - 初始化内核虚拟内存
 */
void kvm_init(void)
{
    kernel_pagetable = kvm_make();
}

/*
 * kvm_inithart - 在当前CPU上激活内核页表
 */
void kvm_inithart(void)
{
    // 等待之前的内存写入完成
    // sfence_vma：内存屏障。意思是“把前面所有写内存的操作都做完，别乱序”。这是为了安全
    sfence_vma();
    
    // 设置satp寄存器启用分页，一旦写入这个寄存器，CPU 的 MMU（内存管理单元）就会立刻启用分页机制
    //之后所有的地址访问都会经过 kernel_pagetable 进行翻译
    w_satp(MAKE_SATP(kernel_pagetable));
    
    // 再次刷新 TLB（快表），因为页表换了，旧的缓存没用了
    sfence_vma();
}
