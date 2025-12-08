#include "lib/print.h"
#include "trap/trap.h"
#include "proc/cpu.h"
#include "mem/vmem.h"
#include "memlayout.h"
#include "riscv.h"

// in trampoline.S
extern char trampoline[];      // 内核和用户切换的代码
extern char user_vector[];     // 用户触发trap进入内核
extern char user_return[];     // trap处理完毕返回用户

// in trap.S
extern char kernel_vector[];   // 内核态trap处理流程

// in trap_kernel.c
extern char* interrupt_info[16]; // 中断错误信息
extern char* exception_info[16]; // 异常错误信息

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler()
{
    uint64 sepc = r_sepc();          // 记录了发生异常时的pc值
    uint64 sstatus = r_sstatus();    // 与特权模式和中断相关的状态信息
    uint64 scause = r_scause();      // 引发trap的原因
    uint64 stval = r_stval();        // 发生trap时保存的附加信息(不同trap不一样)
    proc_t* p = myproc();

    // 确认trap来自U-mode
    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from u-mode");

    if(scause == CAUSE_ECALL_U){
        uint64 syscall_num = p->tf->a7;
        if(syscall_num == SYS_print){
            printf("get a syscall from proc %d\n",p->pid);
        }
        w_sepc(sepc + 4); // 指令长度为4字节，跳过ecall指令
    }else{
         // 其他陷阱（实验暂不处理）
        printf("trap_user_handler: unknown trap, scause=0x%lx, sepc=0x%lx\n", scause, sepc);
        panic("user trap error");
    }

    trap_user_return();

}

// 调用user_return()
// 内核态返回用户态
void trap_user_return()
{
   proc_t *p = myproc();
   
   uint64 sstatus =r_sstatus();
    sstatus &= ~SSTATUS_SPP; // 设置为U-mode
    sstatus |= SSTATUS_SPIE; // 使能中断
    sstatus |=SSTATUS_UIE;
    w_sstatus(sstatus);

    w_sepc(p->tf->epc); // 设置返回的pc值
    w_satp(MAKE_SATP(p->pgtbl)); // 切换到用户页表
    tlb_flush();
    // 跳转到trampoline的user_return函数
    ((void (*)(void))user_return)();
}