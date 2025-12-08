#include "lib/print.h"
#include "mem/pmem.h"
#include "mem/kvm.h"
#include "trap-h/trap.h"  // 修正头文件路径（原trap-h/trap.h改为正确路径）
#include "proc/proc.h"
#include "riscv.h"
#include "dev/timer.h"
#include "dev/uart.h"
#include "dev/plic.h"  // 新增：PLIC初始化依赖

// start.c 中的 main 函数修改
void main() {
    int cpuid = mycpuid();
    cpu_t* c = mycpu();
    c->id = cpuid;

    // 只有CPU0初始化内核页表（全局唯一）
    if (cpuid == 0) {
        kvm_init();  // 创建内核页表并完成所有映射（代码、数据、设备、跳板页、内核栈）
    }

    // 所有CPU核心启用虚拟内存（设置satp）
    kvm_inithart();  // 每个CPU都要执行，启用自身MMU

    // 初始化陷阱向量表（用户态trap指向user_vector）
    w_stvec((uint64)user_vector);

    // CPU0创建首进程，其他CPU空闲
    if (cpuid == 0) {
        printf("CPU%d: starting first process proczero...\n", cpuid);
        proc_make_first();
    } else {
        printf("CPU%d: entering idle loop...\n", cpuid);
        while (1);
    }
}