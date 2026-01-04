/*
 * pmem.c - 物理内存资源管理模块
 * 
 * 基于空闲页链表实现高效的物理页分配与释放，支持多核并发安全访问
 * 采用内核与用户内存区域隔离机制，防止用户态进程耗尽内核核心内存资源
 */

#include "mem/pmem.h"
#include "lib/lock.h"
#include "lib/print.h"
#include "lib/str.h"
#include "riscv.h"
#include "memlayout.h"

typedef struct phy_free_page {
    struct phy_free_page* next_page;  // 指向链表中后续空闲物理页的指针
} phy_free_page_t;

/*
 * 物理内存分配控制区域：
 * 封装单个内存区域的管理属性，提供并发保护与空闲页链表的统一管控
 * 
 * 区域隔离设计的核心价值：
 * 恶意用户进程若无限申请内存且不释放，仅会耗尽自身专属区域的内存
 * 内核区域内存不受影响，能够正常完成中断处理、进程调度等核心功能，保障系统稳定
 */
typedef struct mem_control_zone {
    uint64 zone_start;        // 该区域的起始物理地址
    uint64 zone_end;          // 该区域的终止物理地址
    spinlock_t zone_lock;     // 保护区域并发访问的自旋锁
    uint32 free_page_count;   // 区域内当前剩余可分配的物理页数量
    phy_free_page_t free_list; // 空闲页链表头节点（仅作为链表入口，不对应实际物理页）
} mem_control_zone_t;

// 全局内存区域隔离管理，分别对应内核与用户态进程
static mem_control_zone_t kernel_mem_zone;  // 内核专属物理内存区域
static mem_control_zone_t user_mem_zone;    // 用户态专属物理内存区域


static void mem_zone_initialize(mem_control_zone_t *zone, char *zone_name, void *start, void *end)
{
    // 初始化区域核心属性与空闲链表
    zone->zone_start = (uint64)start;
    zone->zone_end = (uint64)end;
    zone->free_page_count = 0;
    zone->free_list.next_page = NULL;  // 初始化空闲链表为空
    
    // 初始化区域自旋锁，保障多核并发访问安全
    spinlock_init(&zone->zone_lock, zone_name);
    
    // 将起始地址向上对齐到4KB物理页边界，规避不完整物理页的管理问题
    char *curr_page_ptr = (char*)PG_ROUND_UP((uint64)start);
    
    // 遍历区域内所有完整物理页，采用头插法构建空闲页链表（插入效率O(1)）
    for (; curr_page_ptr + PGSIZE <= (char*)end; curr_page_ptr += PGSIZE) {
        // 类型转换，将当前物理页地址转为空闲页节点
        phy_free_page_t *curr_page_node = (phy_free_page_t*)curr_page_ptr;
        
        // 头插法将当前页插入空闲链表头部
        curr_page_node->next_page = zone->free_list.next_page;
        zone->free_list.next_page = curr_page_node;
        
        // 更新区域剩余可分配页面计数
        zone->free_page_count++;
    }
}


void pmem_init(void)
{
    // 计算内核专属内存区域的终止物理地址
    uint64 kernel_zone_end = (uint64)ALLOC_BEGIN + KERNEL_PAGES * PGSIZE;
    
    // 安全边界检查：确保内核区域不超出系统可用物理内存范围
    if (kernel_zone_end > (uint64)ALLOC_END) {
        kernel_zone_end = (uint64)ALLOC_END;
    }
    
    // 初始化内核专属物理内存区域
    mem_zone_initialize(&kernel_mem_zone, "kernel_phy_mem", ALLOC_BEGIN, (void*)kernel_zone_end);
    
    // 初始化用户态专属物理内存区域（从内核区域结束地址到系统内存终止地址）
    mem_zone_initialize(&user_mem_zone, "user_phy_mem", (void*)kernel_zone_end, ALLOC_END);
    
    // 打印区域初始化日志，用于调试与验证
    printf("pmem: kernel_zone [%p - %p], %d free pages\n", 
           kernel_mem_zone.zone_start, kernel_mem_zone.zone_end, kernel_mem_zone.free_page_count);
    printf("pmem: user_zone [%p - %p], %d free pages\n", 
           user_mem_zone.zone_start, user_mem_zone.zone_end, user_mem_zone.free_page_count);
}


void* pmem_alloc(bool in_kernel)
{
    // 根据分配标识选择对应的内存控制区域
    mem_control_zone_t *target_zone = in_kernel ? &kernel_mem_zone : &user_mem_zone;
    
    // 获取区域自旋锁，保护空闲链表的并发修改安全
    spinlock_acquire(&target_zone->zone_lock);
    
    // 从空闲链表头部取出一个可用物理页节点
    phy_free_page_t *alloc_page_node = target_zone->free_list.next_page;
    
    // 若存在可用空闲页，完成链表节点移除与计数更新
    if (alloc_page_node != NULL) {
        // 将链表头指向当前页的下一个节点，完成当前页的移除
        target_zone->free_list.next_page = alloc_page_node->next_page;
        // 减少区域剩余可分配页面计数
        target_zone->free_page_count--;
    }
    
    // 释放区域自旋锁，允许其他核访问该内存区域
    spinlock_release(&target_zone->zone_lock);
    
    // 若分配成功，将物理页内容清零，避免残留数据泄露与逻辑异常
    if (alloc_page_node != NULL) {
        memset(alloc_page_node, 0, PGSIZE);
    }
    
    // 返回分配得到的物理页起始地址
    return (void*)alloc_page_node;
}


void pmem_free(uint64 page, bool in_kernel)
{
    // 根据释放标识选择对应的内存控制区域
    mem_control_zone_t *target_zone = in_kernel ? &kernel_mem_zone : &user_mem_zone;
    
    // 安全检查1：验证待释放地址是否满足4KB物理页对齐要求（低12位必须为0）
    if ((page % PGSIZE) != 0) {
        panic("pmem_free: invalid page address, not aligned to 4KB boundary");
    }
    
    // 安全检查2：验证待释放地址是否在目标区域的合法地址范围内
    if (page < target_zone->zone_start || page >= target_zone->zone_end) {
        panic("pmem_free: invalid page address, out of target zone bounds");
    }
    
    // 填充垃圾数据（0x01重复填充），辅助检测"释放后继续使用"的内存非法访问bug
    memset((void*)page, 1, PGSIZE);
    
    // 获取区域自旋锁，保护空闲链表的并发修改安全
    spinlock_acquire(&target_zone->zone_lock);
    
    // 类型转换，将待释放物理页地址转为空闲页节点
    phy_free_page_t *free_page_node = (phy_free_page_t*)page;
    
    // 头插法将待释放页插入到空闲链表头部，完成释放操作
    free_page_node->next_page = target_zone->free_list.next_page;
    target_zone->free_list.next_page = free_page_node;
    
    // 更新区域剩余可分配页面计数
    target_zone->free_page_count++;
    
    // 释放区域自旋锁，允许其他核访问该内存区域
    spinlock_release(&target_zone->zone_lock);
}