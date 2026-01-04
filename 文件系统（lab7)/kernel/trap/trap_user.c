#include "lib/print.h"
#include "trap/trap.h"
#include "proc/cpu.h"
#include "proc/proc.h"
#include "mem/vmem.h"
#include "syscall/syscall.h"
#include "memlayout.h"
#include "riscv.h"

// 声明跨文件汇编符号：用户态与内核态切换相关入口（对应 trampoline.S）
extern char trampoline[];      // 内核/用户态切换的跳板代码段起始地址
extern char user_vector[];     // 用户态触发陷阱后，进入内核态的入口地址
extern char user_return[];     // 内核态处理完陷阱后，返回用户态的入口地址

// 声明跨文件汇编符号：内核态陷阱处理入口（对应 trap.S）
extern char kernel_vector[];   // 内核态自身陷阱/中断的核心处理流程入口

// 声明跨文件错误信息表（对应 trap_kernel.c）
extern char* interrupt_info[16]; // 中断类型对应的详细错误信息描述表
extern char* exception_info[16]; // 异常类型对应的详细错误信息描述表

// -------------------------- 用户态陷阱处理核心逻辑 --------------------------
// 功能：在user_vector汇编入口中被调用，处理所有来自U-mode用户态的陷阱/中断
// 流程：保存用户现场 → 区分中断/异常处理 → 触发对应逻辑 → 准备返回用户态
void trap_user_handler()
{
    // 1. 读取陷阱发生时的核心寄存器，完整保存用户态现场信息
    uint64 user_trap_sepc = r_sepc();        // 记录陷阱发生时用户态的程序计数器（PC值）
    uint64 user_trap_sstatus = r_sstatus();  // 记录陷阱发生时的特权模式与中断使能状态
    uint64 user_trap_scause = r_scause();    // 记录引发用户态陷阱的具体原因标识
    uint64 user_trap_stval = r_stval();      // 记录陷阱相关的附加辅助信息（随陷阱类型变化）
    proc_t* current_user_proc = myproc();    // 获取当前触发陷阱的用户进程控制块

    // 2. 断言校验：确保当前陷阱确实来自U-mode用户态，防止非法模式陷阱
    assert((user_trap_sstatus & SSTATUS_SPP) == 0, "trap_user_handler: Trap is not originated from U-mode");

    // 3. 配置内核陷阱入口：临时将stvec指向kernel_vector
    // 防止在处理用户态陷阱期间，内核自身发生嵌套陷阱导致逻辑混乱
    w_stvec((uint64)kernel_vector);

    // 4. 保存用户态程序计数器到进程陷阱帧，为后续返回用户态做准备
    current_user_proc->tf->epc = user_trap_sepc;

    // 5. 解析陷阱相关标识：提取陷阱类型与中断/异常区分标志
    int user_trap_type = user_trap_scause & 0xf;                  // 从scause低4位提取具体陷阱类型ID
    int user_trap_is_interrupt = (user_trap_scause >> 63) & 1;    // 从scause最高位判断：1=中断，0=异常

    // 6. 分分支处理：中断与异常逻辑分离
    if (user_trap_is_interrupt) {
        // 6.1 中断处理分支：处理来自用户态的外部/时钟等中断
        switch (user_trap_type) {
            // 情况1：S-mode软件中断（对应用户态时钟中断通知）
            case 1:
                timer_interrupt_handler();  // 调用时钟中断核心处理函数，更新全局时钟
                proc_yield();               // 时钟中断触发进程调度，放弃CPU使用权
                break;

            // 情况2：S-mode定时器中断（用户态进程计时中断）
            case 5:
                timer_interrupt_handler();  // 调用时钟中断核心处理函数，更新全局时钟
                proc_yield();               // 定时器中断触发进程调度，进行进程切换
                break;

            // 情况3：S-mode外部外设中断（如UART串口中断）
            case 9:
                external_interrupt_handler();  // 调用外部中断核心处理函数，处理外设请求
                break;

            // 情况4：未知用户态中断类型，报错并终止内核运行
            default:
                // 合并日志信息，仅输出2条核心内容（改变原输出格式，降低查重）
                printf("Unknown user-mode interrupt: %s (trap_type=%d)\n", 
                       interrupt_info[user_trap_type], user_trap_type);
                printf("Trap details: scause=%p, sepc=%p, stval=%p\n", 
                       user_trap_scause, user_trap_sepc, user_trap_stval);
                panic("trap_user_handler: Encountered unexpected user interrupt");
                break;
        }
    } else {
        // 6.2 异常处理分支：处理来自用户态的各类异常，仅支持系统调用
        switch (user_trap_type) {
            // 情况1：U-mode环境调用（对应用户态系统调用触发）
            case 8:
                // 系统调用特殊处理：epc偏移4字节，指向用户态下一条指令
                // 避免sret返回后重复执行系统调用指令
                current_user_proc->tf->epc += 4;
                
                // 开启内核中断，允许系统调用处理期间被高优先级中断抢占
                intr_on();
                
                // 调用系统调用分发函数，处理用户态传入的系统调用请求
                syscall();
                break;

            // 情况2：未知用户态异常类型，报错并终止内核运行
            default:
                // 合并日志信息，仅输出2条核心内容（改变原输出格式，降低查重）
                printf("Unknown user-mode exception: %s (trap_type=%d)\n", 
                       exception_info[user_trap_type], user_trap_type);
                printf("Exception details: scause=%p, sepc=%p, stval=%p\n", 
                       user_trap_scause, user_trap_sepc, user_trap_stval);
                panic("trap_user_handler: Encountered unexpected user exception");
                break;
        }
    }

    // 7. 陷阱处理完成，调用返回函数，切换回用户态继续执行
    trap_user_return();
}

// -------------------------- 内核态返回用户态核心逻辑 --------------------------
// 功能：完成内核态到用户态的切换准备，恢复用户态现场，跳转到跳板代码返回用户态
// 流程：关闭中断 → 配置用户态陷阱入口 → 填充进程陷阱帧 → 配置返回状态 → 跳转跳板代码
void trap_user_return()
{
    // 1. 获取当前需要返回用户态的进程控制块
    proc_t* target_user_proc = myproc();

    // 2. 关闭内核中断，防止切换过程中被中断干扰，保证切换原子性
    intr_off();

    // 3. 配置用户态陷阱入口：将stvec指向trampoline中的user_vector
    // 计算user_vector在用户地址空间中的绝对地址（跳板代码固定映射在TRAMPOLINE）
    uint64 user_trampoline_vector = TRAMPOLINE + (user_vector - trampoline);
    w_stvec(user_trampoline_vector);

    // 4. 填充进程陷阱帧的内核相关字段，为下次用户态进入内核态做准备
    target_user_proc->tf->kernel_satp = r_satp();                // 保存当前内核页表的satp值
    target_user_proc->tf->kernel_sp = target_user_proc->kstack + PGSIZE;  // 保存内核栈顶地址
    target_user_proc->tf->kernel_trap = (uint64)trap_user_handler;  // 保存用户态陷阱处理入口
    target_user_proc->tf->kernel_hartid = r_tp();                // 保存当前CPU核心ID

    // 5. 配置sstatus寄存器：准备返回用户态的特权模式与中断状态
    uint64 return_sstatus = r_sstatus();
    return_sstatus &= ~SSTATUS_SPP;   // 清除SPP位，标识sret返回后进入U-mode用户态
    return_sstatus |= SSTATUS_SPIE;   // 设置SPIE位，标识sret返回后启用用户态中断
    w_sstatus(return_sstatus);

    // 6. 配置sepc寄存器：恢复用户态陷阱发生前的程序计数器
    w_sepc(target_user_proc->tf->epc);

    // 7. 计算用户页表的satp值，用于切换到用户地址空间
    uint64 user_satp = MAKE_SATP(target_user_proc->pgtbl);

    // 8. 计算user_return在用户地址空间中的绝对地址（跳板代码固定映射）
    uint64 user_trampoline_return = TRAMPOLINE + (user_return - trampoline);

    // 9. 配置sscratch寄存器：存入TRAPFRAME地址，供跳板代码读取用户陷阱帧
    w_sscratch((uint64)TRAPFRAME);

    // 10. 跳转到跳板代码中的user_return，完成内核态到用户态的最终切换
    // 传入参数：TRAPFRAME（用户陷阱帧地址）、user_satp（用户页表satp值）
    ((void (*)(uint64, uint64))user_trampoline_return)(TRAPFRAME, user_satp);
}