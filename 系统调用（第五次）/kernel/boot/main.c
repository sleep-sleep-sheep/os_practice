#include "lib/print.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "mem/kvm.h"
#include "mem/mmap.h"
#include "proc/cpu.h"
#include "proc/proc.h"
#include "syscall/syscall.h"
#include "memlayout.h"

void main() {
    int cpuid = mycpu()->hartid;
    printf("main: CPU%d start kernel initialization\n", cpuid);

    // 仅CPU0初始化核心模块
    if (cpuid == 0) {
        // 核心模块初始化
        pmem_init();          // 物理内存初始化
        kvm_init();           // 内核虚拟内存初始化
        kvm_inithart();       // 内核页表生效
        mmap_init();          // mmap区域池初始化
        
        // 启动首个进程（proc_make_first已包含initcode加载）
        printf("main: start first process (PID=0)\n");
        proc_make_first();
    }

    // 其他CPU核心空闲
    while (1) {
        asm volatile("wfi");
    }
}