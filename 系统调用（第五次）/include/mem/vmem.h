#ifndef __VMEM_H__
#define __VMEM_H__

#include "common.h"
#include "memlayout.h"
#include "pmem.h"


extern char end[]; //内核结束地址，由链接器提供
/*
    打印页表内容，调试用
    参数：
        pagetable：页表的根地址（物理地址）
*/
void vm_print(pgtbl_t pagetable); //打印页表内容，调试用


/*
    获取虚拟地址 va 对应的页表项指针
    参数：
        pagetable：页表的根地址（物理地址）
        va：要查找的虚拟地址
        alloc：如果为 true，表示在缺页时创建新的页表项；否则不创建
    返回值：
        成功时返回指向对应页表项的指针
        失败时返回 NULL（如地址无效或内存不足）
    注意：
        1. va 必须小于 VA_MAX，否则视为无效地址
        2. 如果 alloc 为 true，函数会在需要时分配新的页表页
        3. 返回的页表项指针指向三级页表中的一个条目
        4. 页表项格式参考 memlayout.h 中 PTE_* 宏定义
*/
pte_t * vm_getpte(pgtbl_t pagetable, uint64 va ,bool alloc); //获取虚拟地址va对应的页表项指针，若无则返回NULL


/*
    建立虚拟地址到物理地址的映射
    参数：
        pagetable：页表的根地址（物理地址）
        va：要映射的虚拟地址（需页对齐）
        pa：要映射到的物理地址（需页对齐）
        len：映射长度（字节数，需为页大小的整数倍）
        perm：页表项权限标志（PTE_* 宏定义的按位或组合）
    注意：
        1. va 和 pa 必须页对齐，len 必须是 PGSIZE 的整数倍
        2. 映射区域不能超出 VA_MAX，否则视为无效
        3. 如果映射区域已有映射，函数会触发 panic
        4. 页表项格式参考 memlayout.h 中 PTE_* 宏定义
*/
void vm_mappages(pgtbl_t pagetable, uint64 va, uint64 pa, uint64 len, int perm); //建立虚拟地址到物理地址的映射，参数是页表根、虚拟地址、大小、物理地址和权限



/*
    取消虚拟地址到物理地址的映射
    参数：
        pagetable：页表的根地址（物理地址）
        va：要取消映射的虚拟地址（需页对齐）
        len：取消映射长度（字节数，需为页大小的整数倍）
        do_free：如果为 true，表示同时释放对应的物理页；否则不释放
    注意：
        1. va 必须页对齐，len 必须是 PGSIZE 的整数倍
        2. 取消映射区域不能超出 VA_MAX，否则视为无效
        3. 如果取消的区域没有映射，函数会触发 panic
        4. 如果 do_free 为 true，函数会调用 pmem_free 释放物理页
*/
void vm_unmappages(pgtbl_t pagetable, uint64 va, uint64 len, bool do_free); //取消虚拟地址到物理地址的映射，参数是页表根、虚拟地址、大小和是否释放物理页



/*
    释放整个页表及其映射的物理页
    参数：
        pagetable：页表的根地址（物理地址）
        va ：起始虚拟地址（需页对齐）
    注意：
        1. 函数会递归释放所有三级页表及其映射的物理页
        2. 释放后 pagetable 指针不再有效，调用者需避免使用
        3. 函数会调用 pmem_free 释放所有相关的物理页
*/
uint64 vm_walkaddr(pgtbl_t pagetable, uint64 va); //返回虚拟地址va对应的物理地址，若无映射则返回0


/*
    创建一个新的页表
    返回值：
        成功时返回新页表的根地址（物理地址）
        失败时返回 NULL（如内存不足）
    注意：
        1. 新页表为空，没有任何映射
        2. 函数会调用 pmem_alloc 分配页表页
        3. 调用者负责在不再需要时调用 vm_free 释放页表
*/
pgtbl_t vm_create(); //创建一个新的页表，返回页表根地址（物理地址），失败返回NULL


/*
    释放整个页表及其映射的物理页
    参数：
        pagetable：页表的根地址（物理地址）
    注意：
        1. 函数会递归释放所有三级页表及其映射的物理页
        2. 释放后 pagetable 指针不再有效，调用者需避免使用
        3. 函数会调用 pmem_free 释放所有相关的物理页
*/
void vm_free(pgtbl_t pagetable); //释放整个页表及其映射的物理页，参数是页表根地址（物理地址）

#endif