#include "lib/print.h"
#include "proc/cpu.h"
#include "mem/mmap.h"
#include "mem/vmem.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "syscall/sysfunc.h"
#include "memlayout.h"

// 系统调用跳转
static uint64 (*syscalls[])(void) = {
    [SYS_brk]           sys_brk,
    [SYS_mmap]          sys_mmap,
    [SYS_munmap]        sys_munmap,
    [SYS_copyin]        sys_copyin,
    [SYS_copyout]       sys_copyout,
    [SYS_copyinstr]     sys_copyinstr,
};

// 系统调用主处理函数
void syscall() {
    proc_t* p = myproc();
    if (p == NULL) panic("syscall: no current process!");
    
    // 从a7寄存器获取系统调用号
    uint64 sysnum = p->tf->a7;
    uint64 ret = -1;

    // 检查系统调用号合法性
    if (sysnum < sizeof(syscalls)/sizeof(syscalls[0]) && syscalls[sysnum] != NULL) {
        printf("syscall: PID %d execute syscall %ld (SYS_%s)\n", 
               p->pid, sysnum, 
               sysnum == SYS_brk ? "brk" : 
               sysnum == SYS_mmap ? "mmap" : 
               sysnum == SYS_munmap ? "munmap" :
               sysnum == SYS_copyin ? "copyin" :
               sysnum == SYS_copyout ? "copyout" :
               sysnum == SYS_copyinstr ? "copyinstr" : "unknown");
        ret = syscalls[sysnum]();
    } else {
        printf("syscall: PID %d invalid syscall number %ld\n", p->pid, sysnum);
        ret = -1;
    }

    // 将返回值写入a0寄存器（用户态读取）
    p->tf->a0 = ret;
}

// 读取 n 号参数(an寄存器)
static uint64 arg_raw(int n) {   
    proc_t* proc = myproc();
    switch(n) {
        case 0: return proc->tf->a0;
        case 1: return proc->tf->a1;
        case 2: return proc->tf->a2;
        case 3: return proc->tf->a3;
        case 4: return proc->tf->a4;
        case 5: return proc->tf->a5;
        default:
            panic("arg_raw: illegal arg num %d", n);
            return -1;
    }
}

// 读取 n 号参数, 作为 uint32 存储
void arg_uint32(int n, uint32* ip) {
    *ip = (uint32)arg_raw(n);
}

// 读取 n 号参数, 作为 uint64 存储
void arg_uint64(int n, uint64* ip) {
    *ip = arg_raw(n);
}

// 读取 n 号参数指向的字符串到 buf
void arg_str(int n, char* buf, int maxlen) {
    proc_t* p = myproc();
    uint64 addr;
    arg_uint64(n, &addr);

    memset(buf, 0, maxlen);
    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, maxlen);
}