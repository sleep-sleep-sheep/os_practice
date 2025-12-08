#include "lib/print.h"
#include "dev/timer.h"
#include "dev/uart.h"
#include "dev/plic.h"
#include "trap-h/trap.h"
#include "proc/proc.h"
#include "memlayout.h"
#include "riscv.h"

// 中断信息描述
static char* interrupt_info[16] = {
    "U-mode software interrupt",      // 0
    "S-mode software interrupt",      // 1
    "reserved-1",                     // 2
    "M-mode software interrupt",      // 3
    "U-mode timer interrupt",         // 4
    "S-mode timer interrupt",         // 5
    "reserved-2",                     // 6
    "M-mode timer interrupt",         // 7
    "U-mode external interrupt",      // 8
    "S-mode external interrupt",      // 9
    "reserved-3",                     // 10
    "M-mode external interrupt",      // 11
    "reserved-4",                     // 12
    "reserved-5",                     // 13
    "reserved-6",                     // 14
    "reserved-7",                     // 15
};

// 异常信息描述
static char* exception_info[16] = {
    "Instruction address misaligned", // 0
    "Instruction access fault",       // 1
    "Illegal instruction",            // 2
    "Breakpoint",                     // 3
    "Load address misaligned",        // 4
    "Load access fault",              // 5
    "Store/AMO address misaligned",   // 6
    "Store/AMO access fault",         // 7
    "Environment call from U-mode",   // 8
    "Environment call from S-mode",   // 9
    "reserved-1",                     // 10
    "Environment call from M-mode",   // 11
    "Instruction page fault",         // 12
    "Load page fault",                // 13
    "reserved-2",                     // 14
    "Store/AMO page fault",           // 15
};

// 声明S-mode中断向量（在trap.S中定义）
extern void kernel_vector();

// 初始化全局中断相关设置
void trap_kernel_init() {
   // timer_init();
    timer_create();  // 初始化系统时钟
    //timer_setNext(true);

}

// 初始化当前核心的中断设置
void trap_kernel_inithart() {
    // 1. 设置S-mode中断向量表
    w_stvec((uint64)kernel_vector);

    // 2. 开启S-mode全局中断
    w_sstatus(r_sstatus() | SSTATUS_SIE);

    // 3. 开启S-mode软件中断
    w_sie(r_sie() | SIE_SSIE);

    // 4. 委托软件中断给S-mode（没有这一步 S 永远不会触发）
    w_mideleg(r_mideleg() | (1 << 1));   // SSIP → S

    printf("sstatus = 0x%lx\n", r_sstatus());
    printf("sie     = 0x%lx\n", r_sie());
    printf("mideleg = 0x%lx\n", r_mideleg());
    printf("stvec   = 0x%lx\n", r_stvec());

}


// 处理外部中断（如UART）
void external_interrupt_handler() {
    int irq = plic_claim();  // 获取中断号
    if (irq == 0) return;    // 忽略无效中断号
    
    if (irq == UART_IRQ) {
        // 处理UART输入（读取字符并回显）
        uart_intr();  // 回显输入字符
    }
    
    plic_complete(irq);  // 通知PLIC中断处理完成
}

// 处理时钟中断
void timer_interrupt_handler() {
    timer_update();  // 更新系统时钟计数（关键遗漏）
    // 清除S-mode软件中断标志（避免重复处理）
    *(volatile uint8*)0x10000000 = 'T';
    w_sip(r_sip() & ~SIP_SSIP);
}

// S-mode中断处理核心逻辑
void trap_kernel_handler() {
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();
    uint64 stval = r_stval();

    // 验证中断上下文
    assert((sstatus & SSTATUS_SPP) != 0, "trap not from S-mode");
    assert(intr_get() == 0, "interrupts enabled during trap");
    

    int trap_id = scause & 0x1F;
    int is_interrupt = (scause >> 63) & 1;
    if (is_interrupt) {
        // 处理中断
        switch (trap_id) {
            case 1:  // S-mode软件中断（由M-mode时钟中断触发）
                printf("timer interupt");
                timer_interrupt_handler();
                break;
            case 9:  // S-mode外部中断（如UART）
                external_interrupt_handler();
                break;
            default:
                printf("Unexpected interrupt: %s\n", interrupt_info[trap_id]);
                panic("unhandled interrupt");
        }
    } else {
        // 处理异常（直接报错）
        printf("Exception: %s, sepc=0x%lx, stval=0x%lx\n",
               exception_info[trap_id], sepc, stval);
        panic("unhandled exception");
    }

    // 更新sepc（继续执行中断前的指令）
    //w_sepc(sepc + 4);
}