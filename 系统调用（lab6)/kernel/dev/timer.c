#include "lib/lock.h"
#include "lib/print.h"
#include "dev/timer.h"
#include "memlayout.h"
#include "riscv.h"

/*-------------------- 工作在M-mode --------------------*/

// in trap.S M-mode时钟中断处理流程()
extern void timer_vector();

// 每个CPU在时钟中断中需要的临时空间,其中0 1 2用来保存a1 a2 a3寄存器，3保存CLINT_MTMECMP地址，4保存INTERVAL值
static uint64 mscratch[NCPU][5];

// 时钟初始化
// called in start.c
void timer_init()
{
    // 每个CPU都有独立的定时器中断源
    int id = r_mhartid();
    
    // 向CLINT请求定时器中断
    // 设置 MTIMECMP = MTIME + INTERVAL
    *(uint64*)CLINT_MTIMECMP(id) = *(uint64*)CLINT_MTIME + INTERVAL;
    
    // 在mscratch[]中为timer_vector准备信息
    // mscratch[0..2]: timer_vector保存寄存器的空间
    // mscratch[3]: CLINT MTIMECMP寄存器地址
    // mscratch[4]: 定时器中断之间期望的间隔
    uint64 *scratch = &mscratch[id][0];
    scratch[3] = CLINT_MTIMECMP(id);
    scratch[4] = INTERVAL;
    w_mscratch((uint64)scratch);
    
    // 设置机器模式的陷阱处理程序,mtvec寄存器保存中断向量地址。当M-mode中断发生时，CPU自动跳转到这个地址执行
    w_mtvec((uint64)timer_vector);
    
    // 启用机器模式中断
    w_mstatus(r_mstatus() | MSTATUS_MIE);
    
    // 启用机器模式定时器中断
    w_mie(r_mie() | MIE_MTIE);
    
    // mstatus.MIE 和 mie.MTIE 分别是全局中断开关和定时器中断开关
}


/*--------------------- 工作在S-mode --------------------*/

// 系统时钟
timer_t sys_timer;

// 时钟创建(初始化系统时钟)
void timer_create()
{
    sys_timer.ticks = 0;
    spinlock_init(&sys_timer.lk, "timer");
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
    return sys_timer.ticks;
}