#include "proc/cpu.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "syscall/sysfunc.h"
#include "syscall/syscall.h"
#include "riscv.h"
#include "memlayout.h"
// 堆伸缩
// uint64 new_heap_top 新的堆顶 (如果是0代表查询, 返回旧的堆顶)
// 成功返回新的堆顶 失败返回-1
uint64 sys_brk()
{
// 1. 获取当前进程控制块
    proc_t* p = myproc();
    if (p == NULL) {
        panic("sys_brk: no running process");
        return (uint64)-1;
    }

    // 2. 提取用户传递的参数：新堆顶地址
    uint64 new_heap_top;
    arg_uint64(0, &new_heap_top);

    // 3. 处理「查询堆顶」请求（new_heap_top == 0）
    if (new_heap_top == 0) {
         printf("堆没有发生改变，堆查询结果：%x\n", new_heap_top);
        return p->heap_top;  // 直接返回当前旧堆顶
    }

    // 4. 合法性校验
    // 4.1 堆顶地址不能低于当前堆顶（避免非法收缩，或单独处理ungrow）
    if (new_heap_top < p->heap_top) {
        // 堆收缩：调用uvm_heap_ungrow
        uint64 ret_heap_top = uvm_heap_ungrow(p->pgtbl, p->heap_top, p->heap_top - new_heap_top);

        // 校验收缩结果
        if (ret_heap_top == (uint64)-1 || ret_heap_top != new_heap_top) {
            printf("sys_brk: heap ungrow failed\n");
            return (uint64)-1;
        }
        printf("缩小后的堆：%x\n", ret_heap_top);
        p->heap_top = ret_heap_top;
        return ret_heap_top;
    }
    /*
    // 4.2 堆顶地址不能超出用户空间最大限制（避免越界）
    if (new_heap_top > USER_MEM_MAX) {  // USER_MEM_MAX 需在memlayout.h中定义
        printf("sys_brk: new heap top exceed user mem limit\n");
        return (uint64)-1;
    }
    */

    // 4.3 堆扩展的长度需合理（避免无效操作）
    uint64 grow_len = new_heap_top - p->heap_top;
    if (grow_len == 0) {
        return p->heap_top;  // 无扩展，直接返回当前堆顶
    }

    // 5. 堆扩展：调用uvm_heap_grow
    uint64 ret_heap_top = uvm_heap_grow(p->pgtbl, p->heap_top, grow_len);
    if (ret_heap_top == (uint64)-1) {
        printf("sys_brk: heap grow failed (no enough memory)\n");
        return (uint64)-1;
    }
     
     printf("增大的新堆：%x\n", ret_heap_top);
    // 6. 更新进程堆顶并返回新堆顶
    p->heap_top = ret_heap_top;
    return ret_heap_top;
}

// 内存映射
// uint64 start 起始地址 (如果为0则由内核自主选择一个合适的起点, 通常是顺序扫描找到一个够大的空闲空间)
// uint32 len   范围(字节, 检查是否是page-aligned)
// 成功返回映射空间的起始地址, 失败返回-1
uint64 sys_mmap()
{
    // 1. 获取当前进程控制块
    proc_t* p = myproc();
    if (p == NULL) {
        panic("sys_mmap: no running process");
        return (uint64)-1;
    }

    // 2. 提取用户传递的参数
    uint64 start;
    uint32 len;
    arg_uint64(0, &start);
    arg_uint32(1, &len);

    // 3. 核心合法性校验
    // 3.1 长度必须大于0且页对齐
    if (len <= 0 || (len % PGSIZE) != 0) {
        printf("sys_mmap: invalid len (not page-aligned or <=0)\n");
        return (uint64)-1;
    }

    // 4. 内核自动选择映射起始地址（start == 0）
    uint64 mmap_start = start;
    const uint64 USER_MEM_MAX = TRAPFRAME - 2 * PGSIZE; // 统一用户空间上限
    if (mmap_start == 0) {
        // 简化策略：从堆顶上方空闲区域开始分配（避开栈、已映射区域）
        mmap_start = PG_ROUND_UP(p->heap_top);  // 使用自定义宏，向上页对齐

        // 校验自动分配的地址是否超出用户空间限制
        if ((mmap_start + len) > USER_MEM_MAX) {
            printf("sys_mmap: no enough free user mem for auto allocation\n");
            return (uint64)-1;
        }
        // 扩展：实际内核中需遍历进程mmap链表，找到足够大的连续空闲区域
        // 此处简化为直接使用堆顶上方地址，满足基础功能需求
    }
    // 3.2 手动指定start时的合法性校验（start≠0）
    else {
        // 校验1：start必须页对齐
        if ((start % PGSIZE) != 0) {
            printf("sys_mmap: invalid start (not page-aligned)\n");
            return (uint64)-1;
        }
        // 校验2：start不能低于堆顶，避免覆盖已有数据
        if (start < PG_ROUND_UP(p->heap_top)) {
            printf("sys_mmap: invalid start (below heap top)\n");
            return (uint64)-1;
        }
        // 校验3：start+len不能超出用户空间上限
        if ((start + len) > USER_MEM_MAX) {
            printf("sys_mmap: invalid start+len (exceed user mem max)\n");
            return (uint64)-1;
        }
    }

    // 5. 执行内存映射：调用uvm_mmap（用户内存区域映射核心函数）
    // 权限设置：用户态可读写执行，有效位
    uvm_mmap(mmap_start, len / PGSIZE, PTE_R | PTE_W | PTE_X | PTE_U | PTE_V);

    // 6. 补充：更新堆顶，避免后续重复分配同一地址（仅自动分配时）
    if (start == 0) {
        p->heap_top = mmap_start + len;
    }

    // 7. 校验映射结果（简化：若地址合法则认为映射成功）
    // 实际内核中可通过vm_getpte检查是否建立有效映射
    return mmap_start;
}

// 取消内存映射
// uint64 start 起始地址
// uint32 len   范围(字节, 检查是否是page-aligned)
// 成功返回0 失败返回-1
uint64 sys_munmap()
{
    // 1. 获取当前进程控制块
    proc_t* p = myproc();
    if (p == NULL) {
        panic("sys_munmap: no running process");
        return (uint64)-1;
    }

    // 2. 提取用户传递的参数
    uint64 start;
    uint32 len;
    arg_uint64(0, &start);
    arg_uint32(1, &len);

    // 3. 核心合法性校验
    // 3.1 长度必须大于0且页对齐
    if (len <= 0 || (len % PGSIZE) != 0) {
        printf("sys_munmap: invalid len (not page-aligned or <=0)\n");
        return (uint64)-1;
    }

    // 3.2 start必须页对齐且合法（不超出用户空间）
    const uint64 USER_MEM_MAX = TRAPFRAME - 2 * PGSIZE;
    if ((start % PGSIZE) != 0) {
        printf("sys_munmap: invalid start (not page-aligned)\n");
        return (uint64)-1;
    }
    if (start >= USER_MEM_MAX || (start + len) > USER_MEM_MAX) {
        printf("sys_munmap: invalid start+len (exceed user mem max)\n");
        return (uint64)-1;
    }

    // 3.3 不能解除堆顶以下的核心区域映射（保护代码、堆、栈）
    if ((start + len) <= PG_ROUND_UP(p->heap_top)) {
        printf("sys_munmap: invalid range (below heap top, protected)\n");
        return (uint64)-1;
    }

    // 4. 执行取消映射：调用uvm_munmap（用户内存区域解除映射核心函数）
    // 释放物理页：freeit = true（释放映射对应的用户物理页）
    uvm_munmap(start, len / PGSIZE);

    // 5. 返回成功标识
    return 0;
}
// copyin 测试 (int 数组)
// uint64 addr
// uint32 len
// 返回 0
uint64 sys_copyin()
{
    proc_t* p = myproc();
    uint64 addr;
    uint32 len;

    arg_uint64(0, &addr);
    arg_uint32(1, &len);

    int tmp;
    for(int i = 0; i < len; i++) {
        uvm_copyin(p->pgtbl, (uint64)&tmp, addr + i * sizeof(int), sizeof(int));
        printf("get a number from user: %d\n", tmp);
    }

    return 0;
}

// copyout 测试 (int 数组)
// uint64 addr
// 返回数组元素数量
uint64 sys_copyout()
{
    int L[5] = {1, 2, 3, 4, 5};
    proc_t* p = myproc();
    uint64 addr;

    arg_uint64(0, &addr);
    uvm_copyout(p->pgtbl, addr, (uint64)L, sizeof(int) * 5);

    return 5;
}

// copyinstr测试
// uint64 addr
// 成功返回0
uint64 sys_copyinstr()
{
    char s[64];

    arg_str(0, s, 64);
    printf("get str from user: %s\n", s);

    return 0;
}
