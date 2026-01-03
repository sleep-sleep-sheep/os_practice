#include "lib/lock.h"
#include "lib/print.h"
#include "proc/proc.h"
#include "proc/cpu.h"
#include "riscv.h"

// 带层数叠加的关中断
// 记录第一次关中断前的状态，支持嵌套
void push_off(void)
{
    int old = intr_get();  // 获取当前中断状态
    //intr_get() 函数返回sstatus寄存器的SSTATUS_SIE位，如果为1则表示中断开启，为0则表示中断关闭
    intr_off();            // 关中断,设置sstatus寄存器的SSTATUS_SIE位为0
    
    cpu_t* c = mycpu();
    if (c->noff == 0) {
        c->origin = old;   // 记录原始状态，只有第一次关中断时才记录
    }
    c->noff++;             // 嵌套层数+1
}

// 带层数叠加的开中断
// 只有当嵌套层数归零且原始状态为开中断时才真正开中断
void pop_off(void)
{
    cpu_t* c = mycpu();
    
    //如果中断没有关闭，说明出错了，触发panic
    if (intr_get()) {
        panic("pop_off - interruptible");
    }
    //如果 noff < 1,说明没有对应的 push_off
    if (c->noff < 1) {
        panic("pop_off");
    }
    
    c->noff--;
    if (c->noff == 0 && c->origin) {
        intr_on();         // 恢复中断
    }
    //只有当所有嵌套都结束且原始状态为开中断时才真正开中断
}

// 检查是否持有锁
bool spinlock_holding(spinlock_t *lk)
{
    return (lk->locked && lk->cpuid == mycpuid());
}

// 初始化自旋锁
void spinlock_init(spinlock_t *lk, char *name)
{
    lk->name = name;
    lk->locked = 0;
    lk->cpuid = -1;
}

// 获取自旋锁
void spinlock_acquire(spinlock_t *lk)
{    
    push_off();  // 关中断
    
    if (spinlock_holding(lk)) {
        panic("spinlock_acquire");
    }
    
    // 原子操作：test-and-set
    // 循环直到成功将 locked 从 0 设为 1
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
        ;
    
    __sync_synchronize();  // 内存屏障
    lk->cpuid = mycpuid();
} 

// 释放自旋锁
void spinlock_release(spinlock_t *lk)
{
    if (!spinlock_holding(lk)) {
        panic("spinlock_release");
    }
    
    lk->cpuid = -1;
    __sync_synchronize();  // 内存屏障
    __sync_lock_release(&lk->locked);  // 原子释放
    
    pop_off();  // 恢复中断
}