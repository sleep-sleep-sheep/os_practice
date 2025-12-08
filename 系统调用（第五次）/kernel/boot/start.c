
#include "riscv.h"
#include "memlayout.h"
#include "dev/uart.h"
#include "dev/timer.h"


// 操作系统在内核的栈空间(每个核心占KSTACK_SIZE个字节)
__attribute__ ((aligned (16))) char CPU_stack[4096 * NCPU];

extern void main();


void start()
{
    // 不进行分页(使用物理内存)
    w_satp(0);
    uart_init();
    uint64 hartid = r_mhartid();
    w_tp(hartid);    
    // 开启中断（允许M模式定时器中断、S模式软件中断）
    w_mie(r_mie() | MIE_MTIE);    // 允许M模式定时器中断
    w_mstatus(r_mstatus() | MSTATUS_MIE);  // 开启M模式全局中断

    // 使用tp保存hartid以方便在S态查看
    //w_tp(hartid);

    // 进入main函数完成一系列初始化
    main();
}