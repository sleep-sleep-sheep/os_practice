
#include "lib/lock.h"
#include "lib/print.h"
#include "dev/timer.h"
#include "memlayout.h"
#include "riscv.h"
#include "sbi.h"

/*-------------------- 工作在M-mode --------------------*/

// in trap.S M-mode时钟中断处理流程()
extern void timer_vector();

// 每个CPU在时钟中断中需要的临时空间(考虑为什么可以这么写)
 static uint64 mscratch[NCPU][5];

uint64 timer_mono_clock()
{
    uint64 n;
    asm volatile("rdtime %0" : "=r"(n));
    return n;
}
// 时钟初始化
// called in start.c
void timer_init() {
   int hartid = r_tp();
    
    // 确保不超过最大CPU数量
    if (hartid >= NCPU) {
        panic("timer_init: hartid exceeds NCPU");
    }

    // 设置mscratch数组内容
    mscratch[hartid][3] = CLINT_MTIMECMP(hartid);  // 保存当前CPU的mtimecmp地址
    mscratch[hartid][4] = INTERVAL;                // 保存时钟中断间隔

    // 配置M-mode中断相关寄存器
    w_mscratch((uint64)&mscratch[hartid]);  // 设置mscratch指向当前CPU的临时空间
    w_mtvec((uint64)timer_vector);          // 设置M-mode中断向量表

    // 初始化首次时钟中断：当前时间 + 间隔
    //uint64 mtime = *(uint64*)CLINT_MTIME;
    //*(uint64*)CLINT_MTIMECMP(hartid) = mtime + INTERVAL;
}


/*--------------------- 工作在S-mode --------------------*/

// 系统时钟
static timer_t sys_timer;

// 时钟创建(初始化系统时钟)
void timer_create()
{  
    spinlock_init(&sys_timer.lk, "sys_timer");
    sys_timer.ticks = 0;
}

// 时钟更新(ticks++ with lock)
void timer_update()
{
    spinlock_acquire(&sys_timer.lk);
    sys_timer.ticks++;
    spinlock_release(&sys_timer.lk);
}

// 返回系统时钟ticks
uint64 timer_get_ticks()
{
    uint64 ticks;
    spinlock_acquire(&sys_timer.lk);
    ticks = sys_timer.ticks;
    spinlock_release(&sys_timer.lk);
    return ticks;
}

void timer_setNext(bool update)
{   int hartid = r_tp();
    uint64 mtime = *(uint64*)CLINT_MTIME;
    *(uint64*)CLINT_MTIMECMP(hartid) = mtime + INTERVAL;
}