// start.c 中的启动函数
#include "riscv.h"
#include "common.h"
#include "lib/print.h"  // 引入打印函数
#include "dev/uart.h"  // 引入 UART 初始化函数

// 为每个CPU核心分配4KB栈空间（NCPU=1表示单核心配置）
// aligned (16) 确保栈地址按16字节对齐（满足RISC-V调用规范）
__attribute__ ((aligned (16))) uint8 CPU_stack[4096 * NCPU];

int main();  // 声明main函数（内核主逻辑入口）

/**
 * 系统启动入口函数
 * 负责初始化硬件和执行环境，最终跳转到main函数
 */
void start() {
    // 1. 初始化核心ID识别（多核关键逻辑）
    uint64 hartid = r_mhartid();  // 读取当前核心的硬件ID（mhartid寄存器）

    if(hartid==0){
    w_tp(hartid);                 // 将硬件ID写入tp寄存器，供mycpuid()读取

    // 2. 基础硬件初始化
    w_mstatus(r_mstatus() & ~MSTATUS_MIE);  // 关闭机器模式中断（防止初始化被干扰）
    uart_init();                            // 初始化UART串口（打印输出依赖此硬件）

    // 3. 进入内核主逻辑
    main();  // 调用main函数执行核心业务逻辑

    // 4. 防止CPU跑飞（main正常情况下不会返回）
    while (1);
}
}

