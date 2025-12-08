#ifndef _MEM_KVM_H_
#define _MEM_KVM_H_

#include "common.h"
#include "pmem.h"
#include "memlayout.h"

#include "mem/vmem.h"

uint64* kvm_walk(uint64 *pagetable, uint64 va, int create);


// 内核页表全局变量声明
extern pgtbl_t kernel_pgtbl;

/**
 * @brief 初始化内核虚拟内存系统
 */
void kvm_init(void);

/**
 * @brief 在当前硬件线程上初始化内核虚拟内存
 */
void kvm_inithart(void);

/**
 * @brief 获取当前活跃的页表
 * @return 页表指针
 */
pgtbl_t kvm_get_pgtbl(void);


#endif