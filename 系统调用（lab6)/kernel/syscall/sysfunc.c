#include "proc/cpu.h"
#include "mem/vmem.h"
#include "mem/pmem.h"
#include "mem/mmap.h"
#include "lib/str.h"
#include "lib/print.h"
#include "memlayout.h"
#include "syscall/sysfunc.h"
#include "syscall/syscall.h"
#include "dev/timer.h"

// -------------------------- 堆内存调整系统调用 sys_brk --------------------------
// 功能：实现进程堆内存的查询、扩展与收缩，避免堆内存与代码段、用户栈冲突
// 参数：uint64 new_heap_top - 目标堆顶地址（传入0时执行查询操作，返回当前堆顶）
// 返回值：执行成功返回调整后的堆顶地址，执行失败返回(uint64)-1
uint64 sys_brk()
{
    // 获取当前运行的进程控制块，后续堆操作均基于该进程
    proc_t* current_proc = myproc();
    // 定义变量存储用户态传入的目标堆顶地址
    uint64 target_heap_top;
    
    // 提取用户态传递的第0个64位参数，即目标堆顶地址
    arg_uint64(0, &target_heap_top);
    
    // 处理堆查询请求：若目标堆顶为0，直接返回当前进程的堆顶地址
    if (target_heap_top == 0) {
        return current_proc->heap_top;
    }
    
    // 堆地址下限校验：禁止低于2倍页大小，防止覆盖代码段等核心内存区域
    if (target_heap_top < 2 * PGSIZE) {
        return (uint64)-1;
    }
    
    // 堆地址上限校验：计算用户栈底部地址，避免堆内存与用户栈区域冲突
    // 预留一页内存作为堆与栈之间的安全隔离带
    uint64 user_stack_bottom = TRAPFRAME - current_proc->ustack_pages * PGSIZE - PGSIZE;
    if (target_heap_top > user_stack_bottom) {
        return (uint64)-1;
    }
    
    // 保存进程当前的堆顶地址，用于判断后续堆操作类型
    uint64 original_heap_top = current_proc->heap_top;
    
    // 处理堆扩展操作：目标堆顶大于当前堆顶，需要分配额外内存空间
    if (target_heap_top > original_heap_top) {
        // 计算需要扩展的内存字节长度
        uint32 heap_grow_length = target_heap_top - original_heap_top;
        // 调用用户堆扩展函数，更新进程堆顶地址
        current_proc->heap_top = uvm_heap_grow(current_proc->pgtbl, original_heap_top, heap_grow_length);
    } 
    // 处理堆收缩操作：目标堆顶小于当前堆顶，需要释放多余的内存空间
    else if (target_heap_top < original_heap_top) {
        // 计算需要收缩的内存字节长度
        uint32 heap_shrink_length = original_heap_top - target_heap_top;
        // 调用用户堆收缩函数，更新进程堆顶地址
        current_proc->heap_top = uvm_heap_ungrow(current_proc->pgtbl, original_heap_top, heap_shrink_length);
    }
    // 若目标堆顶与当前堆顶相等，无需执行任何内存操作，直接保留原堆顶地址
    
    // 返回调整后的进程堆顶地址，标识堆操作执行结果
    return current_proc->heap_top;
}

// -------------------------- 内存区域映射系统调用 sys_mmap --------------------------
// 功能：为当前进程分配连续用户内存并建立页表映射，支持自动分配和手动指定地址两种模式
// 参数：uint64 start - 映射起始地址（传入0时由内核自动查找空闲区域）
//       uint32 len   - 映射内存长度（必须为页大小的整数倍且大于0）
// 返回值：执行成功返回映射区域起始地址，执行失败返回(uint64)-1
uint64 sys_mmap()
{
    // 获取当前运行的进程控制块，基于该进程执行内存映射操作
    proc_t* current_proc = myproc();
    // 定义变量存储映射起始地址和映射长度
    uint64 map_start_addr;
    uint32 map_total_length;
    
    // 提取用户态传递的两个参数：映射起始地址（64位）、映射长度（32位）
    arg_uint64(0, &map_start_addr);
    arg_uint32(1, &map_total_length);
    
    // 映射长度合法性校验：长度不能为0，且必须满足页对齐要求
    if (map_total_length == 0 || map_total_length % PGSIZE != 0) {
        return (uint64)-1;
    }
    
    // 计算完成本次映射所需的内存页数
    uint32 map_page_count = map_total_length / PGSIZE;
    
    // 处理内核自动分配映射地址模式：用户传入起始地址为0
    if (map_start_addr == 0) {
        // 遍历当前进程的mmap区域链表，查找满足大小要求的空闲内存区域
        mmap_region_t* current_region = current_proc->mmap;
        while (current_region != NULL) {
            // 找到空闲页数大于等于所需页数的区域，确定映射起始地址
            if (current_region->npages >= map_page_count) {
                map_start_addr = current_region->begin;
                break;
            }
            // 遍历下一个mmap区域节点
            current_region = current_region->next;
        }
        // 遍历结束后未找到合适空闲区域，返回失败标识
        if (map_start_addr == 0) {
            return (uint64)-1;
        }
    } 
    // 处理用户手动指定映射地址模式：校验地址的合法性
    else {
        // 校验手动指定的起始地址是否满足页对齐要求
        if (map_start_addr % PGSIZE != 0) {
            return (uint64)-1;
        }
    }
    
    // 调用用户内存映射函数，建立页表映射（权限设置为可读、可写）
    uvm_mmap(map_start_addr, map_page_count, PTE_R | PTE_W);
    
    // 返回映射区域的起始地址，标识内存映射操作成功
    return map_start_addr;
}

// -------------------------- 内存区域解除映射系统调用 sys_munmap --------------------------
// 功能：解除当前进程指定地址范围的内存映射，释放对应的物理内存资源
// 参数：uint64 start - 待解除映射的起始地址（必须满足页对齐要求）
//       uint32 len   - 待解除映射的内存长度（必须为页大小整数倍且大于0）
// 返回值：执行成功返回0，执行失败返回(uint64)-1
uint64 sys_munmap()
{
    // 定义变量存储待解除映射的起始地址和长度
    uint64 unmap_start_addr;
    uint32 unmap_total_length;
    
    // 提取用户态传递的两个参数：解除映射起始地址（64位）、解除映射长度（32位）
    arg_uint64(0, &unmap_start_addr);
    arg_uint32(1, &unmap_total_length);
    
    // 解除映射参数合法性校验：地址页对齐、长度大于0且页对齐
    if (unmap_start_addr % PGSIZE != 0 || unmap_total_length == 0 || unmap_total_length % PGSIZE != 0) {
        return (uint64)-1;
    }
    
    // 计算需要解除映射的内存页数
    uint32 unmap_page_count = unmap_total_length / PGSIZE;
    
    // 调用用户内存解除映射函数，释放对应的页表项和物理内存
    uvm_munmap(unmap_start_addr, unmap_page_count);
    
    // 返回0，标识内存解除映射操作执行成功
    return 0;
}

// -------------------------- 字符串打印输出系统调用 sys_print --------------------------
// 功能：从用户态内存地址读取字符串，在内核态完成打印输出操作
// 参数：uint64 addr - 用户态中待打印字符串的起始内存地址
// 返回值：执行成功返回0，标识打印操作完成
uint64 sys_print()
{
    // 定义内核缓冲区，用于存储从用户态复制的字符串（最大支持127个有效字符）
    char kernel_print_buf[128] = {0};
    // 提取用户态传递的字符串参数，复制到内核缓冲区中（避免直接访问用户态地址）
    arg_str(0, kernel_print_buf, 128);
    // 调用内核打印函数，输出缓冲区中的字符串内容
    printf("%s", kernel_print_buf);
    // 返回0，标识字符串打印操作执行成功
    return 0;
}

// -------------------------- 进程复制创建系统调用 sys_fork --------------------------
// 功能：复制当前运行的进程（父进程），创建一个全新的子进程
// 返回值：父进程视角返回子进程PID，子进程视角返回0，创建失败返回(uint64)-1
uint64 sys_fork()
{
    // 调用进程复制核心函数，直接返回操作执行结果
    return proc_fork();
}

// -------------------------- 进程等待子进程退出系统调用 sys_wait --------------------------
// 功能：阻塞当前进程（父进程），等待子进程退出并获取子进程退出状态
// 参数：uint64 addr - 用户态中用于存储子进程退出状态的内存地址
// 返回值：执行成功返回退出子进程的PID，执行失败返回(uint64)-1
uint64 sys_wait()
{
    // 定义变量存储用户态传入的退出状态存储地址
    uint64 exit_state_store_addr;
    // 提取用户态传递的第0个64位参数，即退出状态存储地址
    arg_uint64(0, &exit_state_store_addr);
    // 调用进程等待核心函数，返回操作执行结果
    return proc_wait(exit_state_store_addr);
}

// -------------------------- 进程主动退出系统调用 sys_exit --------------------------
// 功能：终止当前进程的运行，设置进程退出状态并释放占用的系统资源
// 参数：int exit_state - 进程退出状态码，用于告知父进程进程终止原因
// 返回值：理论上不会执行到返回语句，仅做容错兜底返回0
uint64 sys_exit()
{
    // 定义变量存储用户态传入的进程退出状态
    int process_exit_state;
    // 提取用户态传递的第0个32位参数，转换为进程退出状态码
    arg_uint32(0, (uint32*)&process_exit_state);
    // 调用进程退出核心函数，完成进程终止和资源释放工作
    proc_exit(process_exit_state);
    // 进程终止后不会继续执行，返回0作为容错兜底
    return 0;
}

// 声明外部全局系统定时器，用于进程睡眠计时功能实现
extern timer_t sys_timer;

// -------------------------- 进程指定时长睡眠系统调用 sys_sleep --------------------------
// 功能：让当前进程阻塞睡眠指定的定时器滴答数，计时完成后自动唤醒进程
// 参数：uint32 ticks - 进程睡眠时长（以定时器滴答数为计量单位）
// 返回值：执行成功返回0，执行失败返回(uint64)-1
uint64 sys_sleep()
{
    // 定义变量存储用户态传入的睡眠滴答数
    uint32 sleep_total_ticks;
    // 提取用户态传递的第0个32位参数，即睡眠时长滴答数
    arg_uint32(0, &sleep_total_ticks);
    
    // 获取系统定时器自旋锁，保护滴答数的并发访问安全
    spinlock_acquire(&sys_timer.lk);
    // 记录睡眠开始时的定时器滴答数，作为计时基准
    uint64 sleep_start_ticks = sys_timer.ticks;
    
    // 循环判断是否达到指定睡眠时长，未达到则让进程进入睡眠状态放弃CPU
    while (sys_timer.ticks - sleep_start_ticks < sleep_total_ticks) {
        // 调用进程睡眠核心函数，将进程加入睡眠队列等待唤醒
        proc_sleep(&sys_timer.ticks, &sys_timer.lk);
    }
    
    // 睡眠时长到达，释放系统定时器自旋锁
    spinlock_release(&sys_timer.lk);
    // 返回0，标识进程睡眠唤醒操作执行成功
    return 0;
}