#ifndef __VMEM_H__
#define __VMEM_H__

#include "common.h"
#include "mem/mmap.h"

/*
    我们使用RISC-V体系结构中的SV39作为虚拟内存的设计规范

    satp寄存器: MODE(4) + ASID(16) + PPN(44)
    MODE控制虚拟内存模式 ASID与Flash刷新有关 PPN存放页表基地址

    基础页面 4KB
    
    VA和PA的构成:
    VA: VPN[2] + VPN[1] + VPN[0] + offset    9 + 9 + 9 + 12 = 39 (使用uint64存储) => 最大虚拟地址为512GB 
    PA: PPN[2] + PPN[1] + PPN[0] + offset   26 + 9 + 9 + 12 = 56 (使用uint64存储)
    
    为什么是 "9" : 4KB / uint64 = 512 = 2^9 所以一个物理页可以存放512个页表项
    我们使用三级页表对应三级VPN, VPN[2]称为顶级页表、VPN[1]称为次级页表、VPN[0]称为低级页表

    PTE定义:
    reserved + PPN[2] + PPN[1] + PPN[0] + RSW + D A G U X W R V  共64bit
       10        26       9        9       2    1 1 1 1 1 1 1 1
    
    需要关注的部分:
    V : valid
    X W R : execute write read (全0意味着这是页表所在的物理页)
    U : 用户态是否可以访问
    PPN区域 : 存放物理页号

*/

// 页表项
typedef uint64 pte_t;

// 顶级页表
typedef uint64* pgtbl_t;

// satp寄存器相关,,告诉CPU，要开启分页并且使用SV39模式
//高四位值表示 MODE,satp 0到43位存放的是PPN，最后硬件就知道了要开启SV39分页，根页表的地址在satp 0到43对应的位置
#define SATP_SV39 (8L << 60)  // MODE = SV39
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12)) // 设置MODE和PPN字段

// 获取虚拟地址中的虚拟页(VPN)信息 占9bit
#define VA_SHIFT(level)         (12 + 9 * (level))
#define VA_TO_VPN(va,level)     ((((uint64)(va)) >> VA_SHIFT(level)) & 0x1FF)

// PA和PTE之间的转换
#define PA_TO_PTE(pa) ((((uint64)(pa)) >> 12) << 10)
#define PTE_TO_PA(pte) (((pte) >> 10) << 12)

// 页面权限控制位
#define PTE_V (1L << 0) // valid - 页表项有效
#define PTE_R (1L << 1) // read - 可读
#define PTE_W (1L << 2) // write - 可写
#define PTE_X (1L << 3) // execute - 可执行
#define PTE_U (1L << 4) // user - 用户态可访问
#define PTE_G (1L << 5) // global - 全局映射
#define PTE_A (1L << 6) // accessed - 已访问
#define PTE_D (1L << 7) // dirty - 已修改

// 检查一个PTE是否是页表（而非叶子页）：R/W/X全为0表示这是指向下级页表的指针
#define PTE_CHECK(pte) (((pte) & (PTE_R | PTE_W | PTE_X)) == 0)

// 获取PTE的低10bit标志位信息
#define PTE_FLAGS(pte) ((pte) & 0x3FF)

// 定义一个相当大的VA, 规定所有VA不得大于它
#define VA_MAX (1ul << 38)

/*---------------------- in kvm.c -------------------------*/
void   vm_print(pgtbl_t pgtbl);
void   vm_print_2(pgtbl_t pgtbl);
pte_t* vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc);
//查表，给定一个VA，找到对应的 PTE在哪里，如果中间的页表不存在，则根据alloc参数决定是否创建新页表
void   vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm);
// 建立映射，在页表里填好 PTE，让 VA指向 PA
void   vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit);

void   kvm_init();
//内核页表初始化
void   kvm_inithart();
//启用，把页表地址写入 satp寄存器，执行sfence.vma指令刷新 TLB快表


/*------------------------ in uvm.c -----------------------*/
void   uvm_show_mmaplist(mmap_region_t* mmap);

void   uvm_destroy_pgtbl(pgtbl_t pgtbl);
void   uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint32 ustack_pages, mmap_region_t* mmap);

void   uvm_mmap(uint64 begin, uint32 npages, int perm);
void   uvm_munmap(uint64 begin, uint32 npages);

uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 heap_top, uint32 len);
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 heap_top, uint32 len);

void   uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len);
void   uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len);
void   uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen);

#endif