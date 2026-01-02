#include "mem/pmem.h"
#include "lib/print.h"
#include "lib/lock.h"
#include "lib/str.h"
#include "memlayout.h"
#include "riscv.h"

// 物理页节点
typedef struct page_node {
    struct page_node* next;
} page_node_t;

// 许多物理页构成一个可分配的区域
typedef struct alloc_region {
    uint64 begin;          // 起始物理地址
    uint64 end;            // 终止物理地址
    spinlock_t lk;         // 自旋锁(保护下面两个变量)
    uint32 allocable;      // 可分配页面数    
    page_node_t list_head; // 可分配链的链头节点
} alloc_region_t;

// 内核和用户可分配的物理页分开
static alloc_region_t kern_region, user_region;

#define KERN_PAGES 1024 // 内核可分配空间占1024个pages
//#define PGSIZE 4096     // 补充页大小宏（若memlayout.h未定义）

// 辅助声明：物理内存分配起始/结束标记（由链接脚本kernel.ld定义）
extern char ALLOC_BEGIN[];
extern char ALLOC_END[];

// 物理内存初始化
void pmem_init(void)
{
    // 1. 初始化内核物理内存分配区域
    spinlock_init(&kern_region.lk, "kernel_phys_mem_lock"); // 初始化内核自旋锁
    kern_region.begin = (uint64)ALLOC_BEGIN;                // 内核物理内存起始地址
    kern_region.end = kern_region.begin + KERN_PAGES * PGSIZE; // 内核物理内存结束地址
    kern_region.allocable = 0;                              // 初始化可分配页数为0
    kern_region.list_head.next = NULL;                      // 初始化空闲链表头

    // 2. 构建内核空闲物理页链表（头插法，高效串联）
    for (uint64 phy_addr = kern_region.begin; phy_addr < kern_region.end; phy_addr += PGSIZE) {
        page_node_t* idle_page = (page_node_t*)phy_addr;    // 物理地址转换为页节点
        idle_page->next = kern_region.list_head.next;       // 新节点指向当前链表头下一个节点
        kern_region.list_head.next = idle_page;             // 链表头指向新节点
        kern_region.allocable++;                            // 累加可分配页数
    }

    // 3. 初始化用户物理内存分配区域
    spinlock_init(&user_region.lk, "user_phys_mem_lock");   // 初始化用户自旋锁
    user_region.begin = kern_region.end;                    // 用户物理内存紧跟内核之后
    user_region.end = (uint64)ALLOC_END;                    // 用户物理内存结束地址（全局物理内存上限）
    user_region.allocable = 0;                              // 初始化可分配页数为0
    user_region.list_head.next = NULL;                      // 初始化空闲链表头

    // 4. 构建用户空闲物理页链表（头插法，与内核逻辑一致）
    for (uint64 phy_addr = user_region.begin; phy_addr < user_region.end; phy_addr += PGSIZE) {
        page_node_t* idle_page = (page_node_t*)phy_addr;    // 物理地址转换为页节点
        idle_page->next = user_region.list_head.next;       // 新节点指向当前链表头下一个节点
        user_region.list_head.next = idle_page;             // 链表头指向新节点
        user_region.allocable++;                            // 累加可分配页数
    }

    // 5. 打印初始化信息（调试用，可选）
    printf("pmem_init: kernel phys mem %lu - %lu, %u pages available\n",
           kern_region.begin, kern_region.end, kern_region.allocable);
    printf("pmem_init: user phys mem %lu - %lu, %u pages available\n",
           user_region.begin, user_region.end, user_region.allocable);
}

// 返回一个可分配的干净物理页
// 失败则panic锁死
void* pmem_alloc(bool in_kernel)
{
    // 1. 选择对应的物理内存分配区域（内核/用户）
    alloc_region_t* target_region = in_kernel ? &kern_region : &user_region;

    // 2. 加锁保护，确保多核并发安全（避免竞态条件）
    spinlock_acquire(&target_region->lk);

    // 3. 检查是否有可用物理页，无可用则panic（先解锁避免锁泄露）
    if (target_region->allocable == 0 || target_region->list_head.next == NULL) {
        spinlock_release(&target_region->lk);
        panic("pmem_alloc: no available physical pages ");
    }

    // 4. 从空闲链表头部提取一个物理页（头删法，O(1)高效操作）
    page_node_t* alloc_page = target_region->list_head.next;
    target_region->list_head.next = alloc_page->next; // 链表头跳过已分配节点

    // 5. 更新可分配页数，解锁释放自旋锁
    target_region->allocable--;
    spinlock_release(&target_region->lk);

    // 6. 清空物理页内容，返回干净的物理地址（避免垃圾数据干扰）
    memset((void*)alloc_page, 0, PGSIZE);
    return (void*)alloc_page;
}

// 释放物理页
// 失败则panic锁死
void pmem_free(uint64 page, bool in_kernel)
{
    // 1. 合法性前置校验（页对齐、地址范围有效）
    if ((page % PGSIZE) != 0) {
        panic("pmem_free: physical address not page-aligned");
    }

    // 2. 选择对应的物理内存分配区域（内核/用户）
    alloc_region_t* target_region = in_kernel ? &kern_region : &user_region;

    // 3. 额外校验：释放的地址是否在目标区域范围内
    if (page < target_region->begin || page >= target_region->end) {
        panic("pmem_free: physical address out of region range ");
    }

    // 4. 转换物理地址为页节点，清空页内容（避免残留敏感数据）
    page_node_t* free_page = (page_node_t*)page;
    memset((void*)free_page, 0, PGSIZE);

    // 5. 加锁保护，确保多核并发安全
    spinlock_acquire(&target_region->lk);

    // 6. 头插法将释放页插入空闲链表（O(1)高效操作）
    free_page->next = target_region->list_head.next;
    target_region->list_head.next = free_page;

    // 7. 更新可分配页数，解锁释放自旋锁
    target_region->allocable++;
    spinlock_release(&target_region->lk);
}