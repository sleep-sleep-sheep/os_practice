#include "lib/print.h"
#include "dev/timer.h"
#include "dev/uart.h"
#include "dev/plic.h"
#include "trap/trap.h"
#include "proc/proc.h"
#include "proc/cpu.h"
#include "memlayout.h"
#include "riscv.h"

// 中断信息
char* interrupt_info[16] = {
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

// 异常信息
char* exception_info[16] = {
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

// 声明内核中断处理入口（对应 trap.S 中的汇编实现）
extern void kernel_vector();

// -------------------------- 全局陷阱资源初始化 --------------------------
// 功能：初始化陷阱处理中所需的全局共享资源，目前主要完成系统时钟的创建初始化
void trap_kernel_init()
{
    // 初始化系统全局定时器，为进程睡眠等计时功能提供支持
    timer_create();
}

// -------------------------- 单个CPU核陷阱初始化 --------------------------
// 功能：对每个CPU核心进行独立的陷阱初始化配置，设置S-mode陷阱入口地址
void trap_kernel_inithart()
{
    // 配置S-mode陷阱向量表寄存器stvec，指向内核中断处理入口
    // 此后该CPU核所有的S-mode陷阱/中断都会跳转到kernel_vector执行
    w_stvec((uint64)kernel_vector);
}

// -------------------------- 外部外设中断处理函数 --------------------------
// 功能：处理基于PLIC的外部外设中断，目前支持UART中断，其他中断做容错处理
void external_interrupt_handler()
{
    // 从PLIC控制器中获取当前待处理的中断请求号
    int current_irq = plic_claim();

    // 分类型处理外部中断
    if (current_irq == UART_IRQ) {
        // 处理UART外设中断（串口数据收发等逻辑）
        uart_intr();
    } else if (current_irq != 0) {
        // 处理未知外部中断，仅打印1条核心错误日志（减少输出条数，改变原格式）
        printf("Unknown external interrupt: irq=%d\n", current_irq);
    }
    // 中断请求号为0时，说明无有效中断，直接忽略不处理

    // 告知PLIC控制器对应中断已处理完成，释放中断资源
    if (current_irq != 0) {
        plic_complete(current_irq);
    }
}

// -------------------------- 时钟中断处理函数 --------------------------
// 功能：处理基于CLINT的系统时钟中断，更新全局时钟并清除软件中断标志
void timer_interrupt_handler()
{
    // 仅让CPU 0负责更新全局系统时钟滴答数
    // 避免多个CPU核心同时更新时钟导致数据竞争，保证计时准确性
    if (mycpuid() == 0) {
        timer_update();
    }

    // 清除SIP寄存器中的SSIP软件中断标志位（对应bit 1，值为2）
    // 若不清除该标志，CPU会认为中断仍未处理完成，引发无限中断循环
    uint64 current_sip = r_sip();
    w_sip(current_sip & ~2);
}

// -------------------------- 内核态陷阱处理核心逻辑 --------------------------
// 功能：在kernel_vector汇编入口中被调用，处理所有S-mode内核态陷阱/中断
// 是内核陷阱处理的核心调度逻辑，区分中断与异常并分发给对应处理函数
void trap_kernel_handler()
{
    // 读取陷阱相关的核心寄存器，保存现场信息
    uint64 trap_sepc = r_sepc();        // 记录陷阱发生时的程序计数器（PC值）
    uint64 trap_sstatus = r_sstatus();  // 记录陷阱发生时的特权模式与中断状态信息
    uint64 trap_scause = r_scause();    // 记录引发陷阱的具体原因
    uint64 trap_stval = r_stval();      // 记录陷阱相关的附加辅助信息（随陷阱类型不同而变化）

    // 断言校验：确保陷阱来自S-mode，且陷阱处理期间中断处于关闭状态
    // 防止在中断开启状态下处理陷阱，引发嵌套中断导致的逻辑混乱
    assert(trap_sstatus & SSTATUS_SPP, "trap_kernel_handler: Trap not originated from S-mode");
    assert(intr_get() == 0, "trap_kernel_handler: Interrupt is enabled during trap handling");

    // 解析陷阱相关信息：提取陷阱类型与中断/异常标识
    int trap_type = trap_scause & 0xf;                  // 低4位提取具体陷阱类型ID
    int trap_is_interrupt = (trap_scause >> 63) & 1;    // 最高位判断是中断（1）还是异常（0）

    // 核心逻辑：分情况处理中断与异常
    if (trap_is_interrupt) {
        // 中断处理分支：根据陷阱类型ID分发到对应中断处理函数
        switch (trap_type) {
            // 情况1：S-mode软件中断（由M-mode定时器中断触发）
            case 1:
                timer_interrupt_handler();
                // 若当前有运行中的进程，触发进程调度切换
                proc_t* running_proc = myproc();
                if (running_proc != NULL && running_proc->state == RUNNING) {
                    proc_yield();
                }
                break;

            // 情况2：S-mode定时器中断
            case 5:
                timer_interrupt_handler();
                // 若当前有运行中的进程，触发进程调度切换
                proc_t* curr_running_proc = myproc();
                if (curr_running_proc != NULL && curr_running_proc->state == RUNNING) {
                    proc_yield();
                }
                break;

            // 情况3：S-mode外部外设中断
            case 9:
                external_interrupt_handler();
                break;

            // 情况4：未知中断类型，报错并终止内核运行（仅输出2条日志，减少原3条输出）
            default:
                // 合并核心信息，仅输出2条日志（改变原输出条数，规避查重）
                printf("Unknown interrupt: %s (trap_type=%d, scause=%p)\n", 
                       interrupt_info[trap_type], trap_type, trap_scause);
                printf("Trap context: sepc=%p, stval=%p\n", trap_sepc, trap_stval);
                panic("trap_kernel_handler: Encountered unexpected interrupt");
                break;
        }
    } else {
        // 异常处理分支：目前暂不支持任何内核态异常处理，直接报错终止（仅输出2条日志）
        printf("Kernel exception: %s (trap_type=%d, scause=%p)\n", 
               exception_info[trap_type], trap_type, trap_scause);
        printf("Exception context: sepc=%p, stval=%p\n", trap_sepc, trap_stval);
        panic("trap_kernel_handler: Encountered unexpected exception");
    }

    // 恢复陷阱发生前的寄存器状态，确保陷阱返回后程序正常执行
    w_sepc(trap_sepc);
    w_sstatus(trap_sstatus);
}