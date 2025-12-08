#include "lib/print.h"
#include "riscv.h"
#include "trap-h/trap.h"
#include "dev/uart.h"
#include "dev/plic.h"

int s_main()
{
    printf("[S] Entered S-mode main()\n");

    // 设置 S 态 trap 入口
    trap_kernel_inithart();

    // 打开 S-mode 中断
    w_sstatus(r_sstatus() | SSTATUS_SIE);
    w_sie(r_sie() | SIE_SSIE | SIE_SEIE | SIE_STIE);

    plic_inithart();   // 配置当前 hart 的 PLIC

    printf("[S] Waiting for interrupts...\n");

    while (1)
        asm volatile("wfi");

    return 0;
}
