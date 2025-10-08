#include "lib/lock.h"
#include "proc/proc.h"   // 用于获取当前CPU核心ID (mycpuid())
#include "riscv.h"      // 用于中断控制 (intr_on/intr_off/intr_get)
#include "common.h"     // 用于panic等通用功能
#include "lib/print.h" // 用于打印调试信息
// 嵌套关闭中断：支持多层关闭，需对应次数的pop_off恢复
void push_off() {
    // 保存当前中断状态
    int old_intr = intr_get();  //intr_get() 返回当前中断使能状态（1=开，0=关）
    
    // 关闭当前核心中断，禁止响应外部中断
    intr_off();
    
    // 获取当前核心的私有数据
    cpu_t* cpu = mycpu();
    
    // 首次关闭时记录原始中断状态
    if (cpu->noff == 0) {  
        cpu->origin = old_intr;
    }
    
    // 增加关闭深度计数
    cpu->noff++;
}

// 嵌套恢复中断：与push_off配对使用
void pop_off() {
    cpu_t* cpu = mycpu();
    
    // 确保当前处于关中断状态（否则为错误）
    if (intr_get()) {
        panic("pop_off: interrupts enabled during pop");
    }
    
    // 检查关闭深度是否合法
    if (cpu->noff <= 0) {
        panic("pop_off: too many pops");
    }
    
    // 减少关闭深度计数
    cpu->noff--;
    
    // 当深度为0且原始状态为开中断时，恢复中断
    if (cpu->noff == 0 && cpu->origin) {
        intr_on();
    }
}

// 初始化自旋锁
void spinlock_init(spinlock_t* lk, char* name) {
    lk->locked = 0;       // 初始化为未锁定状态
    lk->name = name;      // 设置锁名称（用于调试）
    lk->cpuid = -1;       // 初始无持有核心（-1表示无效ID）
}

// 获取自旋锁（阻塞式等待）
void spinlock_acquire(spinlock_t* lk) {
    // 关闭中断防止死锁（持有锁时被中断可能导致锁无法释放）
    push_off();
    
    // 检查是否已被当前核心持有（防止自我死锁）
    if (spinlock_holding(lk)) {
        panic("spinlock_acquire: already holding lock");
    }
    
    // 原子操作获取锁：循环直到成功获取
    // __sync_lock_test_and_set会生成原子交换指令，将lk->locked设为1并返回原值
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0) {
        // 空循环等待（自旋）
    }
    
    // 内存屏障：确保临界区操作不会被编译器或CPU重排序到锁获取前
    __sync_synchronize();
    
    // 记录持有锁的核心ID
    lk->cpuid = mycpuid();
}

// 释放自旋锁
void spinlock_release(spinlock_t* lk) {
    // 检查是否由当前核心持有（防止释放未持有的锁）
    if (!spinlock_holding(lk)) {
        panic("spinlock_release: not holding lock");
    }
    
    // 清除持有核心ID
    lk->cpuid = -1;
    
    // 内存屏障：确保临界区操作完成后再释放锁
    __sync_synchronize();
    
    // 原子操作释放锁：将locked设为0
    __sync_lock_release(&lk->locked);
    
    // 恢复中断状态
    pop_off();
}

// 检查当前核心是否持有该锁
bool spinlock_holding(spinlock_t* lk) {
    // 同时检查锁状态和持有核心ID
    return (lk->locked && lk->cpuid == mycpuid());
}
