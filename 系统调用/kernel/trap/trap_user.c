#include "lib/print.h"
#include "trap/trap.h"
#include "mem/vmem.h"
#include "memlayout.h"
#include "proc/cpu.h"
#include "syscall/syscall.h"
#include "riscv.h"

// 声明外部汇编符号（来自trampoline.S）
extern char trampoline[];      // 内核与用户态切换的汇编代码入口
extern char user_vector[];     // 用户态触发陷阱（trap）时，进入内核的入口
extern char user_return[];     // 陷阱处理完毕后，从内核返回用户态的入口

// 声明外部汇编符号（来自trap.S）
extern char kernel_vector[];   // 内核态自身陷阱（trap）的处理流程入口

// 声明外部全局数组（来自trap_kernel.c）
extern char* interrupt_info[16]; // 中断类型对应的错误信息描述数组
extern char* exception_info[16]; // 异常类型对应的错误信息描述数组

// 在user_vector()汇编函数中被调用
// 功能：用户态陷入内核态（trap）的核心处理逻辑，处理系统调用、中断和异常
void trap_user_handler()
{
    // 读取相关控制寄存器，获取陷阱相关信息
    uint64 sepc = r_sepc();          // 读取异常程序计数器，记录发生陷阱时用户态的PC值
    uint64 sstatus = r_sstatus();    // 读取状态寄存器，获取特权模式和中断使能等状态信息
    uint64 scause = r_scause();      // 读取陷阱原因寄存器，获取引发本次trap的具体原因
    uint64 stval = r_stval();        // 读取陷阱附加信息寄存器，保存陷阱的附加补充信息（随陷阱类型不同而变化）
    proc_t* p = myproc();            // 获取当前正在运行的进程控制块

    // 断言验证：确保本次陷阱是从用户态（U-mode）触发的
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: 陷阱并非来自用户态");

    // 修改内核陷阱向量表入口：将后续的陷阱（中断/异常）导向内核态自身的处理流程
    // 避免在处理本次用户态陷阱期间，内核自身的陷阱被错误导向用户态处理逻辑
    w_stvec((uint64)kernel_vector);

    // 保存用户态的程序计数器到进程的陷阱帧中，用于后续返回用户态时恢复
    p->tf->epc = sepc;

    // 解析陷阱原因寄存器，提取关键信息
    int trap_id = scause & 0xf;                              // 提取低4位，获取具体的陷阱编号
    bool isInterrupt = ((scause & ((uint64)1 << 63)) != 0);  // 判断是否为中断（最高位为1表示中断，为0表示异常）
    char* info = isInterrupt ? interrupt_info[trap_id] : exception_info[trap_id]; // 获取对应的错误信息描述

    // 分支1：处理系统调用（scause=8 对应用户态的ecall指令，即系统调用）
    if(scause == 8){
        // sepc指向用户态的ecall指令，处理完系统调用后，需要跳转到下一条指令继续执行
        // 因此将保存的程序计数器+4（RISC-V指令长度为4字节）
        p->tf->epc += 4;

        // 此时已经完成了sepc、scause、sstatus等关键寄存器的操作，可开启中断
        // 避免在后续系统调用处理过程中，屏蔽过多内核中断导致系统响应延迟
        intr_on();

        // 调用系统调用处理核心函数，解析并执行对应的内核态系统调用逻辑（如fork、exit、mmap等）
        syscall();
    }
    // 分支2：处理中断（由外部事件或定时器触发，非程序错误）
    else if (isInterrupt) {
        switch (trap_id)
        {
        case 1:
            // 处理时钟中断（用于进程调度、定时器等功能）
            timer_interrupt_handler();
            break;
        case 5:
            // 处理计时器中断（预留处理逻辑，暂不做实际操作）
            // Pass anyway...
            break;
        case 9:
            // 处理外部中断（如外设I/O完成、键盘输入等外部设备触发的中断）
            external_interrupt_handler();
            break;
        default:
            // 处理未知的意外中断，打印调试信息并终止系统运行
            printf("usertrap(): 进程发生未知中断，进程ID=%d\n", p->pid);
            printf("            陷阱编号: %d  陷阱信息: %s\n", trap_id, info);
            printf("            陷阱原因寄存器值: %p\n", scause);
            printf("            异常程序计数器: %p  陷阱附加信息: %p\n", sepc, stval);
            panic("usertrap: 发生未预期的中断");
            break;
        }
    }
    // 分支3：处理异常（程序运行错误导致，如非法指令、页错误等）
    else {
        // 打印异常详细信息，便于调试定位问题
        printf("usertrap(): 进程发生异常，进程ID=%d\n", p->pid);
        printf("            陷阱编号: %d  陷阱信息: %s\n", trap_id, info);
        printf("            陷阱原因寄存器值: %p\n", scause);
        printf("            异常程序计数器: %p  陷阱附加信息: %p\n", sepc, stval);
        panic("usertrap: 发生未预期的异常");
        // setkilled(p); // 标记进程为终止状态（预留逻辑，暂注释）
    }

    // 陷阱处理完毕，调用函数返回用户态
    trap_user_return();
}

// 功能：内核态处理完陷阱后，返回用户态继续执行
// 调用汇编函数user_return()完成最终的上下文切换和特权模式切换
void trap_user_return()
{
    // 获取当前正在运行的进程控制块
    struct proc *p = myproc();

    // 关闭中断：即将切换陷阱处理入口到用户态，在返回用户态前，禁止内核中断
    // 避免在切换过程中，内核中断干扰陷阱向量表和上下文的配置
    intr_off();

    // 重新配置陷阱向量表：将后续的用户态陷阱（系统调用、中断、异常）导向trampoline.S中的user_vector
    // 计算user_vector在trampoline内存区域中的实际地址（TRAMPOLINE为该区域的起始地址）
    uint64 trampoline_uservec = TRAMPOLINE + (user_vector - trampoline);
    w_stvec(trampoline_uservec);

    // 初始化进程陷阱帧（trapframe）中的内核态相关信息
    // 这些信息将在用户态下一次触发陷阱时，被user_vector汇编函数使用
    p->tf->kernel_satp = r_satp();         // 保存当前内核页表的satp寄存器值
    p->tf->kernel_sp = p->kstack + PGSIZE; // 保存当前进程的内核栈顶地址（内核栈向下生长）
    p->tf->kernel_trap = (uint64)trap_user_handler; // 保存用户态陷阱的内核处理函数入口地址
    p->tf->kernel_hartid = r_tp();         // 保存当前CPU的硬件线程ID，用于多核环境下的进程调度

    // 配置sret指令所需的寄存器，准备从内核态（S-mode）返回用户态（U-mode）
    // sret指令会恢复sstatus寄存器，并跳转到sepc寄存器指向的地址继续执行

    // 步骤1：修改sstatus状态寄存器，设置返回后的特权模式和中断使能
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP; // 清除SPP位（置0），表示sret返回后进入用户态（U-mode）
    x |= SSTATUS_SPIE; // 置位SPIE位，表示返回用户态后，启用用户态的中断（此前内核态已关闭中断）
    w_sstatus(x);      // 将修改后的状态写回sstatus寄存器

    // 步骤2：恢复用户态的程序计数器，设置sepc寄存器为陷阱帧中保存的用户态PC值
    w_sepc(p->tf->epc);

    // 步骤3：构造用户态页表的satp寄存器值，告知trampoline切换到当前进程的用户页表
    uint64 satp = MAKE_SATP(p->pgtbl);

    // 步骤4：跳转到trampoline.S中的user_return汇编函数，完成最终的返回操作
    // 1. 计算user_return在trampoline内存区域中的实际地址
    // 2. 将该地址强制转换为函数指针，传入TRAPFRAME（陷阱帧地址）和satp（用户页表）两个参数
    // 3. user_return会完成页表切换、用户态寄存器恢复、并通过sret指令返回用户态
    uint64 trampoline_userret = TRAMPOLINE + (user_return - trampoline);
    ((void (*)(uint64, uint64))trampoline_userret)(TRAPFRAME, satp);
}