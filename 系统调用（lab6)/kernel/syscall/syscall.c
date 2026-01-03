#include "lib/print.h"
#include "proc/cpu.h"
#include "mem/mmap.h"
#include "mem/vmem.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "syscall/sysfunc.h"

// 系统调用跳转表
static uint64 (*syscalls[])(void) = {
    [SYS_print]         sys_print,
    [SYS_brk]           sys_brk,
    [SYS_mmap]          sys_mmap,
    [SYS_munmap]        sys_munmap,
    [SYS_fork]          sys_fork,
    [SYS_wait]          sys_wait,
    [SYS_exit]          sys_exit,
    [SYS_sleep]         sys_sleep,
};

// 系统调用
void syscall()
{
    // 1. 获取当前正在运行的进程控制块，用于后续访问陷阱帧和进程信息
    proc_t* current_proc = myproc();

    // 2. 从进程陷阱帧的a7寄存器中提取系统调用号（用户态传入的系统调用标识）
    int syscall_num = current_proc->tf->a7;

    // 3. 合法性校验：系统调用号在有效范围内，且对应处理函数存在
    if ((syscall_num >= 0) && (syscall_num <= SYS_MAX) && (syscalls[syscall_num] != NULL))
    {
        // 4. 调用对应的系统调用处理函数，将返回值存入陷阱帧的a0寄存器（返回给用户态）
        current_proc->tf->a0 = syscalls[syscall_num]();
    }
    else
    {
        // 5. 处理未知系统调用：打印错误日志，设置返回值为-1标识失败
        printf("Undefined system call: %d, issued by process %d\n", syscall_num, current_proc->pid);
        current_proc->tf->a0 = (uint64)-1;
    }
}

/*
    其他用于读取传入参数的函数
    参数分为两种,第一种是数据本身,第二种是指针
    第一种使用tf->ax传递
    第二种使用uvm_copyin 和 uvm_copyinstr 进行传递
*/

// 读取 n 号参数,它放在 an 寄存器中
static uint64 arg_raw(int n)
{   
    proc_t* proc = myproc();
    switch(n) {
        case 0:
            return proc->tf->a0;
        case 1:
            return proc->tf->a1;
        case 2:
            return proc->tf->a2;
        case 3:
            return proc->tf->a3;
        case 4:
            return proc->tf->a4;
        case 5:        
            return proc->tf->a5;
        default:
            panic("arg_raw: illegal arg num");
            return -1;
    }
}

// 读取 n 号参数, 作为 uint32 存储
void arg_uint32(int n, uint32* ip)
{
    *ip = arg_raw(n);
}

// 读取 n 号参数, 作为 uint64 存储
void arg_uint64(int n, uint64* ip)
{
    *ip = arg_raw(n);
}

// 读取 n 号参数指向的字符串到 buf, 字符串最大长度是 maxlen
void arg_str(int n, char* buf, int maxlen)
{
    proc_t* p = myproc();
    uint64 addr;
    arg_uint64(n, &addr);

    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, maxlen);
}