#include "riscv.h"
#include "lib/print.h"
#include "dev/uart.h"
#include "dev/timer.h"
#include "dev/plic.h"
#include "mem/pmem.h"
#include "mem/vmem.h"
#include "mem/mmap.h"
#include "trap/trap.h"
#include "proc/proc.h"

volatile static int started = 0;

int main()
{
    int cpuid = r_tp();

    if(cpuid == 0) {
        // CPU 0 进行初始化
        print_init();
        pmem_init();
        kvm_init();
        trap_kernel_init();
        trap_kernel_inithart();
        kvm_inithart();
        plic_init();
        plic_inithart();
        uart_init();
        mmap_init();
        proc_init();
        intr_on();
        
        printf("\n");
        printf("  xv6-riscv Lab6 - Process Management\n");
        printf("========================================\n\n");
        
        __sync_synchronize();
        started = 1;
        
        // 创建第一个用户进程
        printf("[Debug] main: CPU %d creating first user process\n", cpuid);
        proc_make_first();
        
        printf("[Debug] main: CPU %d entering scheduler\n", cpuid);
        
        // 进入调度器，永不返回
        proc_scheduler();
        
        panic("scheduler returned");
        
    } else {
        // 其他CPU等待CPU 0初始化完成
        while(started == 0);
        __sync_synchronize();
        
        kvm_inithart();
        trap_kernel_inithart();
        plic_inithart();
        intr_on();
        
        printf("[Debug] main: CPU %d initialized, entering scheduler\n", cpuid);
        
        // 进入调度器
        proc_scheduler();
        
        panic("scheduler returned");
    }
    
    return 0;
}