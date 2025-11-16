#include "lib/print.h"
#include "mem/pmem.h"
#include "mem/kvm.h"
#include "trap-h/trap.h"  // 修正头文件路径（原trap-h/trap.h改为正确路径）
#include "proc/proc.h"
#include "riscv.h"
#include "dev/timer.h"
#include "dev/uart.h"
#include "dev/plic.h"  // 新增：PLIC初始化依赖

int main(void) {
    int id = mycpuid();
    if (id == 0) {  // 主核心初始化
        // 1. 初始化硬件
        timer_init();       // 初始化M-mode时钟（必须在开启M中断前）
        plic_init();        // 初始化PLIC控制器
        uart_init();        // 初始化UART（如果之前没初始化）

        // 2. 配置S-mode中断处理
        trap_kernel_init();        // 初始化S-mode时钟等
        trap_kernel_inithart();    // 设置stvec、开启SIE和SSIE

        // 3. 配置PLIC当前核心的中断路由
        plic_inithart();

        // 4. 最后开启M-mode全局中断（确保所有初始化完成）
        w_mstatus(r_mstatus() | MSTATUS_MIE);

        printf("Waiting for UART input... (type any character)\n");
        while (1) {
            __asm__ __volatile__("wfi");  // 等待中断
        }
    }
    return 0;
}