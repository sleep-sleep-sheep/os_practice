#include "riscv.h"
#include "memlayout.h"
#include "dev/timer.h"

void main(void);

// 16字节对齐的 CPU 栈数组，每 CPU 4KB
__attribute__ ((aligned (16))) uint8 CPU_stack[4096 * NCPU];

// M-mode 下运行，完成权限配置后跳转到 main()
void start(void)
{
    // 1. 设置 mstatus.MPP = S-mode
    //    mret 后将进入 S-mode
    // MPP字段记录“返回到哪个模式”
    //先清零MPP字段，然后设置为S-mode的字段
    unsigned long x = r_mstatus();
    x &= ~MSTATUS_MPP_MASK;
    x |= MSTATUS_MPP_S;
    w_mstatus(x);
    
    // 2. 设置 mepc = main
    //    mret 后跳转到 main()，mret会跳转到mepc指向的地址
    w_mepc((uint64)main);
    
    // 3. 关闭分页
    w_satp(0);
    
    // 4. 委托所有中断和异常到 S-mode
    /*
    默认情况下，所有中断和异常都会陷入 M-mode 处理。
    委托后，某些中断/异常可以直接由 S-mode 处理，
    不需要经过 M-mode。
    */
    w_medeleg(0xffff);
    w_mideleg(0xffff);
    //  // 启用管理者模式的外部中断、定时器中断和软件中断
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);
    
    // 5. 配置 PMP，允许 S-mode 访问所有物理内存
    /*
    pmpaddr0
    设置保护区域的地址范围
    0x3fffffffffffff：覆盖几乎所有物理内存
    pmpcfg0
    设置保护区域的权限
    0xf = 1111：可读、可写、可执行、TOR 模式
    实际上是允许 S-mode 访问所有物理内存
    */
    w_pmpaddr0(0x3fffffffffffffull);
    w_pmpcfg0(0xf);
    
    // 6. 保存 hartid 到 tp 寄存器
    //    后续 mycpuid() 通过读取 tp 获取 CPU ID
    int id = r_mhartid();
    w_tp(id);
    
    // 7. 初始化M-mode定时器中断
    timer_init();
    
    // 8. mret: 跳转到 main()，同时切换到 S-mode
    asm volatile("mret");
}