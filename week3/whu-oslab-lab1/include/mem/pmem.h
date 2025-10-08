//实现物理内存分配
#ifndef _PMEM_H_
#define _PMEM_H_

#include "common.h"
#include "lib/lock.h"

//空闲链表节点，空闲物理页用此连接
struct run {
  struct run *next;
};
/*
//物理内存分配器
struct {
    struct spinlock lock; //保护空闲链表的自旋锁
    struct run *freelist_kernel; //内核空闲链表头指针
    struct run *freelist_user;   //用户空闲链表头指针
    uint64 kernel_pages_used; //内核已用物理页数
    uint64 user_pages_used;   //用户已用物理页数
} kmem;

*/
void pmem_init(void); //初始化物理内存分配器

void *pmem_alloc(bool in_kernel); //分配一页物理内存，返回物理地址，失败返回NULL

void pmem_free(void *pa , bool in_kernel); //释放一页物理内存，参数是物理地址

uint64 pmem_free_count(bool in_kernel); //返回空闲物理页数

#endif